#include "cact_install.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <wait.h>

int ci_exec(const char *path, char *const argv[]) {
    pid_t p = fork();
    if (p < 0)
        return -1;
    if (p == 0) {
        execve(path, argv, environ);
        _exit(127);
    }
    int st = 0;
    if (waitpid(p, &st, 0) < 0)
        return -1;
    if (WIFEXITED(st))
        return WEXITSTATUS(st);
    return -1;
}
