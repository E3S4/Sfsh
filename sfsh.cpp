#include <bits/stdc++.h>
#include <readline/readline.h>
#include <readline/history.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <sys/wait.h>
#include <pwd.h>
#include <signal.h>
#include <fcntl.h>

using namespace std;

struct Job {
    pid_t pid;
    string cmdline;
    bool running;
};

static vector<Job> jobs;
vector<string> commands; // executables in PATH
unordered_map<string,string> aliases;

vector<string> executables_in_path(const string &path) {
    vector<string> executables;
    stringstream ss(path);
    string dir;
    struct dirent *ent;
    struct stat st;

    while (getline(ss, dir, ':')) {
        DIR *d = opendir(dir.c_str());
        if (!d) continue;
        while ((ent = readdir(d))) {
            string name = ent->d_name;
            string full = dir + "/" + name;
            if (stat(full.c_str(), &st) == 0 && (st.st_mode & S_IXUSR))
                executables.push_back(name);
        }
        closedir(d);
    }
    return executables;
}

void init_commands() {
    const char *path = getenv("PATH");
    if (!path) return;
    commands = executables_in_path(path);
    sort(commands.begin(), commands.end());
    commands.erase(unique(commands.begin(), commands.end()), commands.end());
}

// Read ~/.sfhsrc for simple alias config: alias ll=ls -la
void load_config() {
    string home = getenv("HOME") ? string(getenv("HOME")) : string();
    string file = home + "/.sfhsrc";
    FILE *f = fopen(file.c_str(), "r");
    if (!f) return;
    char *line = NULL;
    size_t len = 0;
    while (getline(&line, &len, f) != -1) {
        string s(line);
        // trim
        while (!s.empty() && isspace(s.back())) s.pop_back();
        while (!s.empty() && isspace(s.front())) s.erase(s.begin());
        if (s.rfind("alias ", 0) == 0) {
            string rest = s.substr(6);
            auto eq = rest.find('=');
            if (eq != string::npos) {
                string name = rest.substr(0, eq);
                string val = rest.substr(eq+1);
                // remove quotes
                if (!val.empty() && (val.front()=='\''||val.front()=='\"')) val.erase(0,1);
                if (!val.empty() && (val.back()=='\''||val.back()=='\"')) val.pop_back();
                // trim
                while (!name.empty() && isspace(name.back())) name.pop_back();
                while (!name.empty() && isspace(name.front())) name.erase(name.begin());
                aliases[name] = val;
            }
        }
    }
    if (line) free(line);
    fclose(f);
}

