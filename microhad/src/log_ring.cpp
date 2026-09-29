#include "log_ring.h"
#include "json_util.h"

#include <cerrno>
#include <climits>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace {
enum { headerBytes = 32, headerCopies = 2, dataStart = headerBytes * headerCopies,
       capacity = LogRing::MaximumBytes - dataStart, recordHeaderBytes = 16,
       maximumLineBytes = 1024 };
const unsigned char fileMagic[4] = { 'T', 'Z', 'L', '1' };
const unsigned char recordMagic[4] = { 'L', 'O', 'G', '1' };
const unsigned char wrapMagic[4] = { 'W', 'R', 'A', 'P' };

struct DiskState {
    uint32_t generation, head, tail, used, lastSequence;
    DiskState() : generation(0), head(0), tail(0), used(0), lastSequence(0) {}
};

void put32(unsigned char* bytes, uint32_t value) {
    bytes[0] = static_cast<unsigned char>(value >> 24);
    bytes[1] = static_cast<unsigned char>(value >> 16);
    bytes[2] = static_cast<unsigned char>(value >> 8);
    bytes[3] = static_cast<unsigned char>(value);
}
uint32_t get32(const unsigned char* bytes) {
    return (static_cast<uint32_t>(bytes[0]) << 24) | (static_cast<uint32_t>(bytes[1]) << 16) |
           (static_cast<uint32_t>(bytes[2]) << 8) | bytes[3];
}
uint32_t hashBytes(uint32_t hash, const unsigned char* bytes, unsigned long length) {
    for (unsigned long i = 0; i < length; ++i) hash = (hash ^ bytes[i]) * 16777619UL;
    return hash;
}
bool readExact(int fd, unsigned char* bytes, unsigned long length, off_t offset) {
    while (length != 0) {
        ssize_t count = pread(fd, bytes, length, offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        bytes += count; length -= static_cast<unsigned long>(count); offset += count;
    }
    return true;
}
bool writeExact(int fd, const unsigned char* bytes, unsigned long length, off_t offset) {
    while (length != 0) {
        ssize_t count = pwrite(fd, bytes, length, offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        bytes += count; length -= static_cast<unsigned long>(count); offset += count;
    }
    return true;
}
void encodeHeader(const DiskState& state, unsigned char* bytes) {
    memcpy(bytes, fileMagic, 4);
    put32(bytes + 4, state.generation); put32(bytes + 8, state.head);
    put32(bytes + 12, state.tail); put32(bytes + 16, state.used);
    put32(bytes + 20, state.lastSequence); put32(bytes + 24, 0);
    put32(bytes + 28, hashBytes(2166136261UL, bytes, 28));
}
bool decodeHeader(const unsigned char* bytes, DiskState* state) {
    if (memcmp(bytes, fileMagic, 4) != 0 || get32(bytes + 24) != 0 ||
        get32(bytes + 28) != hashBytes(2166136261UL, bytes, 28)) return false;
    state->generation = get32(bytes + 4); state->head = get32(bytes + 8);
    state->tail = get32(bytes + 12); state->used = get32(bytes + 16);
    state->lastSequence = get32(bytes + 20);
    return state->generation != 0 && state->head < capacity && state->tail < capacity &&
           state->used <= capacity && (state->used != 0 || state->head == state->tail);
}
bool bestHeader(int fd, DiskState* state) {
    unsigned char bytes[headerBytes];
    DiskState candidate;
    bool found = false;
    for (unsigned int slot = 0; slot < headerCopies; ++slot) {
        if (readExact(fd, bytes, headerBytes, slot * headerBytes) && decodeHeader(bytes, &candidate) &&
            (!found || candidate.generation > state->generation)) {
            *state = candidate;
            found = true;
        }
    }
    return found;
}
bool sameHeader(const DiskState& a, const DiskState& b) {
    return a.generation == b.generation && a.head == b.head && a.tail == b.tail &&
           a.used == b.used && a.lastSequence == b.lastSequence;
}
bool parseSnapshot(const DiskState& state, const std::vector<unsigned char>& bytes,
                   std::vector<std::string>* lines) {
    unsigned long consumed = 0;
    uint32_t previous = 0;
    lines->clear();
    while (consumed < state.used) {
        unsigned long physical = (state.tail + consumed) % capacity;
        unsigned long atEnd = capacity - physical;
        if (atEnd < recordHeaderBytes ||
            (state.used - consumed >= 4 && memcmp(&bytes[consumed], wrapMagic, 4) == 0)) {
            if (atEnd > state.used - consumed) return false;
            consumed += atEnd;
            continue;
        }
        if (state.used - consumed < recordHeaderBytes ||
            memcmp(&bytes[consumed], recordMagic, 4) != 0) return false;
        const unsigned char* record = &bytes[consumed];
        uint32_t sequence = get32(record + 4), length = get32(record + 8);
        if (length > maximumLineBytes || recordHeaderBytes + length > atEnd ||
            recordHeaderBytes + length > state.used - consumed ||
            (previous != 0 && sequence != previous + 1) || sequence == 0) return false;
        uint32_t hash = hashBytes(2166136261UL, record, 12);
        hash = hashBytes(hash, record + recordHeaderBytes, length);
        if (hash != get32(record + 12)) return false;
        lines->push_back(std::string(reinterpret_cast<const char*>(record + recordHeaderBytes), length));
        previous = sequence;
        consumed += recordHeaderBytes + length;
    }
    return (state.used == 0 || previous == state.lastSequence) &&
           (state.tail + state.used) % capacity == state.head;
}
bool readSnapshot(int fd, DiskState* state, std::vector<std::string>* lines) {
    for (int attempt = 0; attempt < 8; ++attempt) {
        DiskState before, after;
        if (!bestHeader(fd, &before)) return false;
        std::vector<unsigned char> bytes(before.used);
        unsigned long first = before.used < capacity - before.tail ? before.used : capacity - before.tail;
        bool ok = first == 0 || readExact(fd, &bytes[0], first, dataStart + before.tail);
        if (ok && before.used > first)
            ok = readExact(fd, &bytes[first], before.used - first, dataStart);
        if (!ok || !bestHeader(fd, &after)) return false;
        if (!sameHeader(before, after)) continue;
        if (!parseSnapshot(before, bytes, lines)) return false;
        *state = before;
        return true;
    }
    return false;
}
bool lineTimestamp(const unsigned char* line, unsigned long length, unsigned long* timestamp) {
    unsigned long value = 0;
    unsigned long i = 0;
    if (length == 0 || line[0] < '0' || line[0] > '9') return false;
    while (i < length && line[i] >= '0' && line[i] <= '9') {
        unsigned int digit = static_cast<unsigned int>(line[i] - '0');
        if (value > (ULONG_MAX - digit) / 10) return false;
        value = value * 10 + digit;
        ++i;
    }
    if (i == length || (line[i] != ' ' && line[i] != '\t')) return false;
    *timestamp = value;
    return true;
}
bool readSnapshotSince(int fd, unsigned long since, std::string* json) {
    for (int attempt = 0; attempt < 8; ++attempt) {
        DiskState before, after;
        if (!bestHeader(fd, &before)) return false;
        std::vector<unsigned char> bytes(before.used);
        unsigned long first = before.used < capacity - before.tail ? before.used : capacity - before.tail;
        bool ok = first == 0 || readExact(fd, &bytes[0], first, dataStart + before.tail);
        if (ok && before.used > first)
            ok = readExact(fd, &bytes[first], before.used - first, dataStart);
        if (!ok || !bestHeader(fd, &after)) return false;
        if (!sameHeader(before, after)) continue;

        std::ostringstream output;
        output << "{\"lines\":[";
        bool firstLine = true;
        unsigned long consumed = 0;
        uint32_t previous = 0;
        while (consumed < before.used) {
            unsigned long physical = (before.tail + consumed) % capacity;
            unsigned long atEnd = capacity - physical;
            if (atEnd < recordHeaderBytes ||
                (before.used - consumed >= 4 && memcmp(&bytes[consumed], wrapMagic, 4) == 0)) {
                if (atEnd > before.used - consumed) return false;
                consumed += atEnd;
                continue;
            }
            if (before.used - consumed < recordHeaderBytes ||
                memcmp(&bytes[consumed], recordMagic, 4) != 0) return false;
            const unsigned char* record = &bytes[consumed];
            uint32_t sequence = get32(record + 4), length = get32(record + 8);
            if (length > maximumLineBytes || recordHeaderBytes + length > atEnd ||
                recordHeaderBytes + length > before.used - consumed ||
                (previous != 0 && sequence != previous + 1) || sequence == 0) return false;
            const unsigned char* line = record + recordHeaderBytes;
            unsigned long timestamp = 0;
            if (lineTimestamp(line, length, &timestamp) && timestamp >= since) {
                uint32_t hash = hashBytes(2166136261UL, record, 12);
                if (hashBytes(hash, line, length) != get32(record + 12)) return false;
                if (!firstLine) output << ',';
                output << '\"' << jsonEscape(std::string(reinterpret_cast<const char*>(line), length)) << '\"';
                firstLine = false;
            }
            previous = sequence;
            consumed += recordHeaderBytes + length;
        }
        if ((before.used != 0 && previous != before.lastSequence) ||
            (before.tail + before.used) % capacity != before.head) return false;
        output << "]}";
        *json = output.str();
        return true;
    }
    return false;
}
}

const char* LogRing::defaultPath() { return "/tmp/microhad-zigbee-native.log"; }
LogRing::LogRing(const std::string& path) : path_(path), fd_(-1) {}
LogRing::~LogRing() { if (fd_ >= 0) close(fd_); }

bool LogRing::createEmpty(int fd) {
    if (ftruncate(fd, MaximumBytes) != 0) return false;
    unsigned char empty[dataStart];
    memset(empty, 0, sizeof(empty));
    if (!writeExact(fd, empty, sizeof(empty), 0)) return false;
    header_ = Header();
    header_.generation = 1;
    DiskState state;
    state.generation = 1;
    unsigned char bytes[headerBytes];
    encodeHeader(state, bytes);
    return writeExact(fd, bytes, headerBytes, headerBytes);
}

bool LogRing::initialize() {
    if (fd_ >= 0) return true;
    int fd = open(path_.c_str(), O_RDWR | O_CREAT, 0600);
    if (fd < 0) return false;
    struct stat info;
    if (fstat(fd, &info) != 0) { close(fd); return false; }
    if (info.st_size == 0) {
        if (!createEmpty(fd)) { close(fd); return false; }
        fd_ = fd;
        return true;
    }
    DiskState state;
    std::vector<std::string> lines;
    if (info.st_size == MaximumBytes && readSnapshot(fd, &state, &lines)) {
        header_.generation = state.generation; header_.head = state.head;
        header_.tail = state.tail; header_.used = state.used;
        header_.lastSequence = state.lastSequence;
        fd_ = fd;
        return true;
    }
    unsigned char prefix[128];
    ssize_t prefixSize = pread(fd, prefix, sizeof(prefix), 0);
    bool legacy = prefixSize > 0;
    for (ssize_t i = 0; i < prefixSize; ++i)
        if (prefix[i] == 0) legacy = false;
    close(fd);
    if (!legacy) {
        fd = open(path_.c_str(), O_RDWR | O_TRUNC, 0600);
        if (fd < 0) return false;
        if (!createEmpty(fd)) { close(fd); return false; }
        fd_ = fd;
        return true;
    }
    std::deque<std::string> old;
    unsigned long bytes = 0;
    std::ifstream input(path_.c_str());
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty()) continue;
        if (line.size() > maximumLineBytes) line.resize(maximumLineBytes);
        old.push_back(line); bytes += static_cast<unsigned long>(line.size() + recordHeaderBytes);
        while (bytes > capacity && !old.empty()) {
            bytes -= static_cast<unsigned long>(old.front().size() + recordHeaderBytes);
            old.pop_front();
        }
    }
    const std::string temporary = path_ + ".migrate";
    fd = open(temporary.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return false;
    if (!createEmpty(fd)) { close(fd); unlink(temporary.c_str()); return false; }
    fd_ = fd;
    for (std::deque<std::string>::const_iterator it = old.begin(); it != old.end(); ++it)
        if (!appendOpened(*it)) { close(fd_); fd_ = -1; unlink(temporary.c_str()); return false; }
    if (rename(temporary.c_str(), path_.c_str()) != 0) {
        close(fd_); fd_ = -1; unlink(temporary.c_str()); return false;
    }
    return true;
}

