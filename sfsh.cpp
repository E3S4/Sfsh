// minimal, fast-ish Unix-like shell in C++ (single-file).
// features: exec, pipes, <, >, >>, background (&), builtins: cd, exit, jobs, fg, help

#include <bits/stdc++.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <termios.h>

using namespace std;

struct Job {
    pid_t pid;
    string cmdline;
    bool running;
    bool background;
};
static vector<Job> jobs;
static int shell_terminal = 0;
static pid_t shell_pgid = 0;

static inline string trim(const string &s){
    size_t a = s.find_first_not_of(" \t\n\r");
    if(a==string::npos) return "";
    size_t b = s.find_last_not_of(" \t\n\r");
    return s.substr(a, b-a+1);
}

vector<string> tokenize(const string &line){
    vector<string> toks;
    string cur;
    bool in_squote = false, in_dquote = false;
    for(size_t i=0;i<line.size();++i){
        char c = line[i];
        if(c=='\'' && !in_dquote){
            in_squote = !in_squote;
            continue;
        }
        if(c=='"' && !in_squote){
            in_dquote = !in_dquote;
            continue;
        }
        if(!in_squote && !in_dquote && isspace((unsigned char)c)){
            if(!cur.empty()){ toks.push_back(cur); cur.clear(); }
        } else {
            cur.push_back(c);
        }
    }
    if(!cur.empty()) toks.push_back(cur);
    return toks;
}

struct Cmd {
    vector<string> argv;
    string infile;
    string outfile;
    bool append = false;
};

bool parse_line(const string &line, vector<Cmd> &pipeline, bool &background){
    pipeline.clear();
    background = false;
    auto toks = tokenize(line);
    if(toks.empty()) return false;
    if(toks.back()=="&"){
        background = true;
        toks.pop_back();
        if(toks.empty()) return false;
    }
    Cmd cur;
    size_t i=0;
    while(i<toks.size()){
        string t = toks[i];
        if(t=="|"){
            if(cur.argv.empty()) return false;
            pipeline.push_back(cur);
            cur = Cmd();
            ++i;
            continue;
        } else if(t=="<"){
            if(i+1>=toks.size()) return false;
            cur.infile = toks[i+1];
            i += 2;
            continue;
        } else if(t==">" || t==">>"){
            if(i+1>=toks.size()) return false;
            cur.outfile = toks[i+1];
            cur.append = (t==">>");
            i += 2;
            continue;
        } else {
            cur.argv.push_back(t);
            ++i;
        }
    }
    if(!cur.argv.empty()) pipeline.push_back(cur);
    return !pipeline.empty();
}

char** vec_to_cstrs(const vector<string> &v){
    char **arr = (char**)calloc(v.size()+1, sizeof(char*));
    for(size_t i=0;i<v.size();++i) arr[i] = strdup(v[i].c_str());
    arr[v.size()] = nullptr;
    return arr;
}
void free_cstrs(char **arr){
    if(!arr) return;
    for(size_t i=0; arr[i]; ++i) free(arr[i]);
    free(arr);
}

bool is_builtin(const Cmd &c){
    if(c.argv.empty()) return false;
    string cmd = c.argv[0];
    return (cmd=="cd" || cmd=="exit" || cmd=="jobs" || cmd=="fg" || cmd=="help");
}
int run_builtin(Cmd &c, bool &exit_shell){
    string cmd = c.argv[0];
    if(cmd=="cd"){
        string dir = (c.argv.size()>=2 ? c.argv[1] : getenv("HOME"));
        if(chdir(dir.c_str())!=0){
            perror("cd");
            return -1;
        }
        return 0;
    } else if(cmd=="exit"){
        exit_shell = true;
        return 0;
    } else if(cmd=="jobs"){
        for(size_t i=0;i<jobs.size();++i){
            cout << "[" << i+1 << "] pid=" << jobs[i].pid
                 << (jobs[i].running ? " Running " : " Done ")
                 << (jobs[i].background ? "bg " : "fg ")
                 << jobs[i].cmdline << "\n";
        }
        return 0;
    } else if(cmd=="fg"){
        if(c.argv.size()<2){
            cerr << "fg: usage: fg <jobnum>\n";
            return -1;
        }
        int j = stoi(c.argv[1]) - 1;
        if(j<0 || (size_t)j>=jobs.size()){ cerr<<"fg: no such job\n"; return -1; }
        pid_t pid = jobs[j].pid;
        jobs[j].background = false;
        int status;
        if(waitpid(pid, &status, 0) < 0) perror("waitpid");
        return 0;
    } else if(cmd=="help"){
        cout << "fastshell: builtins: cd, exit, jobs, fg, help\n"
             << "supports pipes |, redir < > >>, background &\n";
        return 0;
    }
    return -1;
}

void sigchld_handler(int){
    int status; pid_t pid;
    while((pid = waitpid(-1, &status, WNOHANG)) > 0){
        for(auto &j : jobs){
            if(j.pid == pid){
                j.running = false;
                if(j.background){
                    cerr << "\n[bg done] pid="<<pid<<" cmd="<<j.cmdline<<"\n";
                }
                break;
            }
        }
    }
}
void sigint_handler(int){
    cerr << "\n";
}

