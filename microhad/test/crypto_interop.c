/* Host-only interoperability driver. Its temporary keys must never be deployed. */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int microhad_vapid_generate(unsigned char*, unsigned char*);
extern int microhad_vapid_sign(const unsigned char*, const unsigned char*, unsigned long, unsigned char*);
extern int microhad_web_push_encrypt(const unsigned char*, const unsigned char*, const unsigned char*,
                                    unsigned long, unsigned char*, unsigned long, unsigned long*);
extern int microhad_fcm_sign(const char*, const unsigned char*, size_t, unsigned char*, size_t*);

static const unsigned char message[] = "MicroHAD v4 interoperability\x02";

static void print_hex(const char* name, const unsigned char* bytes, size_t length) {
    size_t i;
    printf("%s=", name);
    for (i = 0; i < length; ++i) printf("%02x", bytes[i]);
    putchar('\n');
}

static void* concurrent_crypto(void* unused) {
    unsigned char public_key[65], private_key[32], signature[64];
    unsigned int i;
    (void)unused;
    for (i = 0; i < 8; ++i) {
        assert(microhad_vapid_generate(public_key, private_key) == 0);
        assert(microhad_vapid_sign(private_key, message, sizeof(message)-1, signature) == 0);
    }
    return 0;
}

int main(int argc, char** argv) {
    unsigned char public_key[65], private_key[32], signature[512], encrypted[4200];
    unsigned char auth[16] = {0}, invalid_public[65] = {0}, large[4095];
    unsigned long encrypted_length = 0;
    size_t signature_length = 0, pem_length;
    char pem[8192];
    FILE* file;
    pthread_t threads[8];
    unsigned int i;
    assert(argc == 2 || argc == 3);
    /* Race the first initialization as well as volatile key creation/destruction. */
    for (i = 0; i < 8; ++i) assert(pthread_create(&threads[i], 0, concurrent_crypto, 0) == 0);
    for (i = 0; i < 8; ++i) assert(pthread_join(threads[i], 0) == 0);
    file = fopen(argv[1], "rb");
    assert(file != 0);
    pem_length = fread(pem, 1, sizeof(pem)-1, file);
    assert(!ferror(file) && feof(file));
    assert(fclose(file) == 0);
    pem[pem_length] = 0;
    if (argc == 3) {
        assert(microhad_fcm_sign(pem, message, sizeof(message)-1, signature, &signature_length) != 0);
        return 0;
    }
    assert(microhad_fcm_sign(pem, message, sizeof(message)-1, signature, &signature_length) == 0);
    print_hex("rsa_signature", signature, signature_length);
    assert(microhad_vapid_generate(public_key, private_key) == 0);
    print_hex("public", public_key, sizeof(public_key));
    print_hex("private", private_key, sizeof(private_key));
    assert(microhad_vapid_sign(private_key, message, sizeof(message)-1, signature) == 0);
    print_hex("ecdsa_signature", signature, 64);
    assert(microhad_web_push_encrypt(public_key, auth, message, sizeof(message)-1,
                                    encrypted, sizeof(encrypted), &encrypted_length) == 0);
    print_hex("encrypted", encrypted, encrypted_length);
    memset(large, 'x', sizeof(large));
    large[sizeof(large)-1] = 2;
    assert(microhad_web_push_encrypt(public_key, auth, large, sizeof(large),
                                    encrypted, sizeof(encrypted), &encrypted_length) == 0);
    print_hex("large_encrypted", encrypted, encrypted_length);
    assert(microhad_web_push_encrypt(invalid_public, auth, message, sizeof(message)-1,
                                    encrypted, sizeof(encrypted), &encrypted_length) != 0);
    assert(microhad_web_push_encrypt(public_key, auth, message, sizeof(message)-1,
                                    encrypted, 100, &encrypted_length) != 0);
    assert(microhad_web_push_encrypt(public_key, auth, large, 4096,
                                    encrypted, sizeof(encrypted), &encrypted_length) != 0);
    memset(private_key, 0, sizeof(private_key));
    assert(microhad_vapid_sign(private_key, message, sizeof(message)-1, signature) != 0);
    print_hex("message", message, sizeof(message)-1);
    return 0;
}
