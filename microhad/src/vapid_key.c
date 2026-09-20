#include <stddef.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>

int microhad_vapid_generate(unsigned char* public_key, unsigned char* private_key) {
    mbedtls_ecp_keypair key;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    int result;
    mbedtls_ecp_keypair_init(&key); mbedtls_entropy_init(&entropy); mbedtls_ctr_drbg_init(&drbg);
    result = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, (const unsigned char*)"microhad-vapid", 10);
    if (result == 0) result = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, &key, mbedtls_ctr_drbg_random, &drbg);
    if (result == 0) result = mbedtls_mpi_write_binary(&key.d, private_key, 32);
    if (result == 0) { size_t length = 0; result = mbedtls_ecp_point_write_binary(&key.grp, &key.Q, MBEDTLS_ECP_PF_UNCOMPRESSED, &length, public_key, 65); if (length != 65) result = -1; }
    mbedtls_ecp_keypair_free(&key); mbedtls_ctr_drbg_free(&drbg); mbedtls_entropy_free(&entropy);
    return result;
}
