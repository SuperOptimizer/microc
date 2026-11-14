/* libcurl the way ufsm's store.c uses it: easy handles with range requests, HEAD, redirects and error
   statuses, the share interface, and the multi interface with many ranges in flight; over HTTP and HTTPS against
   tls_server.c (and one plain GET against busybox httpd), all started by guest.sh on 127.0.0.1. */
#include "t.h"
#include <curl/curl.h>
#define HTTP "http://127.0.0.1:8081"
#define BBHTTP "http://127.0.0.1:8080"
#define HTTPS "https://localhost:8443"
typedef struct { unsigned char *p; size_t n, cap; } sink;
static size_t on_data(char *d, size_t s, size_t n, void *u)
{
    sink *k = u;
    if (k->n + s * n > k->cap) return 0;
    memcpy(k->p + k->n, d, s * n); k->n += s * n;
    return s * n;
}
static int hdr_seen;
static size_t on_hdr(char *d, size_t s, size_t n, void *u)
{
    if (s * n > 14 && !strncasecmp(d, "content-length", 14)) hdr_seen++;
    (void)u;
    return s * n;
}
static int nlock;
static void share_lock(CURL *h, curl_lock_data d, curl_lock_access a, void *u) { (void)h; (void)d; (void)a; (void)u; nlock++; }
static void share_unlock(CURL *h, curl_lock_data d, void *u) { (void)h; (void)d; (void)u; nlock--; }
static int sockopt_calls;
static int on_sockopt(void *u, curl_socket_t fd, curlsocktype purpose) { (void)u; (void)fd; (void)purpose; sockopt_calls++; return CURL_SOCKOPT_OK; }
static struct curl_slist *resolve;
static void base(CURL *c, CURLSH *sh)
{
    curl_easy_reset(c);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "ulibs-test/1");
    curl_easy_setopt(c, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(c, CURLOPT_BUFFERSIZE, 65536L);
    curl_easy_setopt(c, CURLOPT_MAXCONNECTS, 2L);
    curl_easy_setopt(c, CURLOPT_RESOLVE, resolve);
    curl_easy_setopt(c, CURLOPT_CAINFO, "data/tls-ca.pem");
    if (sh) curl_easy_setopt(c, CURLOPT_SHARE, sh);
}
static long get(CURL *c, const char *url, const char *range, sink *k, CURLcode *rc)
{
    long code = 0;
    k->n = 0;
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, k);
    if (range) curl_easy_setopt(c, CURLOPT_RANGE, range);
    *rc = curl_easy_perform(c);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    return code;
}
#define NREQ 24
static int multi(const char *url, const unsigned char *plain, size_t n, CURLSH *sh, int nconn)
{
    CURLM *m = curl_multi_init(); CURL *e[NREQ]; sink k[NREQ]; char range[NREQ][64]; int i, running = 1, done = 0, good = 0; CURLMsg *msg;
    curl_multi_setopt(m, CURLMOPT_PIPELINING, CURLPIPE_MULTIPLEX);
    curl_multi_setopt(m, CURLMOPT_MAXCONNECTS, (long)nconn);
    curl_multi_setopt(m, CURLMOPT_MAX_HOST_CONNECTIONS, (long)nconn);
    curl_multi_setopt(m, CURLMOPT_MAX_CONCURRENT_STREAMS, 32L);
    for (i = 0; i < NREQ; i++) {
        size_t from = (size_t)i * 2371 % (n - 5000), len = 1 + (size_t)i * 199;
        e[i] = curl_easy_init(); base(e[i], sh);
        k[i].p = malloc(len); k[i].cap = len; k[i].n = 0;
        sprintf(range[i], "%zu-%zu", from, from + len - 1);
        curl_easy_setopt(e[i], CURLOPT_URL, url);
        curl_easy_setopt(e[i], CURLOPT_RANGE, range[i]);
        curl_easy_setopt(e[i], CURLOPT_WRITEFUNCTION, on_data);
        curl_easy_setopt(e[i], CURLOPT_WRITEDATA, &k[i]);
        curl_easy_setopt(e[i], CURLOPT_PRIVATE, (char *)(size_t)i);
        curl_easy_setopt(e[i], CURLOPT_PIPEWAIT, 1L);
        curl_easy_setopt(e[i], CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_2TLS);
        curl_multi_add_handle(m, e[i]);
    }
    while (running) {
        int nq;
        if (curl_multi_perform(m, &running) != CURLM_OK) break;
        while ((msg = curl_multi_info_read(m, &nq)) != NULL) {
            if (msg->msg == CURLMSG_DONE) {
                char *priv = NULL; long code = 0; size_t from, len;
                curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &priv);
                curl_easy_getinfo(msg->easy_handle, CURLINFO_RESPONSE_CODE, &code);
                i = (int)(size_t)priv; from = (size_t)i * 2371 % (n - 5000); len = 1 + (size_t)i * 199;
                done++;
                if (msg->data.result == CURLE_OK && code == 206 && k[i].n == len && !memcmp(k[i].p, plain + from, len)) good++;
                else printf("# multi request %d: result %d http %ld, %zu of %zu bytes\n", i, msg->data.result, code, k[i].n, len);
            }
        }
        if (running && curl_multi_poll(m, NULL, 0, 1000, NULL) != CURLM_OK) break;
    }
    for (i = 0; i < NREQ; i++) { curl_multi_remove_handle(m, e[i]); curl_easy_cleanup(e[i]); free(k[i].p); }
    curl_multi_cleanup(m);
    return done == NREQ && good == NREQ;
}
int main(void)
{
    size_t n; unsigned char *plain = slurp("data/plain.bin", &n); sink k; CURL *c; CURLSH *sh; CURLcode rc; long code, nc = 0;
    curl_off_t cl = -1, dl = 0, us = 0; struct curl_slist *hdr; char err[CURL_ERROR_SIZE];
    k.p = malloc(PLAIN_LEN + 100); k.cap = PLAIN_LEN + 100;
    CHECK("curl-global-init", curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK);
    printf("# %s\n", curl_version());
    resolve = curl_slist_append(NULL, "localhost:8443:127.0.0.1");
    sh = curl_share_init();
    CHECK("curl-share-setup", sh && curl_share_setopt(sh, CURLSHOPT_LOCKFUNC, share_lock) == CURLSHE_OK && curl_share_setopt(sh, CURLSHOPT_UNLOCKFUNC, share_unlock) == CURLSHE_OK &&
          curl_share_setopt(sh, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS) == CURLSHE_OK && curl_share_setopt(sh, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION) == CURLSHE_OK &&
          curl_share_setopt(sh, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT) == CURLSHE_OK);
    c = curl_easy_init(); base(c, sh);
    hdr = curl_slist_append(NULL, "Authorization: Bearer test-token");
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, on_hdr);
    curl_easy_setopt(c, CURLOPT_SOCKOPTFUNCTION, on_sockopt);
    code = get(c, HTTP "/plain.bin", NULL, &k, &rc);
    CHECK("curl-http-get", rc == CURLE_OK && code == 200 && k.n == n && !memcmp(k.p, plain, n) && hdr_seen == 1 && sockopt_calls == 1);
    curl_easy_getinfo(c, CURLINFO_SIZE_DOWNLOAD_T, &dl); curl_easy_getinfo(c, CURLINFO_TOTAL_TIME_T, &us); curl_easy_getinfo(c, CURLINFO_NUM_CONNECTS, &nc);
    CHECK("curl-getinfo", dl == (curl_off_t)n && us > 0 && nc == 1);
    base(c, sh);
    code = get(c, HTTP "/plain.bin", "1000-1999", &k, &rc); nc = -1; curl_easy_getinfo(c, CURLINFO_NUM_CONNECTS, &nc);
    CHECK("curl-http-range", rc == CURLE_OK && code == 206 && k.n == 1000 && !memcmp(k.p, plain + 1000, 1000));
    CHECK("curl-connection-reused", nc == 0);
    base(c, sh);
    code = get(c, HTTP "/plain.bin", "59990-", &k, &rc);
    CHECK("curl-http-range-open-end", rc == CURLE_OK && code == 206 && k.n == 10 && !memcmp(k.p, plain + n - 10, 10));
    base(c, sh);
    code = get(c, HTTP "/plain.bin", "-7", &k, &rc);
    CHECK("curl-http-range-suffix", rc == CURLE_OK && code == 206 && k.n == 7 && !memcmp(k.p, plain + n - 7, 7));
    base(c, sh); curl_easy_setopt(c, CURLOPT_NOBODY, 1L);
    code = get(c, HTTP "/plain.bin", NULL, &k, &rc); curl_easy_getinfo(c, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &cl);
    CHECK("curl-http-head", rc == CURLE_OK && code == 200 && k.n == 0 && cl == (curl_off_t)n);
    base(c, sh); curl_easy_setopt(c, CURLOPT_ERRORBUFFER, err);
    code = get(c, HTTP "/missing.bin", NULL, &k, &rc);
    CHECK("curl-http-404-fails", rc == CURLE_HTTP_RETURNED_ERROR && code == 404 && strstr(err, "404"));
    base(c, sh);
    code = get(c, HTTP "/dir", NULL, &k, &rc);       /* httpd answers with a redirect to /dir/ */
    CHECK("curl-http-redirect", rc == CURLE_OK && code == 200 && k.n == 6 && !memcmp(k.p, "index\n", 6));
    base(c, sh); k.cap = 100;
    code = get(c, HTTP "/plain.bin", NULL, &k, &rc); k.cap = PLAIN_LEN + 100;
    CHECK("curl-write-error", rc == CURLE_WRITE_ERROR);
    base(c, sh);
    CHECK("curl-http2-request-degrades", curl_easy_setopt(c, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_2TLS) == CURLE_OK &&
          get(c, HTTP "/plain.bin", "0-99", &k, &rc) == 206 && rc == CURLE_OK && k.n == 100);
    base(c, sh);
    code = get(c, BBHTTP "/plain.bin", NULL, &k, &rc);
    CHECK("curl-http-get-busybox-httpd", rc == CURLE_OK && code == 200 && k.n == n && !memcmp(k.p, plain, n));
    base(c, sh);                         /* a name, through getaddrinfo (in a thread of its own when built with THREADS=1) */
    code = get(c, "http://localhost:8081/plain.bin", "5-9", &k, &rc);
    if (rc) printf("# localhost: %s\n", curl_easy_strerror(rc));
    CHECK("curl-resolve-name", rc == CURLE_OK && code == 206 && k.n == 5);
    base(c, sh);
    code = get(c, "http://no-such-host.invalid:8081/", NULL, &k, &rc);
    CHECK("curl-resolve-failure", rc == CURLE_COULDNT_RESOLVE_HOST);
    base(c, sh);
    code = get(c, "http://127.0.0.1:9/x", NULL, &k, &rc);
    CHECK("curl-connect-refused", rc == CURLE_COULDNT_CONNECT);
    CHECK("curl-multi-http-1conn", multi(HTTP "/plain.bin", plain, n, sh, 1));
    CHECK("curl-multi-http-6conn", multi(HTTP "/plain.bin", plain, n, sh, 6));
    /* TLS: our own authority signs the server's certificate for "localhost" */
    base(c, sh);
    code = get(c, HTTPS "/plain.bin", NULL, &k, &rc);
    if (rc) printf("# https: %s\n", curl_easy_strerror(rc));
    CHECK("curl-https-get", rc == CURLE_OK && code == 200 && k.n == n && !memcmp(k.p, plain, n));
    base(c, sh);
    code = get(c, HTTPS "/plain.bin", "30000-30999", &k, &rc);
    CHECK("curl-https-range", rc == CURLE_OK && code == 206 && k.n == 1000 && !memcmp(k.p, plain + 30000, 1000));
    base(c, sh); curl_easy_setopt(c, CURLOPT_CAINFO, "data/tls-other-ca.pem");
    code = get(c, HTTPS "/plain.bin", NULL, &k, &rc);
    CHECK("curl-https-unknown-ca-rejected", rc == CURLE_PEER_FAILED_VERIFICATION && k.n == 0);
    base(c, sh); curl_easy_setopt(c, CURLOPT_CAINFO, "data/tls-other-ca.pem"); curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
    code = get(c, HTTPS "/plain.bin", "0-9", &k, &rc);
    CHECK("curl-https-verify-off", rc == CURLE_OK && code == 206 && k.n == 10);
    base(c, sh);
    code = get(c, "https://127.0.0.1:8443/plain.bin", NULL, &k, &rc);      /* the certificate does not name the address */
    CHECK("curl-https-wrong-name-rejected", rc == CURLE_PEER_FAILED_VERIFICATION);
    CHECK("curl-multi-https", multi(HTTPS "/plain.bin", plain, n, sh, 4));
    curl_easy_cleanup(c); curl_slist_free_all(hdr); curl_slist_free_all(resolve);
    CHECK("curl-share-cleanup", curl_share_cleanup(sh) == CURLSHE_OK && nlock == 0);
    curl_global_cleanup();
    return t_fail != 0;
}
