// minimal, fast-ish Unix-like shell in C++ (single-file).
// features: exec, pipes, <, >, >>, background (&), builtins: cd, exit, jobs, fg, help

#include <bits/stdc++.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <termios.h>

using namespace std;

// Job tracking
struct Job {
    pid_t pid;
    string cmdline;
    bool running;
    bool background;
};
static vector<Job> jobs;
static int shell_terminal = 0;
static pid_t shell_pgid = 0;

// Trim helpers
static inline string trim(const string &s){
    size_t a = s.find_first_not_of(" \t\n\r");
    if(a==string::npos) return "";
    size_t b = s.find_last_not_of(" \t\n\r");
    return s.substr(a, b-a+1);
}

// Split a command line into tokens with basic quote handling
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

// Command structure for pipeline segments
struct Cmd {
    vector<string> argv;
    string infile;
    string outfile;
    bool append = false;
};

// Parse tokens into pipeline of Cmd objects, and detect background (&)
bool parse_line(const string &line, vector<Cmd> &pipeline, bool &background){
    pipeline.clear();
    background = false;
    auto toks = tokenize(line);
    if(toks.empty()) return false;

    // Check trailing &
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
            if(cur.argv.empty()) return false; // invalid
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

// Convert vector<string> to char* array for execvp
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

// Builtins
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
        // wait for it
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

// Signal handlers
void sigchld_handler(int){
    // Reap any children to avoid zombies; update jobs
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
    // forward ctrl-c to foreground group
    // Using default behavior: print newline
    cerr << "\n";
}

// Execute a pipeline (1..N commands). If background==true, don't wait
int execute_pipeline(vector<Cmd> &pipeline, bool background, const string &orig_line){
    size_t n = pipeline.size();
    vector<int> pfd(2*n); // store pipe fds stashed pairs optionally
    // create pipes
    vector<array<int,2>> pipes;
    for(size_t i=0;i+1<n;++i){
        int fds[2];
        if(pipe(fds) == -1){ perror("pipe"); return -1; }
        pipes.push_back({fds[0], fds[1]});
    }

    vector<pid_t> pids;
    for(size_t i=0;i<n;++i){
        pid_t pid = fork();
        if(pid<0){ perror("fork"); return -1; }
        if(pid==0){
            // child process
            // put child in its own process group for better signal behavior
            setpgid(0, 0);
            // if not first, set stdin from previous pipe
            if(i>0){
                dup2(pipes[i-1][0], STDIN_FILENO);
            }
            // if not last, set stdout to next pipe
            if(i+1<n){
                dup2(pipes[i][1], STDOUT_FILENO);
            }
            // handle infile/outfile for this segment
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
            // close all pipe fds in child
            for(auto &pp : pipes){ close(pp[0]); close(pp[1]); }

            // exec
            char **argv = vec_to_cstrs(pipeline[i].argv);
            execvp(argv[0], argv);
            // if exec fails:
            perror("execvp");
            free_cstrs(argv);
            _exit(127);
        } else {
            // parent: record pid
            setpgid(pid, pid); // set group id = pid
            pids.push_back(pid);
        }
    }

    // parent closes all pipe fds
    for(auto &pp : pipes){ close(pp[0]); close(pp[1]); }

    // If background, add to jobs list and don't wait
    if(background){
        Job j;
        j.pid = pids[0]; // store first process pid as job id
        j.cmdline = orig_line;
        j.running = true;
        j.background = true;
        jobs.push_back(j);
        cout << "[" << jobs.size() << "] " << j.pid << "\n";
    } else {
        // Wait for last process (synchronous)
        int status = 0;
        // Wait for each child in turn
        for(pid_t pid : pids){
            if(waitpid(pid, &status, 0) < 0) perror("waitpid");
        }
    }

    return 0;
}

int main(){
    // Basic shell setup: signals
    signal(SIGCHLD, sigchld_handler);
    signal(SIGINT, sigint_handler);

    // Save shell PGID / terminal settings (simple)
    shell_terminal = STDIN_FILENO;
    shell_pgid = getpid();
    setpgid(shell_pgid, shell_pgid);

    string line;
    bool exit_shell = false;
    while(!exit_shell){
        // Prompt
        char cwd[1024];
        if(getcwd(cwd, sizeof(cwd))==nullptr) strcpy(cwd, "?");
        cout << "\033[1;32mfshell\033[0m:\033[1;34m" << cwd << "\033[0m$ " << flush;

        if(!std::getline(cin, line)){
            cout << "\n";
            break; // EOF (Ctrl-D)
        }
        line = trim(line);
        if(line.empty()) continue;

        // parse
        vector<Cmd> pipeline;
        bool background=false;
        if(!parse_line(line, pipeline, background)){
            cerr << "parse error\n";
            continue;
        }

        // single built-in short-circuit (only when single command and not background and no redirection/pipes)
        if(pipeline.size()==1 && is_builtin(pipeline[0]) && !background &&
           pipeline[0].infile.empty() && pipeline[0].outfile.empty()){
            run_builtin(pipeline[0], exit_shell);
            continue;
        }

        // If single and builtin but with redir or bg, we still fork and run run_builtin in child? Simpler: handle cd & exit locally only above.
        // execute pipeline
        execute_pipeline(pipeline, background, line);
    }

    return 0;
}

