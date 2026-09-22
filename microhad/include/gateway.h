#ifndef MICROHAD_ZIGBEE_GATEWAY_H
#define MICROHAD_ZIGBEE_GATEWAY_H

#include <map>
#include <string>
#include <vector>

#include "ash.h"

class NotificationWorker;

struct EndpointDescriptor {
    unsigned char endpoint;
    unsigned short profileId;
    unsigned short deviceId;
    unsigned char deviceVersion;
    std::vector<unsigned short> inputClusters;
    std::vector<unsigned short> outputClusters;
    EndpointDescriptor();
};

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
    bool hasLight;
    bool lightOn;
    bool hasBrightness;
    int brightnessPercent;
    bool hasRemote;
    int linkQuality;
    long lastSeen;
    int iasZoneId;
    bool hasLowTemperatureAlert;
    bool hasHighTemperatureAlert;
    double lowTemperatureAlert;
    double highTemperatureAlert;
    unsigned long contactInitialAlertMinutes;
    unsigned long contactReminderAlertMinutes;
    unsigned long sensorRefreshIntervalMinutes;
    std::string manufacturerName;
    std::string modelId;
    std::vector<EndpointDescriptor> endpoints;
    Device();
};

/* Scene data is controller configuration.  The IDs are also the Zigbee Group
   and Scene IDs used on capable lamps. */
struct SceneLightState {
    std::string ieeeAddress;
    bool on;
    int brightnessPercent; /* -1 for a non-dimmable lamp */
    SceneLightState();
};
struct Scene {
    unsigned char id;
    std::string name;
    std::vector<SceneLightState> states;
    Scene();
};
struct SceneGroup {
    unsigned short id;
    std::string name;
    std::vector<std::string> members;
    std::vector<Scene> scenes;
    SceneGroup();
};
struct RemoteSceneBinding {
    std::string ieeeAddress;
    std::string action; /* on, off, arrow_left, arrow_right */
    unsigned short groupId;
    bool off;
    std::vector<unsigned char> sceneIds;
    RemoteSceneBinding();
};

