#ifndef MICROHAD_SYSLOG_LOG_READER_H
#define MICROHAD_SYSLOG_LOG_READER_H

#include <string>
#ifdef MICROHAD_TESTING
#include <vector>
#endif

class SyslogLogReader {
public:
    static bool readJson(std::string* json);
#ifdef MICROHAD_TESTING
    static bool readLinesForTest(long key, std::vector<std::string>* lines);
#endif
    static bool readJson(unsigned long since, std::string* json);
    static bool readPage(unsigned long limit, unsigned long before, std::string* json);
};

#endif
