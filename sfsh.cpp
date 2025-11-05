// fshell.cpp
// Optimized single-file shell: exec, pipes, <, >, >>, &, builtins: cd, exit, jobs, fg, help
// Readline tab-completion (builtins, filenames, executables) + simple syntax highlight preview
#include <bits/stdc++.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <termios.h>
#include <readline/readline.h>
#include <readline/history.h>
using namespace std;

struct Job { pid_t pid; string cmdline; bool running; bool background; };
static vector<Job> jobs;
static pid_t shell_pgid = 0;
static int shell_terminal = 0;

static inline string trim(const string &s){
    size_t a = s.find_first_not_of(" \t\n\r");
    if (a==string::npos) return "";
    size_t b = s.find_last_not_of(" \t\n\r");
    return s.substr(a, b-a+1);
}

vector<string> tokenize(const string &line) {
    vector<string> toks;
    string cur;
    bool in_squote=false, in_dquote=false;
    for (size_t i=0;i<line.size();++i){
        char c=line[i];
        if (c=='\'' && !in_dquote){ in_squote=!in_squote; continue; }
        if (c=='"' && !in_squote){ in_dquote=!in_dquote; continue; }
        if (!in_squote && !in_dquote && isspace((unsigned char)c)){
            if(!cur.empty()){
                if(cur[0]=='~' && (cur.size()==1 || cur[1]=='/')){
                    const char* h=getenv("HOME");
                    if(h) cur = string(h)+cur.substr(1);
                }
                toks.push_back(cur); cur.clear();
            }
        } else cur.push_back(c);
    }
    if(!cur.empty()){
        if(cur[0]=='~' && (cur.size()==1 || cur[1]=='/')){
            const char* h=getenv("HOME");
            if(h) cur = string(h)+cur.substr(1);
        }
        toks.push_back(cur);
    }
    return toks;
}

struct Cmd { vector<string> argv; string infile, outfile; bool append=false; };

bool parse_line(const string &line, vector<Cmd> &pipeline, bool &background){
    pipeline.clear(); background=false;
    auto toks = tokenize(line);
    if(toks.empty()) return false;
    if(toks.back()=="&"){ background=true; toks.pop_back(); if(toks.empty()) return false; }
    Cmd cur;
    for(size_t i=0;i<toks.size();){
        string t=toks[i];
        if(t=="|"){ if(cur.argv.empty()) return false; pipeline.push_back(cur); cur=Cmd(); ++i; }
        else if(t=="<"){ if(i+1>=toks.size()) return false; cur.infile=toks[++i]; ++i; }
        else if(t==">"||t==">>"){ if(i+1>=toks.size()) return false; cur.outfile=toks[++i]; cur.append=(t==">>"); ++i; }
        else { cur.argv.push_back(t); ++i; }
    }
    if(!cur.argv.empty()) pipeline.push_back(cur);
    return !pipeline.empty();
}

inline char** vec_to_cstrs(const vector<string> &v){
    char** a = new char*[v.size()+1];
    for(size_t i=0;i<v.size();++i) a[i]=strdup(v[i].c_str());
    a[v.size()] = nullptr;
    return a;
}
inline void free_cstrs(char **a){
    if(!a) return;
    for(size_t i=0;a[i];++i) free(a[i]);
    delete[] a;
}

bool is_builtin(const Cmd &c){
    if(c.argv.empty()) return false;
    string cmd=c.argv[0];
    return (cmd=="cd"||cmd=="exit"||cmd=="jobs"||cmd=="fg"||cmd=="help");
}

int run_builtin(Cmd &c, bool &exit_shell){
    string cmd = c.argv[0];
    if(cmd=="cd"){ string dir=(c.argv.size()>=2?c.argv[1]:getenv("HOME")); if(chdir(dir.c_str())!=0) perror("cd"); return 0; }
    if(cmd=="exit"){ exit_shell=true; return 0; }
    if(cmd=="jobs"){ for(size_t i=0;i<jobs.size();++i) cout<<"["<<i+1<<"] pid="<<jobs[i].pid<<(jobs[i].running?" Running ":" Done ")<<(jobs[i].background?"bg ":"fg ")<<jobs[i].cmdline<<"\n"; return 0; }
    if(cmd=="fg"){ if(c.argv.size()<2){ cerr<<"fg: usage: fg <jobnum>\n"; return -1; } int j=stoi(c.argv[1])-1; if(j<0|| (size_t)j>=jobs.size()){ cerr<<"fg: no such job\n"; return -1; } pid_t pid=jobs[j].pid; jobs[j].background=false; int status; waitpid(pid,&status,0); return 0; }
    if(cmd=="help"){ cout<<"builtins: cd, exit, jobs, fg, help\n"; return 0; }
    return -1;
}

