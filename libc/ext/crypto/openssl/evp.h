/* openssl/evp.h: the part of OpenSSL's digest interface that ufsm uses, over our own SHA-256 (evp.c).
   Not OpenSSL: only SHA-256 (and SHA-224) exist. Functions return 1 for success and 0 for failure, as OpenSSL's do. */
#ifndef _ULIBS_OPENSSL_EVP_H
#define _ULIBS_OPENSSL_EVP_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
#define EVP_MAX_MD_SIZE 64
#define EVP_MAX_BLOCK_LENGTH 32
#define SHA256_DIGEST_LENGTH 32
typedef struct evp_md_st EVP_MD;
typedef struct evp_md_ctx_st EVP_MD_CTX;
typedef struct engine_st ENGINE;
const EVP_MD *EVP_sha256(void);
const EVP_MD *EVP_sha224(void);
int EVP_MD_get_size(const EVP_MD *md);
int EVP_MD_get_block_size(const EVP_MD *md);
#define EVP_MD_size EVP_MD_get_size
#define EVP_MD_block_size EVP_MD_get_block_size
EVP_MD_CTX *EVP_MD_CTX_new(void);
void EVP_MD_CTX_free(EVP_MD_CTX *ctx);
int EVP_MD_CTX_reset(EVP_MD_CTX *ctx);
int EVP_MD_CTX_copy_ex(EVP_MD_CTX *out, const EVP_MD_CTX *in);
#define EVP_MD_CTX_create EVP_MD_CTX_new
#define EVP_MD_CTX_destroy EVP_MD_CTX_free
int EVP_DigestInit_ex(EVP_MD_CTX *ctx, const EVP_MD *type, ENGINE *impl);
int EVP_DigestInit(EVP_MD_CTX *ctx, const EVP_MD *type);
int EVP_DigestUpdate(EVP_MD_CTX *ctx, const void *d, size_t cnt);
int EVP_DigestFinal_ex(EVP_MD_CTX *ctx, unsigned char *md, unsigned int *s);
int EVP_DigestFinal(EVP_MD_CTX *ctx, unsigned char *md, unsigned int *s);
int EVP_Digest(const void *data, size_t count, unsigned char *md, unsigned int *size, const EVP_MD *type, ENGINE *impl);
unsigned char *SHA256(const unsigned char *d, size_t n, unsigned char *md);
#ifdef __cplusplus
}
#endif
#endif
