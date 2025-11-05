# sfsh
A *hella fast*, minimal shell for Linux. Think of it as your terminal, but with less drama and more speed.

## Installation
If you're on Arch Linux, congrats — you probably already know what you’re doing. For everyone else, just build it yourself like a real hacker:
```bash
git clone https://github.com/e3s4/sfsh.git
cd sfsh
g++ -std=c++17 -O2 -o sfsh sfsh.cpp
sudo mv sfsh /usr/local/bin/
```
then launch it like :
```
sfsh
```
Boom — you’ve got a shell that actually listens to you.

---

## Features

**Fast command execution (like, really fast)**

**Pipes, redirection, and background jobs**

**Built-in commands: cd, exit, jobs, fg, help**

**Minimal bloat, zero nonsense**

**Basically: everything you need, nothing you don’t.**

---

## Feedback
Got suggestions, bugs, or just wanna roast the code? Hit me up at eesak490@gmail.com
