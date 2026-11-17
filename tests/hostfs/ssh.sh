# ssh.sh: run inside the root file system when it has ssh (mkssh.sh), as the user "tester" (tests/hostfs.sh adds
# the account to its copy of the image). The network is the container's own and empty (only its loopback
# exists), so the server started here listens where nobody else is. A key pair is made, the server started by
# start-ssh with the public key as the platform would pass it, and the client logs in over loopback: a
# command, its status, a session with a terminal and a shell typed into, a file by scp. Then that it is keys only: another key, no way in, and no password
# asked for.
mkdir -p /work/ssh && cd /work/ssh || exit 1
ok() { echo "ok $1"; }
bad() { echo "FAIL $1"; }
is() { if [ "$2" = "$3" ]; then ok $1; else bad "$1: got '$2', want '$3'"; fi; }
me=$(awk -F: -v u=$(id -u) '$3 == u { print $1; exit }' /etc/passwd) # (the account's name)
export DROPBEAR_DIR=/work/ssh/etc
rm -rf $HOME/.ssh $DROPBEAR_DIR /tmp/dropbear.log
ls /etc/dropbear/* /root/.ssh/* > inimage.out 2>/dev/null
is ssh-no-key-in-image "$(cat inimage.out)" ""
# without a public key the server does not start
if start-ssh 2222 2> nokey.err; then bad ssh-no-key-refused; else ok ssh-no-key-refused; fi
dropbearkey -t ed25519 -f id > key.out 2>&1 && ok ssh-keygen || { bad ssh-keygen; head -3 key.out; }
pub=$(grep '^ssh-ed25519 ' key.out)
dropbearkey -t ed25519 -f other > other.out 2>&1
if PUBLIC_KEY="$pub" start-ssh 2222 2> start.err; then ok ssh-start; else bad ssh-start; cat start.err; fi
[ -s $DROPBEAR_DIR/dropbear_ed25519_host_key ] && ok ssh-host-key-made || bad ssh-host-key-made
i=0; while [ $i -lt 20 ] && ! grep -q . /tmp/dropbear.pid 2>/dev/null; do sleep 0.2; i=$((i + 1)); done
C="dbclient -y -p 2222 -i id $me@127.0.0.1"
is ssh-command "$($C 'echo hello from $USER $((6 * 7)); id -u' 2> cmd.err | tr '\n' ' ')" "hello from $me 42 $(id -u) "
is ssh-status "$($C 'exit 7' 2>/dev/null; echo $?)" 7
# a session with a terminal: dbclient -t wants a terminal on its own side too, and there is none here, so a
# small program built on the spot gives it one (a pseudo-terminal it is the master of), copies what comes
# out, and types its second argument when the first output (the shell's prompt) has come
cat > pty.c <<'EOF'
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
int main(int argc, char **argv)
{
    int m = posix_openpt(O_RDWR | O_NOCTTY), st = 0, typed = 0;
    char buf[4096];
    long n;
    pid_t pid;
    if (argc < 3 || m < 0 || grantpt(m) || unlockpt(m)) {
        perror("pty");
        return 2;
    }
    pid = fork();
    if (pid == 0) {
        int s;
        setsid();
        s = open(ptsname(m), O_RDWR);
        if (s < 0 || ioctl(s, TIOCSCTTY, 0) < 0)
            _exit(126);
        dup2(s, 0);
        dup2(s, 1);
        dup2(s, 2);
        close(m);
        execvp(argv[2], argv + 2);
        _exit(127);
    }
    while ((n = read(m, buf, sizeof buf)) > 0) {
        fwrite(buf, 1, (size_t)n, stdout);
        if (!typed && argv[1][0]) {
            if (write(m, argv[1], strlen(argv[1])) < 0)
                return 2;
            typed = 1;
        }
    }
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}
EOF
if cc -o pty pty.c 2> pty.err; then
    ./pty '' $C -t 'if [ -t 0 ] && [ -t 1 ]; then echo on a terminal; fi' > tty.out 2>&1
    is ssh-tty "$(tr -d '\r' < tty.out | grep -c -x 'on a terminal')" 1
    # no command: a login shell on the terminal, typed into
    ./pty 'echo typed $((40 + 2)) $TERM; exit 5
' env TERM=vt100 $C > shell.out 2>&1
    is ssh-shell "$?: $(tr -d '\r' < shell.out | grep -c -x 'typed 42 vt100')" "5: 1"
else
    bad ssh-tty; head -3 pty.err
fi
is ssh-by-name "$(ssh -y -p 2222 -i id $me@localhost 'uname -s' 2>/dev/null)" "Linux"
seq 1 5000 > file
printf '#!/bin/sh\nexec dbclient -y "$@"\n' > dbc; chmod 755 dbc # (scp's ssh: the client, taking the host's key as it comes)
if scp -P 2222 -i id -S /work/ssh/dbc file $me@127.0.0.1:/tmp/copied > scp.out 2>&1 && cmp -s file /tmp/copied; then ok ssh-scp; else bad ssh-scp; head -3 scp.out; fi
is ssh-other-key-refused "$(dbclient -y -p 2222 -i other $me@127.0.0.1 'echo in' 2>/dev/null < /dev/null; [ $? != 0 ] && echo refused)" "refused"
is ssh-no-password-asked "$(dbclient -y -p 2222 -i other $me@127.0.0.1 true 2>&1 < /dev/null | grep -c -i 'password')" "0"
kill $(cat /tmp/dropbear.pid) 2>/dev/null
echo "== ssh done =="
