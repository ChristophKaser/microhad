#ifndef MICROHAD_NETWORK_BACKUP_H
#define MICROHAD_NETWORK_BACKUP_H
#include <deque>
#include <string>
#include <vector>

// Read-only EZSP v13 export. Never sends reset, token writes or network formation.
class NetworkBackupExport {
public:
    struct Query {
        unsigned short command;
        std::vector<unsigned char> payload;
        Query(unsigned short c, const std::vector<unsigned char>& p) : command(c), payload(p) {}
    };
    NetworkBackupExport();
    ~NetworkBackupExport();
    bool start(const std::string& id, const std::string& directory, std::string* error);
    bool active() const { return fd_ >= 0; }
    bool next(Query* query);
    bool accept(const Query& query, const std::vector<unsigned char>& response, std::string* error);
    bool finish(std::string* error);
    void abort();
private:
    int fd_;
    unsigned long bytes_;
    bool first_;
    bool closing_;
    std::string partial_;
    std::string final_;
    std::deque<Query> queries_;
    void enqueue(unsigned short command, const std::vector<unsigned char>& payload);
    bool append(const std::string& data);
    NetworkBackupExport(const NetworkBackupExport&);
    NetworkBackupExport& operator=(const NetworkBackupExport&);
};
#endif