class Gateway {
public:
    Gateway(const std::string& serialDevice, int serialBaud, const std::string& runtimeDirectory,
            const std::string& firebaseServiceAccount, const std::string& firebaseTopic, const std::string& caBundle,
            NotificationWorker* notificationWorker = 0);
    ~Gateway();
    bool start(std::string* error);
    void stop();
    void poll();
    bool isOnline() const;
    std::string runtimeDirectory() const;
    bool setJoining(unsigned int seconds, std::string* error);
    bool createFreshNetwork(std::string* error);
    bool setDeviceSettings(const std::string& ieeeAddress, const std::string& name,
                           bool hasLow, double low, bool hasHigh, double high,
                           bool hasContactTimings, unsigned long initialAlertMinutes,
                           unsigned long reminderAlertMinutes, bool hasSensorRefreshInterval,
                           unsigned long sensorRefreshIntervalMinutes, std::string* error);
    bool refreshDevice(const std::string& ieeeAddress, std::string* error);
    bool setLightState(const std::string& ieeeAddress, const std::string& state, int brightnessPercent,
                       std::string* error);
    bool setSceneConfiguration(const std::string& encoded, std::string* error);
    bool activateScene(unsigned short groupId, unsigned char sceneId, std::string* error);
    bool turnOffSceneGroup(unsigned short groupId, std::string* error);
    bool applyAttributeReport(const std::string& ieeeAddress, unsigned short cluster, unsigned short attribute,
                              const std::vector<unsigned char>& value);
    bool applyIasZoneStatus(const std::string& ieeeAddress, const std::vector<unsigned char>& payload);
    bool sendTestNotification(std::string* error);
    bool notificationsEnabled() const;
    bool setNotificationsEnabled(bool enabled, std::string* error);
    std::vector<Device> devices() const;
    std::string statusJson() const;
    std::string logsJson() const;

private:
    friend class GatewayTestPeer;
    std::string serialDevice_;
    int serialBaud_;
    std::string runtimeDirectory_;
    std::string firebaseServiceAccount_;
    std::string firebaseTopic_;
    std::string caBundle_;
    NotificationWorker* notificationWorker_;
    int serialFd_;
    bool online_;
    long joinUntil_;
    long joinPermitRenewAt_;
    unsigned int pendingJoinSeconds_;
    unsigned char joinPreparationStep_;
    long joinPreparationDeadline_;
    AshTransport ash_;
    unsigned char ashSendSequence_;
    unsigned char ashReceiveSequence_;
    unsigned char ezspSequence_;
    std::vector<std::vector<unsigned char> > ashTransmitQueue_;
    std::vector<unsigned char> ashPendingData_;
    std::vector<unsigned char> serialPendingBytes_;
    long ashAckDeadline_;
    unsigned int ashRetries_;
    void flushSerialOutput();
    void pollAshTransmit();
    void retransmitAsh();
    unsigned char ezspVersion_;
    unsigned char zdoSequence_;
    unsigned char zclTransactionSequence_;
    bool ezspVersionNegotiated_;
    bool stackTokenWritingRequested_;
    bool networkInitRequested_;
    unsigned char coordinatorInitializationStep_;
    long resetDeadline_;
    long nextHeartbeatAt_;
    unsigned long serialRxBytes_;
    unsigned long ashRxFrames_;
    bool networkForming_;
    unsigned char networkFormationStep_;
    bool networkFormed_;
    std::vector<unsigned char> networkExtendedPan_;
    std::vector<unsigned char> networkKey_;
    std::vector<unsigned char> preconfiguredKey_;
    unsigned short networkPanId_;
    unsigned char networkChannel_;
    std::map<std::string, Device> devices_;
    std::map<unsigned short, std::string> nodeAddresses_;
    enum InterviewOperationType {
        InterviewNodeDescriptor,
        InterviewActiveEndpoints,
        InterviewSimpleDescriptor,
        InterviewBasicRead,
        InterviewBind,
        InterviewAttributeRead,
        InterviewConfigureReporting
    };
    struct InterviewOperation {
        InterviewOperationType type;
        unsigned char endpoint;
        unsigned short cluster;
        unsigned short attribute;
        unsigned char dataType;
        unsigned short minimumInterval;
        unsigned short maximumInterval;
        unsigned short reportableChange;
        unsigned char transactionSequence;
        bool transactionSequenceAssigned;
        unsigned char messageTag;
        unsigned char attempts;
        long retryAt;
        InterviewOperation();
    };
    struct InterviewState {
        std::string ieeeAddress;
        std::vector<InterviewOperation> operations;
        unsigned long operationIndex;
        std::vector<EndpointDescriptor> discoveredEndpoints;
        bool refreshOnly;
        bool paused;
        bool persistentDirty;
        InterviewState();
    };
    std::map<unsigned short, InterviewState> interviews_;
    bool interviewRequestInFlight_;
    bool interviewSendAccepted_;
    unsigned char interviewEzspSequence_;
    unsigned short interviewNode_;
    long interviewDeadline_;
    unsigned char nextMessageTag_;
    std::vector<unsigned char> coordinatorIeeeAddress_;
    bool coordinatorIeeeAddressRequested_;
    std::map<unsigned short, long> nextSensorRefreshAt_;
    std::map<unsigned short, bool> lightSetupPending_;
    std::map<unsigned short, std::string> remoteSceneHolds_;
    std::map<unsigned short, long> lastActiveEndpointRequestAt_;
    std::map<std::string, long> contactOpenedAt_;
    std::map<std::string, long> nextContactAlertAt_;
    std::vector<SceneGroup> sceneGroups_;
    std::vector<RemoteSceneBinding> sceneBindings_;
    std::map<std::string, unsigned char> sceneCursor_; /* RAM only: remote/action */
    std::map<unsigned short, bool> sceneGroupOff_;
    std::vector<std::string> logs_;
    bool configureSerial(std::string* error);
    bool writeBytes(const std::vector<unsigned char>& bytes);
    void sendAshReset();
    void sendEzspCommand(unsigned short command, const std::vector<unsigned char>& payload);
    void continueCoordinatorInitialization();
    unsigned short coordinatorInitializationCommand() const;
    void enableStackTokenWriting();
    void sendApsUnicast(unsigned short destination, unsigned short profile, unsigned short cluster,
                        unsigned char sourceEndpoint, unsigned char destinationEndpoint,
                        const std::vector<unsigned char>& message, unsigned char messageTag = 0);
    void sendApsMulticast(unsigned short groupId, unsigned short profile, unsigned short cluster,
                          unsigned char sourceEndpoint, const std::vector<unsigned char>& message);
    void sendJoinPermit(unsigned int duration);
    void beginJoinWindow(unsigned int seconds);
    void handleAshFrame(const std::vector<unsigned char>& frame);
    void handleEzspFrame(const std::vector<unsigned char>& frame);
    void registerNode(unsigned short nodeId, const unsigned char* eui64);
    int allocateIasZoneId(const std::string& ieeeAddress);
    void sendIasEnrollResponse(unsigned short destination, unsigned char sourceEndpoint,
                               unsigned char destinationEndpoint, unsigned char transactionSequence,
                               unsigned char zoneId);
    void startInterview(unsigned short destination, bool refreshOnly);
    void queueInterviewOperation(unsigned short destination, InterviewOperationType type, unsigned char endpoint,
                                 unsigned short cluster = 0, unsigned short attribute = 0,
                                 unsigned char dataType = 0, unsigned short reportableChange = 0,
                                 unsigned short minimumInterval = 0, unsigned short maximumInterval = 0);
    void sendNextInterviewOperation();
    void sendInterviewOperation(unsigned short destination, InterviewOperation* operation);
    void completeInterviewOperation(unsigned short destination);
    void failInterviewOperation(unsigned short destination, const std::string& reason, bool localFailure = false);
    void buildDescriptorOperations(unsigned short destination, const std::vector<unsigned char>& endpoints);
    void buildConfigurationOperations(unsigned short destination);
    void buildRefreshOperations(unsigned short destination);
    bool parseSimpleDescriptor(const std::vector<unsigned char>& message, EndpointDescriptor* descriptor) const;
    bool endpointSupports(const EndpointDescriptor& endpoint, unsigned short cluster) const;
    bool updateBasicIdentity(Device* device, const std::vector<unsigned char>& records);
    void wakeInterview(unsigned short destination);
    void requestCoordinatorIeeeAddress();
    void sendZdoBindRequest(unsigned short destination, unsigned char endpoint, unsigned short cluster,
                            const std::string& sourceIeeeAddress, unsigned char transactionSequence,
                            unsigned char messageTag);
    void sendZclReadAttribute(unsigned short destination, unsigned char endpoint, unsigned short cluster,
                              unsigned short attribute, unsigned char transactionSequence, unsigned char messageTag);
    void sendZclReadBasic(unsigned short destination, unsigned char endpoint,
                          unsigned char transactionSequence, unsigned char messageTag);
    void sendZclConfigureReporting(unsigned short destination, unsigned char endpoint, unsigned short cluster,
                                   unsigned short attribute, unsigned char dataType, unsigned short minimumInterval,
                                   unsigned short maximumInterval, unsigned short reportableChange,
                                   unsigned char transactionSequence, unsigned char messageTag);
    void sendZclDefaultResponse(unsigned short destination, unsigned char sourceEndpoint, unsigned char destinationEndpoint,
                                unsigned short cluster, unsigned char transactionSequence, unsigned char command);
    void classifyDevice(Device* device) const;
    bool endpointForCluster(const Device& device, unsigned short cluster, unsigned char* endpoint) const;
    void sendZclLightCommand(unsigned short destination, unsigned char endpoint, unsigned short cluster,
                             unsigned char command, const std::vector<unsigned char>& arguments);
    bool logRemoteKeyEvent(unsigned short sender, unsigned short cluster, unsigned char endpoint,
                           unsigned short manufacturer, unsigned char command, const std::vector<unsigned char>& arguments);
    bool sceneMemberNative(const Device& device, const Scene& scene, const SceneLightState& state) const;
    void provisionSceneGroup(const SceneGroup& group);
    void dispatchRemoteSceneAction(const std::string& ieeeAddress, const std::string& action);
    SceneGroup* findSceneGroup(unsigned short id);
    const SceneGroup* findSceneGroup(unsigned short id) const;
    void loadScenes();
    void saveScenes() const;
    void loadSceneRuntime();
    void saveSceneRuntime() const;
    void handleIncomingMessage(const std::vector<unsigned char>& payload);
    bool generateNetworkMaterial(std::string* error);
    void continueNetworkFormation(unsigned short completedCommand);
    void updateContactAlert(const std::string& ieeeAddress, bool wasOpen);
    void updateTemperatureAlert(const Device& device, bool hadTemperature, double previousTemperature);
    bool emitNotification(const std::string& title, const std::string& body, std::string* error);
    std::string deviceLogLabel(unsigned short node) const;
    void addFrameLog(const std::string& direction, unsigned short node, unsigned short profile, unsigned short cluster,
                     unsigned char sourceEndpoint, unsigned char destinationEndpoint, unsigned short options,
                     unsigned short bindingAddress, unsigned char apsSequence, unsigned char messageTag,
                     int lqi, int rssi, const std::vector<unsigned char>& message);
    void addLog(const std::string& message);
    void addHexLog(const std::string& direction, const unsigned char* bytes, unsigned long length);
    void loadLogs();
    void saveLogs() const;
    void loadState();
    void saveState() const;
    void savePersistentState() const;
};

#endif
