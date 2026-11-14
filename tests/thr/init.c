/* The first process of the test machine: runs every program listed in /tests, each as
   "== name ==", its output, "== name: status N ==", then turns the machine off. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void)
{
    FILE *f = fopen("/tests", "r");
    char line[256], path[300];
    mount("proc", "/proc", "proc", 0, 0);
    printf("== start ==\n");
    while (f && fgets(line, sizeof line, f)) {
        int status = 0;
        pid_t pid;
        line[strcspn(line, "\n")] = 0;
        if (!line[0])
            continue;
        printf("== %s ==\n", line);
        fflush(stdout);
        sprintf(path, "/bin/%s", line);
        pid = fork();
        if (pid == 0) {
            char *argv[2];
            argv[0] = path;
            argv[1] = 0;
            execv(path, argv);
            printf("cannot run %s\n", path);
            _exit(126);
        }
        waitpid(pid, &status, 0);
        if (WIFSIGNALED(status))
            printf("== %s: signal %d ==\n", line, WTERMSIG(status));
        else
            printf("== %s: status %d ==\n", line, WEXITSTATUS(status));
    }
    printf("== done ==\n");
    fflush(stdout);
    sync();
    reboot(RB_POWER_OFF);
    for (;;)
        pause();
}
