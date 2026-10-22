#ifndef MICROHAD_SYSLOG_LOG_READER_H
#define MICROHAD_SYSLOG_LOG_READER_H

#include <string>
#ifdef MICROHAD_TESTING
#include <vector>
#endif

#ifdef MICROHAD_TESTING
struct SyslogReadStats {
    unsigned long indexedLines, materializedLines, timestampConversions, zeroBlocksSkipped;
    unsigned long fastTimestampParses, calendarConversions;
    SyslogReadStats() : indexedLines(0), materializedLines(0), timestampConversions(0), zeroBlocksSkipped(0),
        fastTimestampParses(0), calendarConversions(0) {}
};
#endif

class SyslogLogReader {
public:
    static bool readJson(std::string* json);
#ifdef MICROHAD_TESTING
    static bool readLinesForTest(long key, std::vector<std::string>* lines);
    static bool readRequestForTest(long key, bool hasSince, unsigned long since,
                                   unsigned long limit, unsigned long before, long now,
                                   std::string* json, SyslogReadStats* stats);
#endif
    static bool readJson(unsigned long since, std::string* json);
    static bool readPage(unsigned long limit, unsigned long before, std::string* json);
};

#endif
