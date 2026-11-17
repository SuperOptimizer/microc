/* the tools, by name */
int cat_main(int argc, char **argv);
int rm_main(int argc, char **argv);
int mkdir_main(int argc, char **argv);
int rmdir_main(int argc, char **argv);
int mv_main(int argc, char **argv);
int cp_main(int argc, char **argv);
int ln_main(int argc, char **argv);
int touch_main(int argc, char **argv);
int chmod_main(int argc, char **argv);
int ls_main(int argc, char **argv);
int readlink_main(int argc, char **argv);
int mktemp_main(int argc, char **argv);
int cmp_main(int argc, char **argv);
int tee_main(int argc, char **argv);
int dd_main(int argc, char **argv);
int stat_main(int argc, char **argv);
int true_main(int argc, char **argv);
int false_main(int argc, char **argv);
int pwd_main(int argc, char **argv);
int head_main(int argc, char **argv);
int tail_main(int argc, char **argv);
int wc_main(int argc, char **argv);
int basename_main(int argc, char **argv);
int dirname_main(int argc, char **argv);
int sleep_main(int argc, char **argv);
int env_main(int argc, char **argv);
int uname_main(int argc, char **argv);
int hostname_main(int argc, char **argv);
int whoami_main(int argc, char **argv);
int id_main(int argc, char **argv);
int nproc_main(int argc, char **argv);
int getconf_main(int argc, char **argv);
int seq_main(int argc, char **argv);
int which_main(int argc, char **argv);
int date_main(int argc, char **argv);
int expr_main(int argc, char **argv);
int tr_main(int argc, char **argv);
int cut_main(int argc, char **argv);
int sort_main(int argc, char **argv);
int uniq_main(int argc, char **argv);
int od_main(int argc, char **argv);
int xargs_main(int argc, char **argv);
int sha1sum_main(int argc, char **argv);
int grep_main(int argc, char **argv);
int diff_main(int argc, char **argv);
int bc_main(int argc, char **argv);
int sed_main(int argc, char **argv);
int find_main(int argc, char **argv);
int awk_main(int argc, char **argv);
int gzip_main(int argc, char **argv);
int bzip2_main(int argc, char **argv);
int tar_main(int argc, char **argv);
int init_main(int argc, char **argv);

static const struct {
    const char *name;
    applet_fn fn;
} applets[] = {
    { "sh", sh_main },
    { "echo", echo_main },
    { "printf", printf_main },
    { "test", test_main },
    { "[", test_main },
    { "cat", cat_main },
    { "rm", rm_main },
    { "mkdir", mkdir_main },
    { "rmdir", rmdir_main },
    { "mv", mv_main },
    { "cp", cp_main },
    { "ln", ln_main },
    { "touch", touch_main },
    { "chmod", chmod_main },
    { "ls", ls_main },
    { "readlink", readlink_main },
    { "realpath", readlink_main },
    { "mktemp", mktemp_main },
    { "cmp", cmp_main },
    { "tee", tee_main },
    { "dd", dd_main },
    { "stat", stat_main },
    { "true", true_main },
    { "false", false_main },
    { "pwd", pwd_main },
    { "head", head_main },
    { "tail", tail_main },
    { "wc", wc_main },
    { "basename", basename_main },
    { "dirname", dirname_main },
    { "sleep", sleep_main },
    { "env", env_main },
    { "uname", uname_main },
    { "hostname", hostname_main },
    { "whoami", whoami_main },
    { "id", id_main },
    { "nproc", nproc_main },
    { "getconf", getconf_main },
    { "seq", seq_main },
    { "which", which_main },
    { "date", date_main },
    { "expr", expr_main },
    { "tr", tr_main },
    { "cut", cut_main },
    { "sort", sort_main },
    { "uniq", uniq_main },
    { "od", od_main },
    { "xargs", xargs_main },
    { "sha1sum", sha1sum_main },
    { "grep", grep_main },
    { "egrep", grep_main },
    { "fgrep", grep_main },
    { "diff", diff_main },
    { "bc", bc_main },
    { "sed", sed_main },
    { "find", find_main },
    { "awk", awk_main },
    { "gzip", gzip_main },
    { "bzip2", bzip2_main },
    { "gunzip", gzip_main },
    { "zcat", gzip_main },
    { "tar", tar_main },
    { "init", init_main },
    { 0, 0 }
};
