#include <stddef.h>
#include <psa/crypto.h>

extern int microhad_crypto_init(void);

int microhad_vapid_generate(unsigned char* public_key, unsigned char* private_key) {
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_key_id_t key = 0;
    size_t length = 0;
    int result = microhad_crypto_init();
    psa_set_key_type(&attributes, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attributes, 256);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_EXPORT);
    if (result == 0) result = psa_generate_key(&attributes, &key);
    if (result == 0) result = psa_export_key(key, private_key, 32, &length);
    if (result == 0 && length != 32) result = -1;
    if (result == 0) result = psa_export_public_key(key, public_key, 65, &length);
    if (result == 0 && length != 65) result = -1;
    psa_destroy_key(key);
    psa_reset_key_attributes(&attributes);
    return result;
}
