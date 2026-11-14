/* tls_server.c PORT KEY.der CERT.der DOCROOT: a minimal HTTPS file server over BearSSL for tests/ulibs, or with
   "-" for the key a plain HTTP one: GET and HEAD, "Range: bytes=a-b" (also "a-" and "-n"), a redirect from a
   directory to its index.html, persistent connections, one process per connection. It starts listening,
   then goes to the background. */
#include <arpa/inet.h>
#include <bearssl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static unsigned char *slurp(const char *path, size_t *n)
{
    struct stat st;
    FILE *f = stat(path, &st) == 0 && S_ISREG(st.st_mode) ? fopen(path, "rb") : NULL;
    unsigned char *p;
    long len;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); len = ftell(f); fseek(f, 0, SEEK_SET);
    p = malloc(len + 1);
    if (fread(p, 1, len, f) != (size_t)len) { fclose(f); return NULL; }
    fclose(f);
    *n = len;
    return p;
}
static int sock_read(void *ctx, unsigned char *buf, size_t len)
{
    ssize_t r = read(*(int *)ctx, buf, len);
    return r <= 0 ? -1 : (int)r;
}
static int sock_write(void *ctx, const unsigned char *buf, size_t len)
{
    ssize_t r = write(*(int *)ctx, buf, len);
    return r <= 0 ? -1 : (int)r;
}

typedef struct { int fd; br_sslio_context *io; } conn;
static int rd1(conn *c, char *p) { return c->io ? br_sslio_read(c->io, p, 1) == 1 : read(c->fd, p, 1) == 1; }
static void wr(conn *c, const void *p, size_t n)
{
    if (c->io) { br_sslio_write_all(c->io, p, n); return; }
    while (n) { ssize_t r = write(c->fd, p, n); if (r <= 0) return; p = (const char *)p + r; n -= (size_t)r; }
}

static void serve(conn *c, const char *root)
{
    for (;;) {
        char req[4096], url[1024], path[2048], hdr[512], *r; int n = 0, head; unsigned char *body; size_t bn = 0, total; long from = 0, to = -1;
        while (n < (int)sizeof req - 1) {
            if (!rd1(c, req + n)) return;
            n++;
            if (n >= 4 && !memcmp(req + n - 4, "\r\n\r\n", 4)) break;
        }
        req[n] = 0;
        head = !strncmp(req, "HEAD ", 5);
        url[0] = 0; sscanf(req, "%*s %1000s", url);
        if (strstr(url, "..")) strcpy(url, "/");
        snprintf(path, sizeof path, "%s%s", root, url);
        body = slurp(path, &bn);
        if (!body && url[strlen(url) - 1] != '/') {       /* a directory? send the client to its index */
            snprintf(path, sizeof path, "%s%s/index.html", root, url);
            if ((body = slurp(path, &bn)) != NULL) {
                n = snprintf(hdr, sizeof hdr, "HTTP/1.1 302 Found\r\nLocation: %s/index.html\r\nContent-Length: 0\r\n\r\n", url);
                wr(c, hdr, n); if (c->io) br_sslio_flush(c->io);
                free(body);
                continue;
            }
        }
        total = bn;
        r = strstr(req, "\r\nRange: bytes=");
        if (r) {
            r += 15;
            if (*r == '-') { from = (long)bn - atol(r + 1); if (from < 0) from = 0; }
            else sscanf(r, "%ld-%ld", &from, &to);
        }
        if (!body) {
            n = snprintf(hdr, sizeof hdr, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n"); bn = 0;
        } else if (r && from >= 0 && (size_t)from < bn) {
            if (to < 0 || (size_t)to >= bn) to = (long)bn - 1;
            n = snprintf(hdr, sizeof hdr, "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes %ld-%ld/%zu\r\nContent-Length: %ld\r\n\r\n", from, to, total, to - from + 1);
            bn = (size_t)(to - from + 1);
        } else {
            from = 0;
            n = snprintf(hdr, sizeof hdr, "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\nAccept-Ranges: bytes\r\n\r\n", bn);
        }
        wr(c, hdr, n);
        if (!head && bn) wr(c, body + from, bn);
        if (c->io) br_sslio_flush(c->io);
        free(body);
    }
}

int main(int argc, char **argv)
{
    static unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
    br_skey_decoder_context kd; br_x509_certificate chain[1]; const br_rsa_private_key *sk = NULL;
    struct sockaddr_in a; int ls, one = 1, tls; size_t kn, cn; unsigned char *key, *cert;
    if (argc != 5) return 2;
    tls = strcmp(argv[2], "-") != 0;
    if (tls) {
        key = slurp(argv[2], &kn); cert = slurp(argv[3], &cn);
        if (!key || !cert) { fprintf(stderr, "tls_server: cannot read the key or the certificate\n"); return 1; }
        br_skey_decoder_init(&kd); br_skey_decoder_push(&kd, key, kn);
        if (br_skey_decoder_last_error(&kd) || br_skey_decoder_key_type(&kd) != BR_KEYTYPE_RSA) { fprintf(stderr, "tls_server: bad key\n"); return 1; }
        sk = br_skey_decoder_get_rsa(&kd);
        chain[0].data = cert; chain[0].data_len = cn;
    }
    ls = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_port = htons(atoi(argv[1])); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(ls, (struct sockaddr *)&a, sizeof a) || listen(ls, 64)) { perror("tls_server: bind"); return 1; }
    if (fork()) return 0;
    signal(SIGPIPE, SIG_IGN); signal(SIGCHLD, SIG_IGN);
    for (;;) {
        conn c; br_ssl_server_context sc; br_sslio_context io;
        c.fd = accept(ls, NULL, NULL); c.io = NULL;
        if (c.fd < 0) continue;
        if (fork() == 0) {
            close(ls);
            if (tls) {
                br_ssl_server_init_full_rsa(&sc, chain, 1, sk);
                br_ssl_engine_set_buffer(&sc.eng, iobuf, sizeof iobuf, 1);
                br_ssl_server_reset(&sc);
                br_sslio_init(&io, &sc.eng, sock_read, &c.fd, sock_write, &c.fd);
                c.io = &io;
            }
            serve(&c, argv[4]);
            if (tls) br_sslio_close(&io);
            _exit(0);
        }
        close(c.fd);
    }
}
