#ifndef MICROHAD_TEST_CACHE_FIXTURE_H
#define MICROHAD_TEST_CACHE_FIXTURE_H
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>
// Each gateway test owns both global RAM-cache paths, including shutdown writes.
class DeviceCacheIsolation {
    std::string backup_[2];bool saved_[2];
    const char* path(unsigned i) const { return i ? "/tmp/microhad-values.pb" : "/tmp/microhad-values.json"; }
public:
    DeviceCacheIsolation() {
        for(unsigned i=0;i<2;++i) {
            char name[]="/tmp/microhad-cache-backup-XXXXXX";int fd=mkstemp(name);assert(fd>=0);close(fd);unlink(name);backup_[i]=name;
            saved_[i]=rename(path(i),name)==0;assert(saved_[i]||errno==ENOENT);
        }
    }
    ~DeviceCacheIsolation() {
        for(unsigned i=0;i<2;++i){unlink(path(i));if(saved_[i])assert(rename(backup_[i].c_str(),path(i))==0);}
    }
};
#endif
