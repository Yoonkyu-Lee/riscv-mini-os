// Copyright (c) 2025 Yoonkyu Lee
// SPDX-License-Identifier: MIT
//
// shell -- a small interactive REPL for riscv-mini-os.
//
// Commands:
//   ls                  list KTFS files this shell knows about
//   cat <file>          dump a file to the terminal
//   run <name>          fork+exec the named program, wait for it
//   help                print the command list
//   exit                terminate the shell
//
// The shell talks to uart1 (kernel keeps uart0 for itself). Line
// editing supports backspace; everything else is single-character
// echo.

#include <stddef.h>
#include "syscall.h"
#include "string.h"

#define LINE_MAX 128
#define ARGV_MAX 8
#define UART_FD  0

// Files the shell will probe in `ls`. Anything that opens cleanly
// shows up; anything that doesn't is silently skipped.
static const char * const KNOWN_FILES[] = {
    "fib", "hello", "init", "trekfib", "shell", NULL,
};

static void say(int fd, const char * s) {
    _write(fd, s, strlen(s));
}

static int read_line(int fd, char * buf, int max) {
    int n = 0;
    char c;
    for (;;) {
        long r = _read(fd, &c, 1);
        if (r <= 0) {
            buf[n] = '\0';
            return n > 0 ? n : -1;
        }
        if (c == '\r' || c == '\n') {
            say(fd, "\r\n");
            buf[n] = '\0';
            return n;
        }
        if (c == 0x7f || c == 0x08) {     // DEL / BS
            if (n > 0) {
                n--;
                say(fd, "\b \b");
            }
            continue;
        }
        if (n < max - 1) {
            buf[n++] = c;
            _write(fd, &c, 1);
        }
    }
}

static int tokenize(char * buf, char ** argv, int max_argv) {
    int argc = 0;
    char * p = buf;
    while (argc < max_argv - 1) {
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') break;
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = '\0';
    }
    argv[argc] = NULL;
    return argc;
}

static void cmd_ls(int fd) {
    for (const char * const * n = KNOWN_FILES; *n; n++) {
        int probe = _fsopen(-1, *n);
        if (probe >= 0) {
            say(fd, *n);
            say(fd, "\r\n");
            _close(probe);
        }
    }
}

static void cmd_cat(int fd, const char * name) {
    int ffd = _fsopen(-1, name);
    if (ffd < 0) {
        say(fd, "cat: cannot open '");
        say(fd, name);
        say(fd, "'\r\n");
        return;
    }
    char buf[256];
    long n;
    while ((n = _read(ffd, buf, sizeof buf)) > 0)
        _write(fd, buf, n);
    _close(ffd);
}

static void cmd_run(int fd, const char * name) {
    int ffd = _fsopen(-1, name);
    if (ffd < 0) {
        say(fd, "run: cannot open '");
        say(fd, name);
        say(fd, "'\r\n");
        return;
    }
    int tid = _fork();
    if (tid == 0) {
        char * argv[2] = { (char *)name, NULL };
        _exec(ffd, 1, argv);
        _exit();   // exec failed
    }
    if (tid < 0) {
        say(fd, "run: fork failed\r\n");
        _close(ffd);
        return;
    }
    _close(ffd);
    _wait(tid);
}

void main(void) {
    if (_devopen(UART_FD, "uart", 1) < 0) {
        _print("shell: failed to open uart\n");
        _exit();
    }

    say(UART_FD, "riscv-mini-os shell\r\n");
    say(UART_FD, "type 'help' for commands\r\n");

    char line[LINE_MAX];
    char * argv[ARGV_MAX];

    for (;;) {
        say(UART_FD, "$ ");
        int n = read_line(UART_FD, line, sizeof line);
        if (n < 0) break;
        if (n == 0) continue;

        int argc = tokenize(line, argv, ARGV_MAX);
        if (argc == 0) continue;

        if (!strcmp(argv[0], "exit"))
            break;
        if (!strcmp(argv[0], "help")) {
            say(UART_FD, "commands: ls | cat <file> | run <name> | help | exit\r\n");
            continue;
        }
        if (!strcmp(argv[0], "ls")) {
            cmd_ls(UART_FD);
            continue;
        }
        if (!strcmp(argv[0], "cat")) {
            if (argc < 2)
                say(UART_FD, "usage: cat <file>\r\n");
            else
                cmd_cat(UART_FD, argv[1]);
            continue;
        }
        if (!strcmp(argv[0], "run")) {
            if (argc < 2)
                say(UART_FD, "usage: run <name>\r\n");
            else
                cmd_run(UART_FD, argv[1]);
            continue;
        }
        say(UART_FD, "unknown command: ");
        say(UART_FD, argv[0]);
        say(UART_FD, "\r\n");
    }
    _exit();
}
