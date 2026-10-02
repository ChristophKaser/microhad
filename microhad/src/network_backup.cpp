#include "network_backup.h"
#include <cerrno>
#include <fcntl.h>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace {
std::string hex(const std::vector<unsigned char>& data) {
    const char digits[] = "0123456789abcdef";
    std::string result;
    for (unsigned long i = 0; i < data.size(); ++i) {
        result += digits[data[i] >> 4]; result += digits[data[i] & 15];
    }
    return result;
}
}
NetworkBackupExport::NetworkBackupExport() : fd_(-1), bytes_(0), first_(true), closing_(false) {}
NetworkBackupExport::~NetworkBackupExport() { abort(); }
void NetworkBackupExport::enqueue(unsigned short command, const std::vector<unsigned char>& payload) {
    queries_.push_back(Query(command, payload));
}
bool NetworkBackupExport::append(const std::string& data) {
    if (bytes_ + data.size() > 262144) return false;
    unsigned long offset = 0;
    while (offset < data.size()) {
        ssize_t count = write(fd_, data.data() + offset, data.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        offset += static_cast<unsigned long>(count);
    }
    bytes_ += data.size(); return true;
}
bool NetworkBackupExport::start(const std::string& id, const std::string& directory, std::string* error) {
    if (active()) { *error = "network backup already active"; return false; }
    if (id.size() < 12 || id.size() > 24 || id.find_first_not_of("0123456789") != std::string::npos) {
        *error = "backup id must contain 12 to 24 digits"; return false;
    }
    struct stat state;
    if (mkdir(directory.c_str(), 0700) != 0 && errno != EEXIST) {
        *error = "cannot create private backup directory"; return false;
    }
    if (lstat(directory.c_str(), &state) != 0 || !S_ISDIR(state.st_mode) || state.st_uid != getuid() ||
        (state.st_mode & 0777) != 0700) {
        *error = "backup directory must be owned by service user with mode 0700"; return false;
    }
    final_ = directory + "/" + id + ".json"; partial_ = final_ + ".partial";
    if (lstat(final_.c_str(), &state) == 0 || errno != ENOENT) {
        *error = "backup output already exists or cannot be checked"; return false;
    }
    fd_ = open(partial_.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd_ < 0) { *error = "cannot create exclusive backup file"; return false; }
    bytes_ = 0; first_ = true; closing_ = false; queries_.clear();
    const std::vector<unsigned char> empty;
    enqueue(0x0026, empty); enqueue(0x0027, empty); enqueue(0x0028, empty);
    enqueue(0x0069, empty); enqueue(0x0116, empty); enqueue(0x0091, empty);
    for (unsigned char key = 1; key <= 2; ++key) {
        std::vector<unsigned char> context(18, 0); context[0] = key;
        enqueue(0x0114, context);
    }
    enqueue(0x0052, std::vector<unsigned char>(1, 0x1e)); // key table size
    enqueue(0x0052, std::vector<unsigned char>(1, 0x11)); // children
    enqueue(0x0052, std::vector<unsigned char>(1, 0x0d)); // security level
    std::vector<unsigned char> tcContext(18, 0); tcContext[0] = 2;
    enqueue(0x010c, tcContext); // APS metadata, if the installed stack supports TC context
    enqueue(0x0100, empty); // token count; enumerate all persisted stack state
    if (!append("{\"format\":\"microhad-ezsp-export\",\"version\":1,\"ezsp_version\":13,\"records\":[\n")) {
        *error = "cannot write backup header"; abort(); return false;
    }
    return true;
}
bool NetworkBackupExport::next(Query* query) {
    if (queries_.empty() && !closing_) {
        closing_ = true;
        const std::vector<unsigned char> empty;
        enqueue(0x0028, empty); enqueue(0x0069, empty); enqueue(0x0116, empty);
        enqueue(0x0026, empty);
        for (unsigned char key = 1; key <= 2; ++key) {
            std::vector<unsigned char> context(18, 0); context[0] = key;
            enqueue(0x0114, context);
        }
        std::vector<unsigned char> tcContext(18, 0); tcContext[0] = 2;
        enqueue(0x010c, tcContext);
    }
    if (queries_.empty()) return false;
    *query = queries_.front(); queries_.pop_front(); return true;
}
bool NetworkBackupExport::accept(const Query& query, const std::vector<unsigned char>& response,
                                 std::string* error) {
    std::ostringstream record;
    record << (first_ ? "" : ",\n") << "{\"command\":" << query.command << ",\"request\":\""
           << hex(query.payload) << "\",\"response\":\"" << hex(response) << "\"}";
    first_ = false;
    if (!append(record.str())) { *error = "backup size limit or file write failure"; return false; }
    if (query.command == 0x0052 && query.payload[0] != 0x0d) {
        if (response.size() != 3 || response[0] != 0 || response[2] != 0) {
            *error = "invalid table-size response"; return false;
        }
        for (unsigned int i = 0; i < response[1]; ++i)
            enqueue(query.payload[0] == 0x1e ? 0x010f : 0x004a, std::vector<unsigned char>(1, i));
    } else if (query.command == 0x0100) {
        if (response.size() != 1) { *error = "invalid token-count response"; return false; }
        for (unsigned int i = 0; i < response[0]; ++i) enqueue(0x0101, std::vector<unsigned char>(1, i));
    } else if (query.command == 0x0101) {
        if (response.size() != 9 || response[0] != 0) {
            *error = "invalid token-info response"; return false;
        }
        for (unsigned int i = 0; i < response[8]; ++i) {
            std::vector<unsigned char> payload(response.begin() + 1, response.begin() + 5);
            payload.push_back(i); payload.insert(payload.end(), 3, 0);
            enqueue(0x0102, payload);
        }
    }
    if (queries_.size() > 2048) { *error = "backup query limit exceeded"; return false; }
    return true;
}
bool NetworkBackupExport::finish(std::string* error) {
    if (!append("\n],\"complete\":true}\n")) { *error = "cannot finish backup"; abort(); return false; }
    int result = close(fd_); fd_ = -1;
    // link publishes without ever replacing an existing final file.
    if (result != 0 || link(partial_.c_str(), final_.c_str()) != 0) {
        *error = "cannot publish backup"; unlink(partial_.c_str()); return false;
    }
    unlink(partial_.c_str()); partial_.clear(); queries_.clear(); return true;
}
void NetworkBackupExport::abort() {
    if (fd_ >= 0) close(fd_);
    fd_ = -1;
    if (!partial_.empty()) unlink(partial_.c_str());
    partial_.clear(); queries_.clear();
}