int execute_pipeline(vector<Cmd> &pipeline, bool background, const string &orig_line){
    size_t n = pipeline.size();
    vector<array<int,2>> pipes;
    for(size_t i=0;i+1<n;++i){
        int fds[2];
        if(pipe(fds) == -1){ perror("pipe"); return -1; }
        pipes.push_back({fds[0], fds[1]});
    }
    vector<pid_t> pids;
    pid_t first_child_pgid = 0;
    for(size_t i=0;i<n;++i){
        pid_t pid = fork();
        if(pid<0){ perror("fork"); return -1; }
        if(pid==0){
            setpgid(0, 0);
            signal(SIGINT, SIG_DFL);
            signal(SIGQUIT, SIG_DFL);
            signal(SIGTSTP, SIG_DFL);
            signal(SIGTTIN, SIG_DFL);
            signal(SIGTTOU, SIG_DFL);
            signal(SIGCHLD, SIG_DFL);
            if(i>0) dup2(pipes[i-1][0], STDIN_FILENO);
            if(i+1<n) dup2(pipes[i][1], STDOUT_FILENO);
            if(!pipeline[i].infile.empty()){
                int fd = open(pipeline[i].infile.c_str(), O_RDONLY);
                if(fd<0){ perror("open infile"); _exit(1); }
                dup2(fd, STDIN_FILENO);
                close(fd);
            }
            if(!pipeline[i].outfile.empty()){
                int flags = O_WRONLY | O_CREAT | (pipeline[i].append ? O_APPEND : O_TRUNC);
                int fd = open(pipeline[i].outfile.c_str(), flags, 0644);
                if(fd<0){ perror("open outfile"); _exit(1); }
                dup2(fd, STDOUT_FILENO);
                close(fd);
            }
            for(auto &pp : pipes){ close(pp[0]); close(pp[1]); }
            if(is_builtin(pipeline[i])){
                bool exit_shell_local = false;
                run_builtin(pipeline[i], exit_shell_local);
                _exit(0);
            }
            char **argv = vec_to_cstrs(pipeline[i].argv);
            execvp(argv[0], argv);
            perror("execvp");
            free_cstrs(argv);
            _exit(127);
        } else {
            setpgid(pid, pid);
            if(first_child_pgid == 0) first_child_pgid = pid;
            pids.push_back(pid);
        }
    }
    for(auto &pp : pipes){ close(pp[0]); close(pp[1]); }
    if(!background){
        struct sigaction sa_ignore{}, sa_old_ttou, sa_old_ttin;
        sa_ignore.sa_handler = SIG_IGN;
        sigemptyset(&sa_ignore.sa_mask);
        sa_ignore.sa_flags = 0;
        sigaction(SIGTTOU, &sa_ignore, &sa_old_ttou);
        sigaction(SIGTTIN, &sa_ignore, &sa_old_ttin);
        if(tcsetpgrp(shell_terminal, first_child_pgid) == -1){}
        sigaction(SIGTTOU, &sa_old_ttou, nullptr);
        sigaction(SIGTTIN, &sa_old_ttin, nullptr);
    }
    if(background){
        Job j;
        j.pid = pids[0];
        j.cmdline = orig_line;
        j.running = true;
        j.background = true;
        jobs.push_back(j);
        cout << "[" << jobs.size() << "] " << j.pid << "\n";
    } else {
        int status;
        for(pid_t pid : pids){
            if(waitpid(pid, &status, 0) < 0) perror("waitpid");
        }
        if(tcsetpgrp(shell_terminal, shell_pgid) == -1){}
    }
    return 0;
}

int main(){
    signal(SIGCHLD, sigchld_handler);
    signal(SIGINT, sigint_handler);
    shell_terminal = STDIN_FILENO;
    shell_pgid = getpid();
    setpgid(shell_pgid, shell_pgid);
    tcsetpgrp(shell_terminal, shell_pgid);
    signal(SIGTTOU, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    ios::sync_with_stdio(false);
    cin.tie(nullptr);
    string line;
    bool exit_shell = false;
    while(!exit_shell){
        char cwd[1024];
        if(getcwd(cwd, sizeof(cwd))==nullptr) strcpy(cwd, "?");
        cout << "\033[1;32mfshell\033[0m:\033[1;34m" << cwd << "\033[0m$ " << flush;
        if(!std::getline(cin, line)){
            cout << "\n";
            break;
        }
        line = trim(line);
        if(line.empty()) continue;
        vector<Cmd> pipeline;
        bool background=false;
        if(!parse_line(line, pipeline, background)){
            cerr << "parse error\n";
            continue;
        }
        if(pipeline.size()==1 && is_builtin(pipeline[0]) && !background &&
           pipeline[0].infile.empty() && pipeline[0].outfile.empty()){
            run_builtin(pipeline[0], exit_shell);
            continue;
        }
        execute_pipeline(pipeline, background, line);
    }
    return 0;
}

