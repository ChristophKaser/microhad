#include <stddef.h>
#include <string.h>
#include <psa/crypto.h>
#include <mbedtls/pk.h>

extern int microhad_crypto_init(void);

int microhad_fcm_sign(const char* pem, const unsigned char* input, size_t input_length,
                     unsigned char* signature, size_t* signature_length) {
    unsigned char hash[32];
    size_t hash_length = 0;
    mbedtls_pk_context key;
    int result = microhad_crypto_init();
    mbedtls_pk_init(&key);
    if (result == 0) result = mbedtls_pk_parse_key(&key, (const unsigned char*)pem, strlen(pem) + 1, 0, 0);
    if (result == 0 && (!mbedtls_pk_can_do_psa(&key, PSA_ALG_RSA_PKCS1V15_SIGN(PSA_ALG_SHA_256), PSA_KEY_USAGE_SIGN_HASH) || mbedtls_pk_get_bitlen(&key) > 4096)) result = -1;
    if (result == 0) result = psa_hash_compute(PSA_ALG_SHA_256, input, input_length, hash, sizeof(hash), &hash_length);
    /* The caller provides 512 bytes for RSA keys up to 4096 bits. */
    if (result == 0) result = mbedtls_pk_sign_ext(MBEDTLS_PK_SIGALG_RSA_PKCS1V15, &key, MBEDTLS_MD_SHA256, hash, hash_length,
                                            signature, 512, signature_length);
    mbedtls_pk_free(&key);
    return result;
}
