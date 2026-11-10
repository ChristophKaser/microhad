#ifndef MICROHAD_OTA_H
#define MICROHAD_OTA_H
#include <string>
#include <vector>
#include <pthread.h>
#include <stdint.h>

/* One temporary image and one explicitly selected requestor. No persistent state. */
class OtaStore {
public:
    static OtaStore& instance();
    bool upload(const std::string& action, const std::string& body, std::string* error);
    std::string status();
    bool startZigbee(const std::string& image, const std::string& address, std::string* error);
    bool zigbee(const std::string& address, unsigned char command,
                const std::vector<unsigned char>& args, std::vector<unsigned char>* response);
    bool cancel(const std::string& image, bool discard, std::string* error);
    bool matterControl(const std::string& action, const std::string& image,
                       const std::string& node, const std::string& socket, std::string* error, unsigned short endpoint = 0);
private:
    OtaStore();
    ~OtaStore();
    OtaStore(const OtaStore&);
    OtaStore& operator=(const OtaStore&);
    pthread_mutex_t mutex_;
    int fd_;
    std::string path_, id_, protocol_, state_, target_, error_, matterSocket_;
    unsigned long size_, received_, progress_;
    uint16_t manufacturer_, type_, minHardware_, maxHardware_;
    uint32_t version_;
    bool hardwareRange_;
    bool zigbeeOffered_;
    long touched_;
    void clear();
    bool parse(std::string* error);
    bool active() const;
    void expire();
};
#endif
