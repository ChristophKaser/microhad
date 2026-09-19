#ifndef MICROHAD_ZIGBEE_GATEWAY_H
#define MICROHAD_ZIGBEE_GATEWAY_H

#include <map>
#include <string>
#include <vector>

#include "ash.h"

struct Device {
    std::string ieeeAddress;
    std::string name;
    std::string kind;
    bool contactOpen;
    bool hasContact;
    bool hasTemperature;
    double temperatureCelsius;
    bool hasBattery;
    int batteryPercent;
    int linkQuality;
    long lastSeen;
    Device();
};

class Gateway {
public:
    Gateway(const std::string& serialDevice, int serialBaud, const std::string& runtimeDirectory);
    ~Gateway();
    bool start(std::string* error);
    void stop();
    void poll();
    bool isOnline() const;
    bool setJoining(unsigned int seconds, std::string* error);
    bool renameDevice(const std::string& ieeeAddress, const std::string& name, std::string* error);
    bool applyAttributeReport(const std::string& ieeeAddress, unsigned short cluster, unsigned short attribute,
                              const std::vector<unsigned char>& value);
    bool applyIasZoneStatus(const std::string& ieeeAddress, const std::vector<unsigned char>& payload);
    std::vector<Device> devices() const;
    std::string statusJson() const;

private:
    std::string serialDevice_;
    int serialBaud_;
    std::string runtimeDirectory_;
    int serialFd_;
    bool online_;
    long joinUntil_;
    AshTransport ash_;
    unsigned char ashSendSequence_;
    unsigned char ashReceiveSequence_;
    unsigned char ezspSequence_;
    unsigned char ezspVersion_;
    long resetDeadline_;
    std::map<std::string, Device> devices_;
    std::map<unsigned short, std::string> nodeAddresses_;
    bool configureSerial(std::string* error);
    bool writeBytes(const std::vector<unsigned char>& bytes);
    void sendAshReset();
    void sendEzspCommand(unsigned short command, const std::vector<unsigned char>& payload);
    void handleAshFrame(const std::vector<unsigned char>& frame);
    void handleEzspFrame(const std::vector<unsigned char>& frame);
    void registerNode(unsigned short nodeId, const unsigned char* eui64);
    void handleIncomingMessage(const std::vector<unsigned char>& payload);
    void loadState();
    void saveState() const;
};

#endif
