#include <bits/stdc++.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <termios.h>
#include <readline/readline.h>
#include <readline/history.h>
using namespace std;

struct Job {
    pid_t pid;
    string cmdline;
    bool running;
    bool background;
};
static vector<Job> jobs;
static pid_t shell_pgid = 0;
static int shell_terminal = 0;

static inline string trim(const string &s) {
    size_t a = s.find_first_not_of(" \t\n\r");
    if (a == string::npos) return "";
    size_t b = s.find_last_not_of(" \t\n\r");
    return s.substr(a, b - a + 1);
}

vector<string> tokenize(const string &line) {
    vector<string> toks;
    string cur;
    bool in_squote = false, in_dquote = false;
    for (char c : line) {
        if (c == '\'' && !in_dquote) { in_squote = !in_squote; continue; }
        if (c == '"' && !in_squote) { in_dquote = !in_dquote; continue; }
        if (!in_squote && !in_dquote && isspace((unsigned char)c)) {
            if (!cur.empty()) {
                if (cur[0] == '~' && (cur.size() == 1 || cur[1] == '/')) {
                    const char* home = getenv("HOME");
                    if (home) cur = string(home) + cur.substr(1);
                }
                toks.push_back(cur);
                cur.clear();
            }
        } else cur.push_back(c);
    }
    if (!cur.empty()) {
        if (cur[0] == '~' && (cur.size() == 1 || cur[1] == '/')) {
            const char* home = getenv("HOME");
            if (home) cur = string(home) + cur.substr(1);
        }
        toks.push_back(cur);
    }
    return toks;
}

struct Cmd {
    vector<string> argv;
    string infile, outfile;
    bool append = false;
};

bool parse_line(const string &line, vector<Cmd> &pipeline, bool &background) {
    pipeline.clear();
    background = false;
    auto toks = tokenize(line);
    if (toks.empty()) return false;
    if (toks.back() == "&") { background = true; toks.pop_back(); }
    Cmd cur;
    for (size_t i = 0; i < toks.size();) {
        string t = toks[i];
        if (t == "|") {
            if (cur.argv.empty()) return false;
            pipeline.push_back(cur);
            cur = Cmd();
            ++i;
        } else if (t == "<") {
            if (i + 1 >= toks.size()) return false;
            cur.infile = toks[++i];
            ++i;
        } else if (t == ">" || t == ">>") {
            if (i + 1 >= toks.size()) return false;
            cur.outfile = toks[++i];
            cur.append = (t == ">>");
            ++i;
        } else {
            cur.argv.push_back(t);
            ++i;
        }
    }
    if (!cur.argv.empty()) pipeline.push_back(cur);
    return !pipeline.empty();
}

inline char** vec_to_cstrs(const vector<string> &v) {
    char **arr = new char*[v.size() + 1];
    for (size_t i = 0; i < v.size(); ++i) arr[i] = strdup(v[i].c_str());
    arr[v.size()] = nullptr;
    return arr;
}
inline void free_cstrs(char **arr) {
    if (!arr) return;
    for (size_t i = 0; arr[i]; ++i) free(arr[i]);
    delete[] arr;
}

bool is_builtin(const Cmd &c) {
    if (c.argv.empty()) return false;
    string cmd = c.argv[0];
    return (cmd == "cd" || cmd == "exit" || cmd == "jobs" || cmd == "fg" || cmd == "help");
}

int run_builtin(Cmd &c, bool &exit_shell) {
    string cmd = c.argv[0];
    if (cmd == "cd") {
        string dir = (c.argv.size() >= 2 ? c.argv[1] : getenv("HOME"));
        if (chdir(dir.c_str()) != 0) perror("cd");
        return 0;
    } else if (cmd == "exit") {
        exit_shell = true;
        return 0;
    } else if (cmd == "jobs") {
        for (size_t i = 0; i < jobs.size(); ++i)
            cout << "[" << i + 1 << "] pid=" << jobs[i].pid
                 << (jobs[i].running ? " Running " : " Done ")
                 << (jobs[i].background ? "bg " : "fg ")
                 << jobs[i].cmdline << "\n";
        return 0;
    } else if (cmd == "fg") {
        if (c.argv.size() < 2) { cerr << "fg: usage: fg <jobnum>\n"; return -1; }
        int j = stoi(c.argv[1]) - 1;
        if (j < 0 || (size_t)j >= jobs.size()) { cerr << "fg: no such job\n"; return -1; }
        pid_t pid = jobs[j].pid;
        jobs[j].background = false;
        int status;
        waitpid(pid, &status, 0);
        return 0;
    } else if (cmd == "help") {
        cout << "builtins: cd, exit, jobs, fg, help\n";
        return 0;
    }
    return -1;
}

