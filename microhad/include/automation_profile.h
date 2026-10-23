#ifndef MICROHAD_AUTOMATION_PROFILE_H
#define MICROHAD_AUTOMATION_PROFILE_H

#include <algorithm>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

// Opt-in, RAM-only diagnostics. Each call site belongs to one worker thread.
namespace AutomationProfile {
inline bool enabled() { return access("/tmp/microhad-automation-profile", F_OK) == 0; }
inline double clock(clockid_t id) {
    timespec value;
    return clock_gettime(id, &value) == 0 ? value.tv_sec + value.tv_nsec / 1e9 : -1;
}
struct Stats {
    unsigned long count;
    double wall, cpu, maximum, reported;
    Stats() : count(0), wall(0), cpu(0), maximum(0), reported(0) {}
};
class Scope {
    Stats& stats_; const char* name_; bool active_; double wall_, cpu_;
public:
    Scope(Stats& stats, const char* name) : stats_(stats), name_(name), active_(enabled()), wall_(0), cpu_(0) {
        if (active_) { wall_=clock(CLOCK_MONOTONIC);cpu_=clock(CLOCK_THREAD_CPUTIME_ID); }
    }
    ~Scope() {
        if (!active_) return;
        const double now=clock(CLOCK_MONOTONIC), elapsed=now-wall_;
        const double cpu=clock(CLOCK_THREAD_CPUTIME_ID);
        ++stats_.count;stats_.wall+=elapsed;
        if (cpu_>=0 && cpu>=0) stats_.cpu+=cpu-cpu_;
        stats_.maximum=std::max(stats_.maximum,elapsed);
        if (stats_.reported==0) stats_.reported=now;
        if (now-stats_.reported>=3) {
            syslog(LOG_INFO,"AUTOPROF %s n=%lu wall_ms=%.3f cpu_ms=%.3f max_ms=%.3f",name_,stats_.count,stats_.wall*1000,stats_.cpu*1000,stats_.maximum*1000);
            stats_.count=0;stats_.wall=stats_.cpu=stats_.maximum=0;stats_.reported=now;
        }
    }
private:
    Scope(const Scope&);Scope& operator=(const Scope&);
};
inline void mark(const char* name, const char* detail) {
    if (enabled()) syslog(LOG_INFO,"AUTOPROF %s t=%.6f cpu=%.6f %.100s",name,clock(CLOCK_MONOTONIC),clock(CLOCK_THREAD_CPUTIME_ID),detail);
}
}
#define AUTOMATION_PROFILE(name) static AutomationProfile::Stats profileStats; AutomationProfile::Scope profileScope(profileStats,name)
#endif