bool LogRing::commit() {
    if (header_.generation == 0xffffffffUL) return false;
    DiskState state;
    state.generation = header_.generation + 1;
    state.head = header_.head; state.tail = header_.tail;
    state.used = header_.used; state.lastSequence = header_.lastSequence;
    unsigned char bytes[headerBytes];
    encodeHeader(state, bytes);
    if (!writeExact(fd_, bytes, headerBytes, (state.generation % 2) * headerBytes)) return false;
    header_.generation = state.generation;
    return true;
}

bool LogRing::advanceTail() {
    if (header_.used == 0) return false;
    unsigned long atEnd = capacity - header_.tail;
    if (atEnd < recordHeaderBytes) {
        if (atEnd > header_.used) return false;
        header_.tail = 0; header_.used -= atEnd;
    } else {
        unsigned char bytes[recordHeaderBytes];
        if (!readExact(fd_, bytes, recordHeaderBytes, dataStart + header_.tail)) return false;
        if (memcmp(bytes, wrapMagic, 4) == 0) {
            if (atEnd > header_.used) return false;
            header_.tail = 0; header_.used -= atEnd;
        } else {
            unsigned long length = get32(bytes + 8);
            if (memcmp(bytes, recordMagic, 4) != 0 || length > maximumLineBytes ||
                recordHeaderBytes + length > header_.used) return false;
            header_.tail = (header_.tail + recordHeaderBytes + length) % capacity;
            header_.used -= recordHeaderBytes + length;
        }
    }
    if (header_.used == 0) header_.tail = header_.head;
    return true;
}

