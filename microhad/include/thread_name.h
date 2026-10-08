#ifndef MICROHAD_ZIGBEE_THREAD_NAME_H
#define MICROHAD_ZIGBEE_THREAD_NAME_H

#include <sys/prctl.h>

inline void setCurrentThreadName(const char* name) {
    prctl(PR_SET_NAME, name, 0, 0, 0);
}

#endif
