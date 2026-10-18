#ifndef MICROHAD_COMPANION_DISCOVERY_H
#define MICROHAD_COMPANION_DISCOVERY_H
#include <string>
#include <vector>
#include <stdint.h>

/* Main-loop-owned, bounded IPv4 mDNS advertiser. No gateway state or flash writes. */
class CompanionDiscovery {
public:
    explicit CompanionDiscovery(unsigned short port);
    ~CompanionDiscovery();
    void poll();
    static bool readName(const std::string& packet, size_t* offset, std::string* name);
    static std::string records(const std::string& label, uint32_t address, unsigned short port, unsigned long ttl);
private:
    struct Interface {
        uint32_t address;
        std::string label;
        unsigned int suffix;
        unsigned int probes;
        bool announced;
        uint64_t due;
    };
    int fd_;
    unsigned short port_;
    uint64_t refresh_;
    uint64_t lastReply_;
    std::vector<Interface> interfaces_;
    void refresh(uint64_t now);
    void send(const Interface& interface, const std::string& packet);
    void closeSocket();
    CompanionDiscovery(const CompanionDiscovery&);
    CompanionDiscovery& operator=(const CompanionDiscovery&);
};
#endif
