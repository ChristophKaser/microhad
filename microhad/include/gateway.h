#ifndef MICROHAD_ZIGBEE_GATEWAY_H
#define MICROHAD_ZIGBEE_GATEWAY_H

#include <map>
#include <string>
#include <vector>

#include "ash.h"

struct Device {
    std::string ieeeAddress;
    unsigned short networkAddress;
    std::string name;
    std::string kind;
    bool contactOpen;
    bool hasContact;
    bool hasTemperature;
    double temperatureCelsius;
    bool hasHumidity;
    double humidityPercent;
    bool hasOccupancy;
    bool occupied;
    bool hasBattery;
    int batteryPercent;
    int linkQuality;
    long lastSeen;
    int iasZoneId;
    bool hasLowTemperatureAlert;
    bool hasHighTemperatureAlert;
    double lowTemperatureAlert;
    double highTemperatureAlert;
    Device();
};

class Gateway {
public:
    Gateway(const std::string& serialDevice, int serialBaud, const std::string& runtimeDirectory,
            const std::string& firebaseServiceAccount, const std::string& firebaseTopic, const std::string& caBundle);
    ~Gateway();
    bool start(std::string* error);
    void stop();
    void poll();
    bool isOnline() const;
    std::string runtimeDirectory() const;
    bool setJoining(unsigned int seconds, std::string* error);
    bool createFreshNetwork(std::string* error);
    bool renameDevice(const std::string& ieeeAddress, const std::string& name, std::string* error);
    bool applyAttributeReport(const std::string& ieeeAddress, unsigned short cluster, unsigned short attribute,
                              const std::vector<unsigned char>& value);
    bool applyIasZoneStatus(const std::string& ieeeAddress, const std::vector<unsigned char>& payload);
    bool setTemperatureAlerts(const std::string& ieeeAddress, bool hasLow, double low, bool hasHigh, double high,
                              std::string* error);
    void sendTestNotification();
    std::vector<Device> devices() const;
    std::string statusJson() const;

private:
    std::string serialDevice_;
    int serialBaud_;
    std::string runtimeDirectory_;
    std::string firebaseServiceAccount_;
    std::string firebaseTopic_;
    std::string caBundle_;
    int serialFd_;
    bool online_;
    long joinUntil_;
    long joinPermitRenewAt_;
    unsigned int pendingJoinSeconds_;
    unsigned char joinPreparationStep_;
    AshTransport ash_;
    unsigned char ashSendSequence_;
    unsigned char ashReceiveSequence_;
    unsigned char ezspSequence_;
    unsigned char ezspVersion_;
    bool ezspVersionNegotiated_;
    long resetDeadline_;
    bool networkForming_;
    bool networkFormed_;
    std::vector<unsigned char> networkExtendedPan_;
    std::vector<unsigned char> networkKey_;
    std::vector<unsigned char> preconfiguredKey_;
    unsigned short networkPanId_;
    unsigned char networkChannel_;
    std::map<std::string, Device> devices_;
    std::map<unsigned short, std::string> nodeAddresses_;
    std::map<std::string, long> contactOpenedAt_;
    std::map<std::string, long> nextContactAlertAt_;
    bool configureSerial(std::string* error);
    bool writeBytes(const std::vector<unsigned char>& bytes);
    void sendAshReset();
    void sendEzspCommand(unsigned short command, const std::vector<unsigned char>& payload);
    void sendApsUnicast(unsigned short destination, unsigned short profile, unsigned short cluster,
                        unsigned char sourceEndpoint, unsigned char destinationEndpoint,
                        const std::vector<unsigned char>& message);
    void sendJoinPermit(unsigned int duration);
    void beginJoinWindow(unsigned int seconds);
    void handleAshFrame(const std::vector<unsigned char>& frame);
    void handleEzspFrame(const std::vector<unsigned char>& frame);
    void registerNode(unsigned short nodeId, const unsigned char* eui64);
    int allocateIasZoneId(const std::string& ieeeAddress);
    void sendIasEnrollResponse(unsigned short destination, unsigned char sourceEndpoint,
                               unsigned char destinationEndpoint, unsigned char transactionSequence,
                               unsigned char zoneId);
    void handleIncomingMessage(const std::vector<unsigned char>& payload);
    bool generateNetworkMaterial(std::string* error);
    void continueNetworkFormation(unsigned short completedCommand);
    void updateContactAlert(const std::string& ieeeAddress, bool wasOpen);
    void updateTemperatureAlert(const Device& device, bool hadTemperature, double previousTemperature);
    void emitNotification(const std::string& title, const std::string& body);
    void loadState();
    void saveState() const;
};

#endif