void sigchld_handler(int){ int status; pid_t pid; while((pid=waitpid(-1,&status,WNOHANG))>0) for(auto &j:jobs) if(j.pid==pid) j.running=false; }
void sigint_handler(int){ cerr<<"\n"; }

static vector<string> builtin_list = {"cd","exit","jobs","fg","help"};

static bool file_executable(const string &path){
    return access(path.c_str(), X_OK)==0 && !access(path.c_str(), F_OK);
}

static vector<string> executables_in_path(const string &prefix){
    vector<string> out;
    const char* p = getenv("PATH");
    if(!p) return out;
    stringstream ss(p);
    string dir;
    while(getline(ss, dir, ':')){
        DIR *d = opendir(dir.c_str());
        if(!d) continue;
        struct dirent *ent;
        while((ent = readdir(d))){
            string name = ent->d_name;
            if(name.size() >= prefix.size() && name.rfind(prefix,0)==0){
                string full = dir + "/" + name;
                if(file_executable(full)) out.push_back(name);
            }
        }
        closedir(d);
    }
    sort(out.begin(), out.end());
    out.erase(unique(out.begin(), out.end()), out.end());
    return out;
}

static char *completion_generator(const char *text, int state){
    static vector<string> matches;
    static size_t idx;
    if(state==0){
        matches.clear(); idx=0;
        string s(text);
        // if word 0 (first token) then include builtins and PATH executables, else file completion
        int point = rl_point;
        // find start of current word
        int start = point-1;
        while(start>=0 && !isspace((unsigned char)rl_line_buffer[start])) start--;
        start++;
        string prefix = s;
        // first token?
        bool first_token = true;
        for(int i=0;i<start;i++) if(!isspace((unsigned char)rl_line_buffer[i])) { first_token=false; break; }
        if(first_token){
            for(auto &b: builtin_list) if(b.rfind(prefix,0)==0) matches.push_back(b);
            auto ex = executables_in_path(prefix);
            for(auto &e: ex) matches.push_back(e);
        } else {
            // filename completion: use globbing via opendir of current dir and also consider relative paths
            string pathprefix = string(prefix);
            string dir = ".";
            string base = pathprefix;
            size_t pos = pathprefix.find_last_of('/');
            if(pos!=string::npos){ dir = pathprefix.substr(0,pos); base = pathprefix.substr(pos+1); }
            DIR *d = opendir(dir.c_str());
            if(d){
                struct dirent *ent;
                while((ent = readdir(d))){
                    string name = ent->d_name;
                    if(name.rfind(base,0)==0){
                        string full = (dir==".")? name : dir + "/" + name;
                        struct stat st;
                        if(stat(full.c_str(), &st)==0 && S_ISDIR(st.st_mode)) matches.push_back(full + "/");
                        else matches.push_back(full);
                    }
                }
                closedir(d);
            }
        }
        sort(matches.begin(), matches.end());
        matches.erase(unique(matches.begin(), matches.end()), matches.end());
    }
    if(idx < matches.size()) {
        return strdup(matches[idx++].c_str());
    }
    return nullptr;
}

static char **fshell_completion(const char *text, int start, int end){
    rl_attempted_completion_over = 1;
    return rl_completion_matches(text, completion_generator);
}

static string colorize_preview(const string &line){
    auto toks = tokenize(line);
    string out;
    for(size_t i=0;i<toks.size();++i){
        string t=toks[i];
        if(i==0 && (find(builtin_list.begin(), builtin_list.end(), t)!=builtin_list.end())){
            out += "\033[1;33m" + t + "\033[0m"; // builtin: yellow
        } else if((t.size()>0 && (t[0]=='\'' || t[0]=='\"')) || (t.find(' ')!=string::npos)){
            out += "\033[1;32m" + t + "\033[0m"; // string: green
        } else if(t=="|"||t=="<"||t==">"||t==">>"){
            out += "\033[1;35m" + t + "\033[0m"; // operator: magenta
        } else {
            // executable? file? highlight executable-like
            if(i==0){
                // check PATH
                auto exs = executables_in_path(t);
                if(!exs.empty() || access(t.c_str(), X_OK)==0) out += "\033[1;36m" + t + "\033[0m"; // cyan
                else out += t;
            } else out += t;
        }
        if(i+1<toks.size()) out += " ";
    }
    return out;
}