// Split by whitespace but respecting simple quotes
vector<string> tokenize(const string &s) {
    vector<string> out;
    string cur;
    bool in_s=false, in_d=false;
    for (size_t i=0;i<s.size();++i) {
        char c = s[i];
        if (c=='\'' && !in_d) { in_s = !in_s; continue; }
        if (c=='\"' && !in_s) { in_d = !in_d; continue; }
        if (isspace((unsigned char)c) && !in_s && !in_d) {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
        } else cur.push_back(c);
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// Expand aliases for first token
void expand_aliases(vector<string> &args) {
    if (args.empty()) return;
    auto it = aliases.find(args[0]);
    if (it!=aliases.end()) {
        string repl = it->second;
        vector<string> rtok = tokenize(repl);
        vector<string> newargs;
        for (auto &x: rtok) newargs.push_back(x);
        for (size_t i=1;i<args.size();++i) newargs.push_back(args[i]);
        args.swap(newargs);
    }
}

// Signal handler to reap children and update jobs
void sigchld_handler(int) {
    int saved_errno = errno;
    while (true) {
        int status;
        pid_t pid = waitpid(-1, &status, WNOHANG);
        if (pid <= 0) break;
        for (auto &j: jobs) if (j.pid==pid) j.running = false;
    }
    errno = saved_errno;
}

// Colorful prompt with username@host:cwd$
string build_prompt() {
    char host[128]; gethostname(host, sizeof(host));
    struct passwd *pw = getpwuid(getuid());
    string user = pw ? pw->pw_name : string("user");
    char cwd[1024]; getcwd(cwd, sizeof(cwd));
    string home = getenv("HOME") ? string(getenv("HOME")) : string();
    string display(cwd);
    if (!home.empty() && display.rfind(home,0)==0) display = string("~") + display.substr(home.size());
    // ANSI colors
    string u = "\x1b[1;32m" + user + "\x1b[0m"; // green
    string h = "\x1b[1;34m" + string(host) + "\x1b[0m"; // blue
    string p = "\x1b[1;33m" + display + "\x1b[0m"; // yellow
    return u + "@" + h + ":" + p + "$ ";
}

// Execute pipeline of commands (with optional redirs), background flag
void execute_pipeline(vector<vector<string>> &cmds, bool background, vector<pair<int,int>> &redir) {
    size_t n = cmds.size();
    vector<int> pipes_fds;
    for (size_t i=0;i+1<n;++i) {
        int fds[2]; if (pipe(fds)!=0) { perror("pipe"); return; }
        pipes_fds.push_back(fds[0]); pipes_fds.push_back(fds[1]);
    }

    vector<pid_t> pids;
    for (size_t i=0;i<n;++i) {
        pid_t pid = fork();
        if (pid==0) {
            // child
            // set up input from previous pipe
            if (i>0) {
                int in_fd = pipes_fds[(i-1)*2];
                dup2(in_fd, STDIN_FILENO);
            }
            // set up output to next pipe
            if (i+1<n) {
                int out_fd = pipes_fds[i*2+1];
                dup2(out_fd, STDOUT_FILENO);
            }
            // apply redirections if present for this command
            if (!redir.empty() && redir.size()>= (int)(i+1)) {
                int inf = redir[i].first;
                int outf = redir[i].second;
                if (inf!=-1) {
                    int fd = open(cmds[i][inf].c_str(), O_RDONLY);
                    if (fd==-1) { perror("open"); exit(EXIT_FAILURE); }
                    dup2(fd, STDIN_FILENO);
                    close(fd);
                }
                if (outf!=-1) {
                    int fd = open(cmds[i][outf].c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
                    if (fd==-1) { perror("open"); exit(EXIT_FAILURE); }
                    dup2(fd, STDOUT_FILENO);
                    close(fd);
                }
            }
            // close all pipe fds in child
            for (int fd: pipes_fds) close(fd);
            // build args
            vector<char*> cargs;
            for (auto &a: cmds[i]) cargs.push_back(const_cast<char*>(a.c_str()));
            cargs.push_back(nullptr);
            execvp(cargs[0], cargs.data());
            perror("execvp");
            exit(EXIT_FAILURE);
        } else if (pid>0) {
            pids.push_back(pid);
        } else {
            perror("fork");
            // close fds and cleanup
            for (int fd: pipes_fds) close(fd);
            return;
        }
    }
    // parent: close pipes
    for (int fd: pipes_fds) close(fd);

    // track background jobs
    string fullcmd;
    for (size_t i=0;i<n;++i) {
        for (auto &t: cmds[i]) fullcmd += t + ((i+1<n)?" | ":"");
    }

    if (background) {
        for (auto pid: pids) jobs.push_back({pid, fullcmd, true});
        cout << "[bg] " << pids.back() << " " << fullcmd << "\n";
    } else {
        for (auto pid: pids) {
            int status; waitpid(pid, &status, 0);
        }
    }
}

// Parse one full command line: support |, >, <, &
void handle_line(const string &line) {
    string s = line;
    // split by | first
    bool background = false;
    string trimmed = s;
    while (!trimmed.empty() && isspace(trimmed.back())) trimmed.pop_back();
    if (!trimmed.empty() && trimmed.back()=='&') {
        background = true;
        trimmed.pop_back();
    }
    vector<string> pipe_parts;
    string cur;
    bool in_s=false,in_d=false;
    for (size_t i=0;i<trimmed.size();++i) {
        char c = trimmed[i];
        if (c=='\'' && !in_d) { in_s = !in_s; cur.push_back(c); continue; }
        if (c=='\"' && !in_s) { in_d = !in_d; cur.push_back(c); continue; }
        if (c=='|' && !in_s && !in_d) { pipe_parts.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    if (!cur.empty()) pipe_parts.push_back(cur);

    vector<vector<string>> cmds;
    vector<pair<int,int>> redirs; // pair: index-of-infile-in-args or -1, index-of-outfile-in-args or -1

    for (auto &part: pipe_parts) {
        vector<string> args = tokenize(part);
        if (args.empty()) continue;
        expand_aliases(args);
        int inf=-1, outf=-1;
        // scan for > and < and remove them, keep filename token index
        for (int i=0;i<(int)args.size();++i) {
            if (args[i]==">" && i+1<(int)args.size()) { outf = i+1; args.erase(args.begin()+i); args.erase(args.begin()+i); i--; }
            else if (args[i]=="<" && i+1<(int)args.size()) { inf = i+1; args.erase(args.begin()+i); args.erase(args.begin()+i); i--; }
        }
        cmds.push_back(args);
        redirs.push_back({inf,outf});
    }

    if (cmds.empty()) return;

    // builtins: cd, exit, jobs, help, fg
    if (cmds.size()==1) {
        auto &a = cmds[0];
        if (a.empty()) return;
        if (a[0]=="exit") exit(0);
        if (a[0]=="cd") {
            const char *path = a.size()>1 ? a[1].c_str() : getenv("HOME");
            if (chdir(path)!=0) perror("cd");
            return;
        }
        if (a[0]=="jobs") {
            for (size_t i=0;i<jobs.size();++i) {
                cout << "["<<i<<"] "<<(jobs[i].running?"Running":"Done")<<" "<<jobs[i].pid<<" "<<jobs[i].cmdline<<"\n";
            }
            return;
        }
        if (a[0]=="fg") {
            if (a.size()<2) { cerr<<"fg <jobid>\n"; return; }
            int id = stoi(a[1]);
            if (id<0 || id>=(int)jobs.size()) { cerr<<"invalid job\n"; return; }
            pid_t pid = jobs[id].pid;
            int status; waitpid(pid, &status, 0);
            jobs[id].running = false;
            return;
        }
    }

    execute_pipeline(cmds, background, redirs);
}

// Readline completion: minimal, uses PATH and files
char *completion_generator(const char *text, int state) {
    static size_t list_index, len;
    static vector<string> matches;

    if (!state) {
        list_index = 0; len = strlen(text); matches.clear();
        vector<string> internal = {"cd","exit","help","jobs","fg"};
        for (auto &cmd: internal) if (cmd.compare(0,len,text)==0) matches.push_back(cmd);
        for (auto &cmd: commands) if (cmd.compare(0,len,text)==0) matches.push_back(cmd);
        // files
        string prefix(text);
        string dir = ".";
        if (strchr(text,'/')) dir = string(text, strrchr(text,'/')-text);
        DIR *d = opendir(dir.c_str());
        if (d) {
            struct dirent *ent;
            struct stat st;
            while ((ent = readdir(d))) {
                string name = ent->d_name;
                string full = dir + "/" + name;
                if (stat(full.c_str(), &st)==0 && S_ISDIR(st.st_mode)) name += "/";
                if (name.compare(0,len,text)==0) matches.push_back(name);
            }
            closedir(d);
        }
    }
    if (list_index < matches.size()) return strdup(matches[list_index++].c_str());
    return nullptr;
}

char **completer(const char *text, int start, int end) {
    rl_attempted_completion_over = 1;
    return rl_completion_matches(text, completion_generator);
}

int main() {
    signal(SIGCHLD, sigchld_handler);
    rl_attempted_completion_function = completer;
    init_commands();
    load_config();

    while (true) {
        string prompt = build_prompt();
        char *input = readline(prompt.c_str());
        if (!input) break;
        if (strlen(input)==0) { free(input); continue; }
        add_history(input);
        string command(input);
        free(input);
        handle_line(command);
    }
    return 0;
}

