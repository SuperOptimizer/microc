/* Dropbear's options for the hostfs image (mkssh.sh), where they differ from default_options.h.
   Keys only: the server takes no passwords (there is no password database to check them against, and the
   image has no account with one), and the client asks for none. Nothing about the machine is recorded or
   looked up; the host keys are made when the server first starts or come from a mounted directory
   (tests/hostfs and the README say how). */
#define DROPBEAR_SVR_PASSWORD_AUTH 0
#define DROPBEAR_SVR_PAM_AUTH 0
#define DROPBEAR_CLI_PASSWORD_AUTH 0
#define DROPBEAR_USE_PASSWORD_ENV 0
#define DROPBEAR_CLI_ASKPASS_HELPER 0
#define DROPBEAR_X11FWD 0
#define DO_MOTD 0
#define DO_HOST_LOOKUP 0
#define DROPBEAR_SFTPSERVER 0
#define DROPBEAR_PIDFILE "/tmp/dropbear.pid"
#define DROPBEAR_PATH_SSH_PROGRAM "/usr/bin/dbclient"
#define DEFAULT_PATH "/usr/bin"
#define DEFAULT_ROOT_PATH "/usr/bin"
