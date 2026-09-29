#ifndef MICROHAD_ZIGBEE_LOG_RING_H
#define MICROHAD_ZIGBEE_LOG_RING_H

#include <string>
#include <vector>
#include <stdint.h>

/* RAM-backed, single-writer circular log. Readers use their own descriptors
   and never acquire a coordinator-owned lock. */
class LogRing {
public:
    enum { MaximumBytes = 1000000 };
    explicit LogRing(const std::string& path);
    ~LogRing();
    bool initialize();
    bool append(const std::string& line);
    static bool readLines(const std::string& path, std::vector<std::string>* lines);
    static bool readJson(const std::string& path, std::string* json);
    static bool readJson(const std::string& path, unsigned long since, std::string* json);
    static const char* defaultPath();

private:
    struct Header {
        uint32_t generation;
        uint32_t head;
        uint32_t tail;
        uint32_t used;
        uint32_t lastSequence;
        Header() : generation(0), head(0), tail(0), used(0), lastSequence(0) {}
    };
    std::string path_;
    int fd_;
    Header header_;
    bool createEmpty(int fd);
    bool appendOpened(const std::string& line);
    bool advanceTail();
    bool commit();
    LogRing(const LogRing&);
    LogRing& operator=(const LogRing&);
};

#endif
