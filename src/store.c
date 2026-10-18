#include <stdlib.h>
#include <string.h>

#include <sodium.h>

#include "platform.h"
#include "store.h"

#define KEY_BYTES   crypto_aead_xchacha20poly1305_ietf_KEYBYTES
#define NONCE_BYTES crypto_aead_xchacha20poly1305_ietf_NPUBBYTES
#define MAC_BYTES   crypto_aead_xchacha20poly1305_ietf_ABYTES

enum {
    FORMAT_VERSION = 1,
    KDF_ARGON2ID13 = 1,
    HEADER_SIZE = 64,
    OFF_OPSLIMIT = 8,
    OFF_MEMLIMIT = 16,
    OFF_SALT = 24,
    OFF_NONCE = 40
};

static const unsigned char MAGIC[4] = { 'D', 'R', 'Y', 0x1A };

/* Limits that protect against a file demanding absurd amounts of resources. */
#define MAX_FILE_SIZE (512L * 1024 * 1024)
#define MAX_OPSLIMIT  ((uint64_t)crypto_pwhash_OPSLIMIT_SENSITIVE * 4)
#define MAX_MEMLIMIT  ((uint64_t)1 << 30)

static void put_u64(unsigned char *p, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (unsigned char)(v >> (8 * i));
}

static uint64_t get_u64(const unsigned char *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v |= (uint64_t)p[i] << (8 * i);
    return v;
}

static char *dup_string(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d)
        memcpy(d, s, n);
    return d;
}

static StoreResult derive_key(Store *s, const char *password)
{
    s->key = sodium_malloc(KEY_BYTES);
    if (!s->key)
        return STORE_ERR_MEMORY;
    if (crypto_pwhash(s->key, KEY_BYTES, password, strlen(password), s->salt,
                      (unsigned long long)s->opslimit, (size_t)s->memlimit,
                      crypto_pwhash_ALG_ARGON2ID13) != 0) {
        sodium_free(s->key);
        s->key = NULL;
        return STORE_ERR_MEMORY;
    }
    return STORE_OK;
}

StoreResult store_create(Store *s, const char *path, const char *password)
{
    memset(s, 0, sizeof *s);
    s->path = dup_string(path);
    if (!s->path)
        return STORE_ERR_MEMORY;
    s->opslimit = crypto_pwhash_OPSLIMIT_MODERATE;
    s->memlimit = crypto_pwhash_MEMLIMIT_MODERATE;
    randombytes_buf(s->salt, sizeof s->salt);
    return derive_key(s, password);
}

static StoreResult read_file(const char *path, unsigned char **data, size_t *len)
{
    FILE *f = platform_fopen(path, "rb");
    long size;
    unsigned char *buf;

    if (!f)
        return STORE_ERR_IO;
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return STORE_ERR_IO;
    }
    if (size > MAX_FILE_SIZE) {
        fclose(f);
        return STORE_ERR_FORMAT;
    }
    buf = malloc(size > 0 ? (size_t)size : 1);
    if (!buf) {
        fclose(f);
        return STORE_ERR_MEMORY;
    }
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        return STORE_ERR_IO;
    }
    fclose(f);
    *data = buf;
    *len = (size_t)size;
    return STORE_OK;
}

StoreResult store_open(Store *s, const char *path, const char *password,
                       unsigned char **plain, size_t *plain_len)
{
    unsigned char *file, *m;
    size_t file_len, clen;
    unsigned long long mlen;
    StoreResult r;

    memset(s, 0, sizeof *s);
    r = read_file(path, &file, &file_len);
    if (r != STORE_OK)
        return r;

    if (file_len < HEADER_SIZE + MAC_BYTES || memcmp(file, MAGIC, sizeof MAGIC) != 0 ||
        file[4] != FORMAT_VERSION || file[5] != KDF_ARGON2ID13) {
        free(file);
        return STORE_ERR_FORMAT;
    }
    s->opslimit = get_u64(file + OFF_OPSLIMIT);
    s->memlimit = get_u64(file + OFF_MEMLIMIT);
    if (s->opslimit < crypto_pwhash_OPSLIMIT_MIN || s->opslimit > MAX_OPSLIMIT ||
        s->memlimit < crypto_pwhash_MEMLIMIT_MIN || s->memlimit > MAX_MEMLIMIT) {
        free(file);
        return STORE_ERR_FORMAT;
    }
    memcpy(s->salt, file + OFF_SALT, sizeof s->salt);

    r = derive_key(s, password);
    if (r != STORE_OK) {
        free(file);
        return r;
    }

    clen = file_len - HEADER_SIZE;
    m = malloc(clen - MAC_BYTES + 1);
    if (!m) {
        free(file);
        store_close(s);
        return STORE_ERR_MEMORY;
    }
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(m, &mlen, NULL, file + HEADER_SIZE, clen,
                                                   file, HEADER_SIZE, file + OFF_NONCE,
                                                   s->key) != 0) {
        free(m);
        free(file);
        store_close(s);
        return STORE_ERR_AUTH;
    }
    free(file);

    s->path = dup_string(path);
    if (!s->path) {
        sodium_memzero(m, (size_t)mlen);
        free(m);
        store_close(s);
        return STORE_ERR_MEMORY;
    }
    *plain = m;
    *plain_len = (size_t)mlen;
    return STORE_OK;
}

