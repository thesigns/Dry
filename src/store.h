/* store.h – the encrypted diary file.
 *
 * File format (integers are little-endian):
 *   0   4  magic "DRY\x1A"
 *   4   1  format version (1)
 *   5   1  KDF algorithm (1 = Argon2id v1.3)
 *   6   2  reserved (0)
 *   8   8  Argon2 opslimit
 *   16  8  Argon2 memlimit
 *   24  16 salt
 *   40  24 XChaCha20-Poly1305 nonce
 *   64  ... ciphertext + 16-byte MAC
 * The whole 64-byte header is authenticated as additional data (AAD). */
#ifndef DRY_STORE_H
#define DRY_STORE_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    STORE_OK,
    STORE_ERR_IO,
    STORE_ERR_FORMAT,
    STORE_ERR_AUTH,
    STORE_ERR_MEMORY
} StoreResult;

typedef struct {
    char *path;
    unsigned char *key; /* protected memory (sodium_malloc) */
    uint64_t opslimit;
    uint64_t memlimit;
    unsigned char salt[16];
} Store;

/* New diary: generates a random salt and derives the key (writes nothing). */
StoreResult store_create(Store *s, const char *path, const char *password);

/* Opens an existing file and decrypts its contents into *plain (malloc). */
StoreResult store_open(Store *s, const char *path, const char *password,
                       unsigned char **plain, size_t *plain_len);

/* Encrypts and atomically writes the contents (always with a fresh nonce). */
StoreResult store_save(const Store *s, const unsigned char *plain, size_t len);

void store_close(Store *s);
const char *store_strerror(StoreResult r);

#endif
