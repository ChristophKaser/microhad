#ifndef MICROHAD_AUTOMATION_WORKER_H
#define MICROHAD_AUTOMATION_WORKER_H

#include <string>

class GatewayRunner;
class NotificationWorker;

/* Owns all Lua states.  No coordinator or HTTP thread is ever allowed to
 * evaluate script code. */
class AutomationWorker {
public:
    AutomationWorker(const std::string& runtimeDirectory, GatewayRunner* gateway,
                     NotificationWorker* notifications);
    ~AutomationWorker();
    bool start(std::string* error);
    void stop();

    std::string listJson() const;
    bool source(const std::string& filename, std::string* value, std::string* error) const;
    bool validate(const std::string& filename, const std::string& value, std::string* error) const;
    bool save(const std::string& filename, const std::string& value, bool reload, std::string* error);
    bool reload(const std::string& filename, std::string* error);
    bool setEnabled(const std::string& filename, bool enabled, std::string* error);
    bool remove(const std::string& filename, std::string* error);
    bool resetPersistent(const std::string& filename, std::string* error);

private:
    struct Impl;
    Impl* impl_;
    AutomationWorker(const AutomationWorker&);
    AutomationWorker& operator=(const AutomationWorker&);
};

#endif