int execute_pipeline(vector<Cmd> &pipeline, bool background, const string &orig_line){
    size_t n = pipeline.size();
    vector<array<int,2>> pipes(n>1? n-1:0);
    for(size_t i=0;i+1<n;++i) if(pipe(pipes[i].data())==-1) return perror("pipe"), -1;
    vector<pid_t> pids(n);
    pid_t first_child_pgid = 0;
    for(size_t i=0;i<n;++i){
        pid_t pid = fork();
        if(pid<0) return perror("fork"), -1;
        if(pid==0){
            if(i>0) dup2(pipes[i-1][0], STDIN_FILENO);
            if(i+1<n) dup2(pipes[i][1], STDOUT_FILENO);
            for(auto &p: pipes){ close(p[0]); close(p[1]); }
            if(!pipeline[i].infile.empty()){ int fd=open(pipeline[i].infile.c_str(),O_RDONLY); if(fd<0) { perror("open infile"); _exit(1);} dup2(fd,STDIN_FILENO); close(fd); }
            if(!pipeline[i].outfile.empty()){ int flags=O_WRONLY|O_CREAT|(pipeline[i].append?O_APPEND:O_TRUNC); int fd=open(pipeline[i].outfile.c_str(),flags,0644); if(fd<0){ perror("open outfile"); _exit(1);} dup2(fd,STDOUT_FILENO); close(fd); }
            setpgid(0,0);
            signal(SIGINT,SIG_DFL); signal(SIGQUIT,SIG_DFL); signal(SIGTSTP,SIG_DFL); signal(SIGTTIN,SIG_DFL); signal(SIGTTOU,SIG_DFL); signal(SIGCHLD,SIG_DFL);
            if(is_builtin(pipeline[i])){ bool es=false; run_builtin(pipeline[i], es); _exit(0); }
            char **argv = vec_to_cstrs(pipeline[i].argv);
            execvp(argv[0], argv);
            perror("execvp"); free_cstrs(argv); _exit(127);
        } else {
            setpgid(pid, pid);
            if(first_child_pgid==0) first_child_pgid = pid;
            pids[i]=pid;
        }
    }
    for(auto &p: pipes){ close(p[0]); close(p[1]); }
    if(background){
        jobs.push_back({pids[0], orig_line, true, true});
        cout<<"["<<jobs.size()<<"] "<<pids[0]<<"\n";
    } else {
        // give terminal to job
        struct sigaction sa_ignore{}, sa_old_ttou, sa_old_ttin;
        sa_ignore.sa_handler = SIG_IGN; sigemptyset(&sa_ignore.sa_mask); sa_ignore.sa_flags=0;
        sigaction(SIGTTOU,&sa_ignore,&sa_old_ttou); sigaction(SIGTTIN,&sa_ignore,&sa_old_ttin);
        if(tcsetpgrp(shell_terminal, first_child_pgid)==-1){}
        sigaction(SIGTTOU,&sa_old_ttou,nullptr); sigaction(SIGTTIN,&sa_old_ttin,nullptr);
        for(pid_t pid: pids) waitpid(pid, nullptr, 0);
        if(tcsetpgrp(shell_terminal, shell_pgid)==-1){}
    }
    return 0;
}

int main(){
    signal(SIGCHLD, sigchld_handler);
    signal(SIGINT, sigint_handler);
    shell_terminal = STDIN_FILENO; shell_pgid = getpid();
    setpgid(shell_pgid, shell_pgid);
    tcsetpgrp(shell_terminal, shell_pgid);
    signal(SIGTTOU, SIG_IGN); signal(SIGTTIN, SIG_IGN);
    ios::sync_with_stdio(false); cin.tie(nullptr);

    rl_attempted_completion_function = fshell_completion;
    rl_bind_key('\t', rl_complete);

    bool exit_shell=false;
    while(!exit_shell){
        char cwd[512]; if(!getcwd(cwd,sizeof(cwd))) strcpy(cwd,"?");
        string prompt = "\001\033[1;32m\002fshell\001\033[0m\002:\001\033[1;34m\002" + string(cwd) + "\001\033[0m\002$ ";
        char *input = readline(prompt.c_str());
        if(!input){ cout<<"\n"; break; }
        string line = trim(input);
        free(input);
        if(line.empty()) continue;
        add_history(line.c_str());
        // show colored preview
        string colored = colorize_preview(line);
        cerr << "\033[2m" << colored << "\033[0m\n"; // dim preview
        vector<Cmd> pipeline; bool background=false;
        if(!parse_line(line, pipeline, background)){ cerr<<"parse error\n"; continue; }
        if(pipeline.size()==1 && is_builtin(pipeline[0]) && !background && pipeline[0].infile.empty() && pipeline[0].outfile.empty()){
            run_builtin(pipeline[0], exit_shell); continue;
        }
        execute_pipeline(pipeline, background, line);
    }
    return 0;
}

