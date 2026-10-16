// Opt-in diagnostics for the pinned Linux/OpenSSL CHIP Tool (raw session
// keystore). Installed into the ignored SDK checkout by build-controller.sh.
#pragma once

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

namespace chip {
namespace DiagnosticKeys {

inline bool Enabled()
{
    return std::getenv("MATTER_DIAGNOSTIC_DIR") != nullptr;
}

inline std::string Hex(const uint8_t * data, size_t size)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(size * 2);
    for (size_t i = 0; i < size; ++i)
    {
        result += digits[data[i] >> 4];
        result += digits[data[i] & 15];
    }
    return result;
}

inline std::string Quote(const char * value)
{
    std::string result = "\"";
    for (const unsigned char * p = reinterpret_cast<const unsigned char *>(value); *p; ++p)
    {
        if (*p == '\\' || *p == '"')
        {
            result += '\\';
        }
        if (*p < 32)
        {
            char escaped[7];
            std::snprintf(escaped, sizeof(escaped), "\\u%04x", *p);
            result += escaped;
        }
        else
        {
            result += static_cast<char>(*p);
        }
    }
    return result + "\"";
}

inline void Fail()
{
    std::perror("Matter diagnostic export failed");
    std::abort(); // An explicitly requested export must never fail silently.
}

inline void Write(const char * relativePath, uint64_t context, const std::string & fields)
{
    const char * directory = std::getenv("MATTER_DIAGNOSTIC_DIR");
    if (directory == nullptr)
    {
        return;
    }
    static std::mutex lock;
    std::lock_guard<std::mutex> guard(lock);
    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now) != 0)
    {
        Fail();
    }
    const char * build = std::getenv("MATTER_DIAGNOSTIC_BUILD");
    std::string record = "{\"schema\":1,\"attempt\":" + Quote(directory) +
        ",\"controller_build\":" + (build ? Quote(build) : "null") + ",\"pid\":" + std::to_string(getpid()) +
        ",\"context\":" + std::to_string(context) +
        ",\"unix_ns\":" + std::to_string(static_cast<uint64_t>(now.tv_sec) * 1000000000ULL + static_cast<uint64_t>(now.tv_nsec)) +
        "," + fields + "}\n";
    std::string path = std::string(directory) + "/" + relativePath;
    int fd           = open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    struct stat info;
    if (fd < 0 || fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_uid != getuid() || (info.st_mode & 077) != 0)
    {
        Fail();
    }
    size_t offset = 0;
    while (offset < record.size())
    {
        ssize_t count = write(fd, record.data() + offset, record.size() - offset);
        if (count < 0 && errno == EINTR)
        {
            continue;
        }
        if (count <= 0)
        {
            Fail();
        }
        offset += static_cast<size_t>(count);
    }
    if (fsync(fd) != 0 || close(fd) != 0)
    {
        Fail();
    }
}

inline uint64_t Keys(const char * type, const uint8_t * tx, const uint8_t * rx, const uint8_t * challenge, size_t challengeSize,
                     bool initiator, bool resumed)
{
    if (!Enabled())
    {
        return 0;
    }
    static std::atomic<uint64_t> sequence{ 0 };
    uint64_t context = ++sequence;
    Write("keys/matter-session-keys.jsonl", context,
          "\"event\":\"keys\",\"type\":" + Quote(type) +
              ",\"local_session_id\":null,\"peer_session_id\":null,"
              "\"local_node_id\":null,\"peer_node_id\":null,\"peer_address\":null,"
              "\"algorithm\":\"AES-128-CCM\",\"role\":" +
              Quote(initiator ? "initiator" : "responder") + ",\"resumed\":" + (resumed ? "true" : "false") + ",\"tx_key\":\"" +
              Hex(tx, 16) + "\",\"rx_key\":\"" + Hex(rx, 16) + "\",\"attestation_challenge\":\"" + Hex(challenge, challengeSize) +
              "\"");
    return context;
}

inline void Session(uint64_t context, const char * type, uint16_t localId, uint16_t peerId, uint64_t localNode, uint64_t peerNode,
                    const char * address)
{
    if (context != 0)
    {
        Write("keys/matter-session-keys.jsonl", context,
              "\"event\":\"session\",\"type\":" + Quote(type) + ",\"local_session_id\":" + std::to_string(localId) +
                  ",\"peer_session_id\":" + std::to_string(peerId) + ",\"local_node_id\":" + std::to_string(localNode) +
                  ",\"peer_node_id\":" + std::to_string(peerNode) + ",\"peer_address\":" + Quote(address));
    }
}

inline void Message(uint64_t context, const char * direction, const uint8_t * nonce, size_t nonceSize, const uint8_t * aad,
                    size_t aadSize, const uint8_t * cipher, size_t size, const uint8_t * tag, size_t tagSize,
                    const std::string & plaintext)
{
    if (context != 0)
    {
        Write("capture/matter-messages.jsonl", context,
              "\"event\":\"message\",\"direction\":" + Quote(direction) + ",\"nonce\":\"" + Hex(nonce, nonceSize) +
                  "\",\"aad\":\"" + Hex(aad, aadSize) + "\",\"ciphertext\":\"" + Hex(cipher, size) + "\",\"tag\":\"" +
                  Hex(tag, tagSize) + "\",\"plaintext\":" + (plaintext.empty() ? "null" : Quote(plaintext.c_str())));
    }
}

} // namespace DiagnosticKeys
} // namespace chip
