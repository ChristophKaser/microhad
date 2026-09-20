#include <stddef.h>
#include <string.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/entropy.h>
#include <mbedtls/gcm.h>
#include <mbedtls/md.h>
#include <mbedtls/sha256.h>

static int hmac_sha256(const unsigned char* key, size_t key_length, const unsigned char* input,
                       size_t input_length, unsigned char* output) {
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    return mbedtls_md_hmac(info, key, key_length, input, input_length, output);
}

static int hkdf_expand(const unsigned char* prk, const unsigned char* info, size_t info_length,
                       unsigned char* output, size_t output_length) {
    unsigned char block[32];
    unsigned char input[256];
    size_t block_length = 0;
    size_t copied = 0;
    unsigned char counter = 1;
    int result = 0;
    if (info_length + 33 > sizeof(input)) return -1;
    while (copied < output_length) {
        memcpy(input, block, block_length);
        memcpy(input + block_length, info, info_length);
        input[block_length + info_length] = counter;
        result = hmac_sha256(prk, 32, input, block_length + info_length + 1, block);
        if (result != 0) return result;
        block_length = sizeof(block);
        if (output_length - copied < sizeof(block)) block_length = output_length - copied;
        memcpy(output + copied, block, block_length);
        copied += block_length;
        counter++;
    }
    return 0;
}

int microhad_web_push_encrypt(const unsigned char* client_public, const unsigned char* auth_secret,
                          const unsigned char* plaintext, unsigned long plaintext_length,
                          unsigned char* output, unsigned long output_capacity, unsigned long* output_length) {
    mbedtls_ecdh_context context;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_gcm_context gcm;
    unsigned char shared[32], prk[32], ikm[32], salt[16], cek[16], nonce[12], tag[16];
    unsigned char server_public[65], webpush_info[144];
    const unsigned char cek_info[] = "Content-Encoding: aes128gcm\0";
    const unsigned char nonce_info[] = "Content-Encoding: nonce\0";
    size_t server_public_length = 0;
    size_t webpush_info_length;
    size_t ciphertext_length;
    int result = -1;
    if (plaintext_length > 4095 || output_capacity < plaintext_length + 102) return -1;
    mbedtls_ecdh_init(&context);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);
    mbedtls_gcm_init(&gcm);
    result = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                                   (const unsigned char*)"microhad-web-push", 13);
    if (result == 0) result = mbedtls_ecp_group_load(&context.grp, MBEDTLS_ECP_DP_SECP256R1);
    if (result == 0) result = mbedtls_ecp_point_read_binary(&context.grp, &context.Qp, client_public, 65);
    if (result == 0) result = mbedtls_ecp_gen_keypair(&context.grp, &context.d, &context.Q,
                                                       mbedtls_ctr_drbg_random, &drbg);
    if (result == 0) result = mbedtls_ecdh_compute_shared(&context.grp, &context.z, &context.Qp, &context.d,
                                                           mbedtls_ctr_drbg_random, &drbg);
    if (result == 0) result = mbedtls_mpi_write_binary(&context.z, shared, sizeof(shared));
    if (result == 0) result = hmac_sha256(auth_secret, 16, shared, sizeof(shared), prk);
    if (result == 0) {
        memcpy(webpush_info, "WebPush: info\0", 14);
        memcpy(webpush_info + 14, client_public, 65);
        result = mbedtls_ecp_point_write_binary(&context.grp, &context.Q, MBEDTLS_ECP_PF_UNCOMPRESSED,
                                                &server_public_length, server_public, sizeof(server_public));
        if (result == 0 && server_public_length != 65) result = -1;
        if (result == 0) memcpy(webpush_info + 79, server_public, 65);
    }
    webpush_info_length = 144;
    if (result == 0) result = hkdf_expand(prk, webpush_info, webpush_info_length, ikm, sizeof(ikm));
    if (result == 0) result = mbedtls_ctr_drbg_random(&drbg, salt, sizeof(salt));
    if (result == 0) result = hmac_sha256(salt, sizeof(salt), ikm, sizeof(ikm), prk);
    if (result == 0) result = hkdf_expand(prk, cek_info, sizeof(cek_info) - 1, cek, sizeof(cek));
    if (result == 0) result = hkdf_expand(prk, nonce_info, sizeof(nonce_info) - 1, nonce, sizeof(nonce));
    ciphertext_length = plaintext_length;
    if (result == 0) result = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, cek, 128);
    if (result == 0) result = mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, plaintext_length, nonce,
                                                         sizeof(nonce), 0, 0, plaintext, output + 86, sizeof(tag), tag);
    if (result == 0) {
        memcpy(output, salt, sizeof(salt));
        output[16] = 0; output[17] = 0; output[18] = 16; output[19] = 0;
        output[20] = 65;
        memcpy(output + 21, server_public, sizeof(server_public));
        memcpy(output + 86 + ciphertext_length, tag, sizeof(tag));
        *output_length = (unsigned long)(102 + ciphertext_length);
    }
    mbedtls_gcm_free(&gcm);
    mbedtls_ecdh_free(&context);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);
    return result;
}

int microhad_vapid_sign(const unsigned char* private_key, const unsigned char* input, unsigned long input_length,
                    unsigned char* signature) {
    mbedtls_ecp_keypair key;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_mpi r, s;
    unsigned char hash[32];
    int result;
    mbedtls_ecp_keypair_init(&key);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);
    mbedtls_mpi_init(&r); mbedtls_mpi_init(&s);
    result = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                                   (const unsigned char*)"microhad-vapid-sign", 15);
    if (result == 0) result = mbedtls_ecp_group_load(&key.grp, MBEDTLS_ECP_DP_SECP256R1);
    if (result == 0) result = mbedtls_mpi_read_binary(&key.d, private_key, 32);
    if (result == 0) result = mbedtls_sha256_ret(input, input_length, hash, 0);
    if (result == 0) result = mbedtls_ecdsa_sign(&key.grp, &r, &s, &key.d, hash, sizeof(hash),
                                                  mbedtls_ctr_drbg_random, &drbg);
    if (result == 0) result = mbedtls_mpi_write_binary(&r, signature, 32);
    if (result == 0) result = mbedtls_mpi_write_binary(&s, signature + 32, 32);
    mbedtls_mpi_free(&r); mbedtls_mpi_free(&s);
    mbedtls_ecp_keypair_free(&key);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);
    return result;
}
