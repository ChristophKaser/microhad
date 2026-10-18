#include <stddef.h>
#include <string.h>

#include <psa/crypto.h>
#include <mbedtls/platform_util.h>

extern int microhad_crypto_init(void);

static int hmac_sha256(const unsigned char* key, size_t key_length, const unsigned char* input,
                       size_t input_length, unsigned char* output) {
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_key_id_t id = 0;
    size_t length = 0;
    int result;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_HMAC);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attributes, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    result = psa_import_key(&attributes, key, key_length, &id);
    if (result == 0) result = psa_mac_compute(id, PSA_ALG_HMAC(PSA_ALG_SHA_256), input, input_length, output, 32, &length);
    if (result == 0 && length != 32) result = -1;
    psa_destroy_key(id);
    psa_reset_key_attributes(&attributes);
    return result;
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
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_key_id_t ephemeral = 0, aes = 0;
    size_t length = 0;
    unsigned char shared[32], prk[32], ikm[32], salt[16], cek[16], nonce[12];
    unsigned char server_public[65], webpush_info[144];
    const unsigned char cek_info[] = "Content-Encoding: aes128gcm\0";
    const unsigned char nonce_info[] = "Content-Encoding: nonce\0";
    size_t server_public_length = 0;
    size_t webpush_info_length;
    size_t ciphertext_length;
    int result = -1;
    if (plaintext_length > 4095 || output_capacity < plaintext_length + 102) return -1;
    result = microhad_crypto_init();
    psa_set_key_type(&attributes, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attributes, 256);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_DERIVE);
    psa_set_key_algorithm(&attributes, PSA_ALG_ECDH);
    if (result == 0) result = psa_generate_key(&attributes, &ephemeral);
    if (result == 0) result = psa_raw_key_agreement(PSA_ALG_ECDH, ephemeral, client_public, 65,
                                                  shared, sizeof(shared), &length);
    if (result == 0 && length != sizeof(shared)) result = -1;
    if (result == 0) result = hmac_sha256(auth_secret, 16, shared, sizeof(shared), prk);
    if (result == 0) {
        memcpy(webpush_info, "WebPush: info\0", 14);
        memcpy(webpush_info + 14, client_public, 65);
        result = psa_export_public_key(ephemeral, server_public, sizeof(server_public), &server_public_length);
        if (result == 0 && server_public_length != 65) result = -1;
        if (result == 0) memcpy(webpush_info + 79, server_public, 65);
    }
    webpush_info_length = 144;
    if (result == 0) result = hkdf_expand(prk, webpush_info, webpush_info_length, ikm, sizeof(ikm));
    if (result == 0) result = psa_generate_random(salt, sizeof(salt));
    if (result == 0) result = hmac_sha256(salt, sizeof(salt), ikm, sizeof(ikm), prk);
    if (result == 0) result = hkdf_expand(prk, cek_info, sizeof(cek_info) - 1, cek, sizeof(cek));
    if (result == 0) result = hkdf_expand(prk, nonce_info, sizeof(nonce_info) - 1, nonce, sizeof(nonce));
    ciphertext_length = plaintext_length;
    psa_reset_key_attributes(&attributes);
    psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attributes, 128);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_ENCRYPT);
    psa_set_key_algorithm(&attributes, PSA_ALG_GCM);
    if (result == 0) result = psa_import_key(&attributes, cek, sizeof(cek), &aes);
    if (result == 0) result = psa_aead_encrypt(aes, PSA_ALG_GCM, nonce, sizeof(nonce), 0, 0,
                                              plaintext, plaintext_length, output + 86,
                                              output_capacity - 86, &length);
    if (result == 0 && length != plaintext_length + 16) result = -1;
    if (result == 0) {
        memcpy(output, salt, sizeof(salt));
        output[16] = 0; output[17] = 0; output[18] = 16; output[19] = 0;
        output[20] = 65;
        memcpy(output + 21, server_public, sizeof(server_public));
        *output_length = (unsigned long)(102 + ciphertext_length);
    }
    psa_destroy_key(aes);
    psa_destroy_key(ephemeral);
    psa_reset_key_attributes(&attributes);
    mbedtls_platform_zeroize(shared, sizeof(shared));
    mbedtls_platform_zeroize(prk, sizeof(prk));
    mbedtls_platform_zeroize(ikm, sizeof(ikm));
    mbedtls_platform_zeroize(cek, sizeof(cek));
    return result;
}

int microhad_vapid_sign(const unsigned char* private_key, const unsigned char* input, unsigned long input_length,
                    unsigned char* signature) {
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_key_id_t key = 0;
    size_t length = 0;
    int result = microhad_crypto_init();
    psa_set_key_type(&attributes, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attributes, 256);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attributes, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
    if (result == 0) result = psa_import_key(&attributes, private_key, 32, &key);
    if (result == 0) result = psa_sign_message(key, PSA_ALG_ECDSA(PSA_ALG_SHA_256), input, input_length,
                                              signature, 64, &length);
    if (result == 0 && length != 64) result = -1;
    psa_destroy_key(key);
    psa_reset_key_attributes(&attributes);
    return result;
}
