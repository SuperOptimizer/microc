/* The first process of the test machine of tests/cuda.sh: for every name in /tests runs /bin/ptxt /t/NAME.ptx
   (or /bin/NAME, if there is such a program) as "== name ==", its standard output, "== name: status N ==",
   then turns the machine off. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void)
{
    FILE *f = fopen("/tests", "r");
    char line[256], path[300], prog[300];
    printf("== start ==\n");
    while (f && fgets(line, sizeof line, f)) {
        int status = 0;
        pid_t pid;
        line[strcspn(line, "\n")] = 0;
        if (!line[0])
            continue;
        printf("== %s ==\n", line);
        fflush(stdout);
        sprintf(prog, "/bin/%s", line);
        sprintf(path, "/t/%s.ptx", line);
        pid = fork();
        if (pid == 0) {
            char *argv[3];
            int fd = open("/dev/null", O_WRONLY);
            if (fd >= 0)
                dup2(fd, 2);
            if (access(prog, X_OK) == 0) {
                argv[0] = prog;
                argv[1] = 0;
                execv(prog, argv);
            } else {
                argv[0] = "/bin/ptxt";
                argv[1] = path;
                argv[2] = 0;
                execv(argv[0], argv);
            }
            printf("cannot run %s\n", line);
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
