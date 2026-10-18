#include <pthread.h>
#include <psa/crypto.h>

static pthread_once_t crypto_once = PTHREAD_ONCE_INIT;
static psa_status_t crypto_status = PSA_ERROR_BAD_STATE;

static void initialize_crypto(void) {
    crypto_status = psa_crypto_init();
}

int microhad_crypto_init(void) {
    if (pthread_once(&crypto_once, initialize_crypto) != 0) return -1;
    return (int)crypto_status;
}