bool LogRing::appendOpened(const std::string& text) {
    const std::string line = text.substr(0, maximumLineBytes);
    const unsigned long recordSize = recordHeaderBytes + line.size();
    const unsigned long padding = header_.head + recordSize > capacity ? capacity - header_.head : 0;
    bool evicted = false;
    while (header_.used + padding + recordSize > capacity) {
        if (!advanceTail()) return false;
        evicted = true;
    }
    /* Make eviction durable before writing over any old record. */
    if (evicted && !commit()) return false;
    if (header_.lastSequence == 0xffffffffUL) return false;
    if (padding != 0) {
        if (padding >= 4 && !writeExact(fd_, wrapMagic, 4, dataStart + header_.head)) return false;
        header_.head = 0;
        header_.used += padding;
        if (header_.used == padding) header_.tail = 0;
    }
    unsigned char record[recordHeaderBytes + maximumLineBytes];
    memcpy(record, recordMagic, 4);
    put32(record + 4, header_.lastSequence + 1);
    put32(record + 8, static_cast<uint32_t>(line.size()));
    if (!line.empty()) memcpy(record + recordHeaderBytes, line.data(), line.size());
    uint32_t hash = hashBytes(2166136261UL, record, 12);
    put32(record + 12, hashBytes(hash, record + recordHeaderBytes, line.size()));
    if (!writeExact(fd_, record, recordSize, dataStart + header_.head)) return false;
    if (header_.used == 0) header_.tail = header_.head;
    header_.head = (header_.head + recordSize) % capacity;
    header_.used += recordSize;
    ++header_.lastSequence;
    return commit();
}

bool LogRing::append(const std::string& line) {
    if (!initialize()) return false;
    if (appendOpened(line)) return true;
    close(fd_); fd_ = -1;
    return false;
}

bool LogRing::readLines(const std::string& path, std::vector<std::string>* lines) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;
    struct stat info;
    DiskState state;
    bool ok = fstat(fd, &info) == 0 && info.st_size == MaximumBytes && readSnapshot(fd, &state, lines);
    close(fd);
    return ok;
}

bool LogRing::readJson(const std::string& path, std::string* json) {
    std::vector<std::string> lines;
    if (!readLines(path, &lines)) return false;
    std::ostringstream output;
    output << "{\"lines\":[";
    for (unsigned long i = 0; i < lines.size(); ++i) {
        if (i != 0) output << ',';
        output << '"' << jsonEscape(lines[i]) << '"';
    }
    output << "]}";
    *json = output.str();
    return true;
}

bool LogRing::readJson(const std::string& path, unsigned long since, std::string* json) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;
    struct stat info;
    bool ok = fstat(fd, &info) == 0 && info.st_size == MaximumBytes && readSnapshotSince(fd, since, json);
    close(fd);
    return ok;
}