void sigchld_handler(int) {
    int status; pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
        for (auto &j : jobs) if (j.pid == pid) j.running = false;
}
void sigint_handler(int) { cerr << "\n"; }

int execute_pipeline(vector<Cmd> &pipeline, bool background, const string &orig_line) {
    size_t n = pipeline.size();
    vector<array<int, 2>> pipes(n > 1 ? n - 1 : 0);
    for (size_t i = 0; i + 1 < n; ++i)
        if (pipe(pipes[i].data()) == -1) return perror("pipe"), -1;

    vector<pid_t> pids(n);
    for (size_t i = 0; i < n; ++i) {
        pid_t pid = fork();
        if (pid < 0) return perror("fork"), -1;
        if (pid == 0) {
            if (i > 0) dup2(pipes[i - 1][0], STDIN_FILENO);
            if (i + 1 < n) dup2(pipes[i][1], STDOUT_FILENO);
            for (auto &p : pipes) { close(p[0]); close(p[1]); }
            if (!pipeline[i].infile.empty()) {
                int fd = open(pipeline[i].infile.c_str(), O_RDONLY);
                dup2(fd, STDIN_FILENO); close(fd);
            }
            if (!pipeline[i].outfile.empty()) {
                int flags = O_WRONLY | O_CREAT | (pipeline[i].append ? O_APPEND : O_TRUNC);
                int fd = open(pipeline[i].outfile.c_str(), flags, 0644);
                dup2(fd, STDOUT_FILENO); close(fd);
            }
            if (is_builtin(pipeline[i])) { bool ex = false; run_builtin(pipeline[i], ex); _exit(0); }
            char **argv = vec_to_cstrs(pipeline[i].argv);
            execvp(argv[0], argv);
            perror("execvp"); free_cstrs(argv); _exit(127);
        } else pids[i] = pid;
    }
    for (auto &p : pipes) { close(p[0]); close(p[1]); }

    if (background) {
        jobs.push_back({pids[0], orig_line, true, true});
        cout << "[" << jobs.size() << "] " << pids[0] << "\n";
    } else {
        for (pid_t pid : pids) waitpid(pid, nullptr, 0);
    }
    return 0;
}

int main() {
    signal(SIGCHLD, sigchld_handler);
    signal(SIGINT, sigint_handler);
    shell_terminal = STDIN_FILENO;
    shell_pgid = getpid();
    setpgid(shell_pgid, shell_pgid);
    tcsetpgrp(shell_terminal, shell_pgid);

    ios::sync_with_stdio(false);
    cin.tie(nullptr);

    bool exit_shell = false;
    while (!exit_shell) {
        char cwd[512]; getcwd(cwd, sizeof(cwd));
        string prompt = "\001\033[1;32m\002fshell\001\033[0m\002:\001\033[1;34m\002" + string(cwd) + "\001\033[0m\002$ ";
        char* input = readline(prompt.c_str());
        if (!input) { cout << "\n"; break; }
        string line = trim(input); free(input);
        if (line.empty()) continue;
        add_history(line.c_str());

        vector<Cmd> pipeline;
        bool background = false;
        if (!parse_line(line, pipeline, background)) { cerr << "parse error\n"; continue; }
        if (pipeline.size() == 1 && is_builtin(pipeline[0]) && !background &&
            pipeline[0].infile.empty() && pipeline[0].outfile.empty()) {
            run_builtin(pipeline[0], exit_shell);
            continue;
        }
        execute_pipeline(pipeline, background, line);
    }
    return 0;
}

