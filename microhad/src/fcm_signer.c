#include <stddef.h>
#include <string.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>

int microhad_fcm_sign(const char* pem, const unsigned char* input, size_t input_length,
                  unsigned char* signature, size_t* signature_length) {
    unsigned char hash[32];
    mbedtls_pk_context key;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    int result;
    mbedtls_pk_init(&key);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);
    result = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                                   (const unsigned char*)"microhad-zigbee-fcm", 15);
    if (result == 0) result = mbedtls_pk_parse_key(&key, (const unsigned char*)pem, strlen(pem) + 1, 0, 0);
    if (result == 0) result = mbedtls_sha256_ret(input, input_length, hash, 0);
    if (result == 0) result = mbedtls_pk_sign(&key, MBEDTLS_MD_SHA256, hash, sizeof(hash), signature,
                                               signature_length, mbedtls_ctr_drbg_random, &drbg);
    mbedtls_pk_free(&key);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);
    return result;
}
