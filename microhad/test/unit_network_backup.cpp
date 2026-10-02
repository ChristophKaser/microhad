#include "network_backup.h"
#include <cassert>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdlib>
#include <set>

void runNetworkBackupUnitTests() {
    char parent[] = "/tmp/microhad-backup-unit-XXXXXX";
    assert(mkdtemp(parent) != 0);
    const std::string directory = std::string(parent) + "/private";
    const std::string id = "1234567890123456";
    const std::string output = directory + "/" + id + ".json";
    std::string error;
    NetworkBackupExport exporter;
    assert(!exporter.start("../bad", directory, &error));
    assert(exporter.start(id, directory, &error));
    assert(!exporter.start(id, directory, &error));
    struct stat state;
    assert(stat((output + ".partial").c_str(), &state) == 0 && (state.st_mode & 0777) == 0600);
    NetworkBackupExport::Query query(0, std::vector<unsigned char>());
    std::set<unsigned short> allowed;
    const unsigned short commands[] = {0x26,0x27,0x28,0x69,0x116,0x91,0x114,0x52,0x100,0x101,0x102,0x10f,0x4a,0x10c};
    for (unsigned int i = 0; i < sizeof(commands)/sizeof(commands[0]); ++i) allowed.insert(commands[i]);
    unsigned int count = 0, keys = 0, children = 0, tokens = 0;
    while (exporter.next(&query)) {
        assert(allowed.count(query.command) == 1); ++count;
        std::vector<unsigned char> response(1, 0);
        if (query.command == 0x52) { response.resize(3); response[1] = query.payload[0] == 0x0d ? 5 : 2; }
        if (query.command == 0x100) response[0] = 1;
        if (query.command == 0x101) {
            response.resize(9); response[1] = 0x23; response[2] = 0xe1; response[3] = 1;
            response[5] = 1; response[7] = 4; response[8] = 1;
        }
        if (query.command == 0x10f) ++keys;
        if (query.command == 0x4a) ++children;
        if (query.command == 0x102) {
            ++tokens; assert(query.payload.size() == 8 && query.payload[0] == 0x23 && query.payload[3] == 0);
        }
        assert(exporter.accept(query, response, &error));
        assert(count < 100);
    }
    assert(keys == 2 && children == 2 && tokens == 1 && count == 26);
    assert(exporter.finish(&error));
    assert(!exporter.active());
    assert(stat(output.c_str(), &state) == 0 && (state.st_mode & 0777) == 0600);
    assert(access((output + ".partial").c_str(), F_OK) != 0);
    std::ifstream input(output.c_str()); std::ostringstream content; content << input.rdbuf();
    assert(content.str().find("\"complete\":true") != std::string::npos);
    assert(!exporter.start(id, directory, &error)); // no overwrite
    assert(exporter.start("1234567890123457", directory, &error));
    NetworkBackupExport::Query bad(0x52, std::vector<unsigned char>(1, 0x1e));
    assert(!exporter.accept(bad, std::vector<unsigned char>(1, 0), &error));
    exporter.abort();
    assert(access((directory + "/1234567890123457.json.partial").c_str(), F_OK) != 0);
    unlink(output.c_str()); rmdir(directory.c_str()); rmdir(parent);
}
