// Test-only SRP SIG(0) replay. Uses a synthetic RCP, never the production UART.
#include "instance/instance.hpp"
#include "net/srp_server.hpp"
#include <cstdio>

extern "C" int gatewayOtInit(const char *);
extern "C" otInstance *gatewayOtDiscoveryTestInstance();
extern "C" otError __real_otPlatCryptoEcdsaVerify(const otPlatCryptoEcdsaPublicKey *,
    const otPlatCryptoSha256Hash *, const otPlatCryptoEcdsaSignature *);

extern "C" otError __wrap_otPlatCryptoEcdsaVerify(const otPlatCryptoEcdsaPublicKey *key,
    const otPlatCryptoSha256Hash *hash, const otPlatCryptoEcdsaSignature *signature)
{
    std::printf("verification_sha256=");
    for (unsigned i = 0; i < 32; ++i) std::printf("%02x", hash->m8[i]);
    std::printf("\n");
    return __real_otPlatCryptoEcdsaVerify(key, hash, signature);
}

int main(int argc, char **argv)
{
    std::setbuf(stdout, NULL);
    if (argc != 4 || gatewayOtInit(argv[1])) return 2;
    uint8_t bytes[2048];
    FILE *file = std::fopen(argv[3], "rb");
    if (!file) return 2;
    size_t length = std::fread(bytes, 1, sizeof(bytes), file);
    std::fclose(file);
    // Fixture format ends with root-owner SIG(0), 84-byte compressed RDATA.
    if (length < 107 || length == sizeof(bytes)) return 2;
    ot::Crypto::Ecdsa::P256::PublicKey key;
    file = std::fopen(argv[2], "rb");
    if (!file) return 2;
    size_t keyLength = std::fread(&key, 1, sizeof(key), file);
    std::fclose(file);
    if (keyLength != sizeof(key)) return 2;

    ot::Instance &instance = ot::AsCoreType(gatewayOtDiscoveryTestInstance());
    ot::Message *message = instance.Get<ot::MessagePool>().Allocate(ot::Message::kTypeIp6);
    if (!message) return 3;
    uint8_t prefix[48] = {};
    if (message->AppendBytes(prefix, sizeof(prefix)) != ot::kErrorNone ||
        message->AppendBytes(bytes, length) != ot::kErrorNone) return 3;
    message->SetOffset(48);
    ot::Dns::UpdateHeader header;
    if (message->Read(48, header) != ot::kErrorNone) return 2;
    uint16_t sigStart = 48 + length - 95;
    uint16_t sigRdata = sigStart + 11;
    uint16_t signerOffset = sigRdata + 18;
    ot::Dns::Name::Buffer signer;
    if (ot::Dns::Name::ReadName(*message, signerOffset, signer) != ot::kErrorNone) return 2;
    ot::Srp::Server &server = instance.Get<ot::Srp::Server>();
    ot::Error valid = server.VerifySignature(key, *message, header, sigStart, sigRdata, 84, signer);
    uint8_t last;
    if (message->Read(message->GetLength() - 1, last) != ot::kErrorNone) return 2;
    last ^= 1;
    message->Write(message->GetLength() - 1, last);
    ot::Error tampered = server.VerifySignature(key, *message, header, sigStart, sigRdata, 84, signer);
    std::printf("SRP valid=%u tampered=%u\n", valid, tampered);
    message->Free();
    return valid == ot::kErrorNone && tampered == ot::kErrorSecurity ? 0 : 1;
}
