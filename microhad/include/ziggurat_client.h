#ifndef MICROHAD_ZIGGURAT_CLIENT_H
#define MICROHAD_ZIGGURAT_CLIENT_H

#include <deque>
#include <map>
#include <string>
#include <vector>

// Protocol v2: little-endian wire values, COBS messages delimited by zero.
namespace ZigguratWire {
typedef std::vector<unsigned char> Bytes;
Bytes encode(const Bytes& plain);
bool decode(const Bytes& encoded, Bytes* plain);
unsigned long get32(const Bytes& bytes, unsigned long offset);
void put16(Bytes* bytes, unsigned int value);
void set32(Bytes* bytes, unsigned long offset, unsigned long value);
}

struct ZigguratState {
    ZigguratWire::Bytes network; // 82-byte protocol NetworkState
    ZigguratWire::Bytes keys; // counted KeyEntry table
    ZigguratWire::Bytes children; // counted ChildEntry table
    ZigguratWire::Bytes addresses; // counted AddressEntry table
    bool load(const std::string& path, std::string* error);
    bool save(const std::string& path, std::string* error) const;
    bool valid() const;
    bool advance(std::string* error);
};

class ZigguratClient {
public:
    typedef ZigguratWire::Bytes Bytes;
    struct Event {
        enum Type { Ready, Down, ReceivedAps, Joined, Left, Admission, Confirmation, JoinReply, Diagnostic } type;
        unsigned int id;
        unsigned char command;
        unsigned char status;
        Bytes payload;
        std::string message;
        Event(Type value) : type(value), id(0), command(0), status(0) {}
    };
    ZigguratClient();
    ~ZigguratClient();
    bool start(const std::string& socket, const std::string& state, std::string* error);
    void stop();
    void poll();
    int fd() const { return fd_; }
    bool needsWrite() const { return !output_.empty(); }
    long deadline() const;
    bool ready() const { return ready_; }
    bool pop(Event* event);
    unsigned int request(unsigned char command, const Bytes& payload);
    const ZigguratState& state() const { return state_; }
private:
    struct Pending { unsigned char command; long expires; bool admitted; Pending() : command(0), expires(0), admitted(false) {} };
    int fd_;
    int lockFd_;
    bool connecting_;
    bool hello_;
    bool ready_;
    bool failed_;
    bool discarding_;
    bool stopping_;
    bool dirtyTables_;
    unsigned int nextId_;
    unsigned int controlId_;
    unsigned char phase_;
    long reconnectAt_;
    long helloDeadline_;
    long checkpointAt_;
    std::string socketPath_;
    std::string statePath_;
    Bytes input_;
    Bytes output_;
    std::map<unsigned int, Pending> pending_;
    std::deque<Event> events_;
    ZigguratState state_;
    ZigguratState snapshot_;
    unsigned long keyRevision_;
    unsigned long snapshotRevision_;
    bool connectSocket(std::string* error);
    void disconnect(const std::string& reason, bool permanent = false);
    void frame(const Bytes& data);
    void control(unsigned char command, const Bytes& payload, unsigned char phase);
    void controlReply(unsigned char command, unsigned char status, const Bytes& body);
    bool persist();
    void snapshot(bool initial);
    void finishSnapshot();
};
#endif
