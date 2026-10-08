#ifndef MICROHAD_SYSLOG_LOG_READER_H
#define MICROHAD_SYSLOG_LOG_READER_H

#include <string>

class SyslogLogReader {
public:
    static bool readJson(std::string* json);
    static bool readJson(unsigned long since, std::string* json);
    static bool readPage(unsigned long limit, unsigned long before, std::string* json);
};

#endif