StoreResult store_save(const Store *s, const unsigned char *plain, size_t len)
{
    size_t total = HEADER_SIZE + len + MAC_BYTES;
    size_t path_len = strlen(s->path);
    unsigned char *buf;
    char *tmp;
    FILE *f;
    bool ok;

    buf = malloc(total);
    tmp = malloc(path_len + 5);
    if (!buf || !tmp) {
        free(buf);
        free(tmp);
        return STORE_ERR_MEMORY;
    }

    memset(buf, 0, HEADER_SIZE);
    memcpy(buf, MAGIC, sizeof MAGIC);
    buf[4] = FORMAT_VERSION;
    buf[5] = KDF_ARGON2ID13;
    put_u64(buf + OFF_OPSLIMIT, s->opslimit);
    put_u64(buf + OFF_MEMLIMIT, s->memlimit);
    memcpy(buf + OFF_SALT, s->salt, sizeof s->salt);
    randombytes_buf(buf + OFF_NONCE, NONCE_BYTES);

    crypto_aead_xchacha20poly1305_ietf_encrypt(buf + HEADER_SIZE, NULL, plain, len,
                                               buf, HEADER_SIZE, NULL, buf + OFF_NONCE,
                                               s->key);

    /* Write to a temporary file, then swap – a crash midway cannot destroy the diary. */
    memcpy(tmp, s->path, path_len);
    memcpy(tmp + path_len, ".tmp", 5);
    f = platform_fopen(tmp, "wb");
    ok = f != NULL;
    if (ok)
        ok = fwrite(buf, 1, total, f) == total && platform_sync_file(f);
    if (f && fclose(f) != 0)
        ok = false;
    if (ok)
        ok = platform_replace_file(tmp, s->path);
    if (!ok)
        platform_remove_file(tmp);

    free(buf);
    free(tmp);
    return ok ? STORE_OK : STORE_ERR_IO;
}

bool store_check_password(const Store *s, const char *password)
{
    Store probe;
    bool ok;

    memset(&probe, 0, sizeof probe);
    probe.opslimit = s->opslimit;
    probe.memlimit = s->memlimit;
    memcpy(probe.salt, s->salt, sizeof probe.salt);
    if (derive_key(&probe, password) != STORE_OK)
        return false;
    ok = sodium_memcmp(probe.key, s->key, KEY_BYTES) == 0;
    sodium_free(probe.key);
    return ok;
}

StoreResult store_change_password(Store *s, const char *password,
                                  const unsigned char *plain, size_t len)
{
    Store next;
    StoreResult r;

    /* Old files are upgraded to the current Argon2 limits on the way. */
    memset(&next, 0, sizeof next);
    next.path = s->path;
    next.opslimit = crypto_pwhash_OPSLIMIT_MODERATE;
    next.memlimit = crypto_pwhash_MEMLIMIT_MODERATE;
    randombytes_buf(next.salt, sizeof next.salt);
    r = derive_key(&next, password);
    if (r != STORE_OK)
        return r;
    r = store_save(&next, plain, len);
    if (r != STORE_OK) {
        sodium_free(next.key);
        return r;
    }
    sodium_free(s->key);
    s->key = next.key;
    s->opslimit = next.opslimit;
    s->memlimit = next.memlimit;
    memcpy(s->salt, next.salt, sizeof s->salt);
    return STORE_OK;
}

void store_close(Store *s)
{
    if (s->key)
        sodium_free(s->key);
    free(s->path);
    memset(s, 0, sizeof *s);
}

const char *store_strerror(StoreResult r)
{
    switch (r) {
    case STORE_OK:         return "OK";
    case STORE_ERR_IO:     return "file read or write error";
    case STORE_ERR_FORMAT: return "not a dry diary file, or the file is damaged";
    case STORE_ERR_AUTH:   return "wrong password or damaged file";
    case STORE_ERR_MEMORY: return "out of memory";
    }
    return "unknown error";
}
