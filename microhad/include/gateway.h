#ifndef MICROHAD_ZIGBEE_GATEWAY_H
#define MICROHAD_ZIGBEE_GATEWAY_H

#include <map>
#include <string>
#include <vector>
#include <deque>
#include "log_ring.h"

#include "ash.h"
#include "sensor_history.h"

class NotificationWorker;
class ExternalHttpDeviceWorker;

struct EndpointDescriptor {
    unsigned char endpoint;
    unsigned short profileId;
    unsigned short deviceId;
    unsigned char deviceVersion;
    std::vector<unsigned short> inputClusters;
    std::vector<unsigned short> outputClusters;
    EndpointDescriptor();
};

struct DeviceValueAlert {
    bool hasLow;
    bool hasHigh;
    double low;
    double high;
    double hysteresis;
    bool lowActive;
    bool highActive;
    DeviceValueAlert() : hasLow(false), hasHigh(false), low(0), high(0), hysteresis(0), lowActive(false), highActive(false) {}
};

struct Device {
    std::string source;
    std::string externalId;
    std::string component;
    std::string ieeeAddress;
    unsigned short networkAddress;
    std::string name;
    std::string kind;
    bool contactOpen;
    bool hasContact;
    bool contactKnown;
    bool hasTemperature;
    bool temperatureKnown;
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
    bool hasActor;
    bool actorKnown;
    bool actorOn;
    bool hasPower;
    bool powerKnown;
    double powerWatts;
    int linkQuality;
    bool hasWifiSignal;
    int wifiSignalPercent;
    long lastSeen;
    int iasZoneId;
    bool hasLowTemperatureAlert;
    bool hasHighTemperatureAlert;
    double lowTemperatureAlert;
    double highTemperatureAlert;
    unsigned long contactInitialAlertMinutes;
    unsigned long contactReminderAlertMinutes;
    unsigned long sensorRefreshIntervalMinutes;
    std::map<std::string, DeviceValueAlert> valueAlerts;
    std::string manufacturerName;
    std::string modelId;
    std::string softwareBuildId; /* read during interview; persisted for remote binding compatibility */
    std::vector<EndpointDescriptor> endpoints;
    std::vector<unsigned char> nodeDescriptor; /* raw 13-byte Zigbee Node Descriptor body */
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
struct NativeRemoteBinding {
    std::string ieeeAddress;
    unsigned short groupId;
    NativeRemoteBinding() : groupId(0) {}
};
struct NativeBindingStatus {
    std::string ieeeAddress;
    std::string state;
};

struct DeviceBindingEntry {
    unsigned short clusterId;
    bool groupDestination;
    unsigned short groupId;
    std::string ieeeAddress;
    unsigned char endpoint;
    DeviceBindingEntry() : clusterId(0), groupDestination(false), groupId(0), endpoint(0) {}
};

struct DeviceConfigurationStatus {
    std::string ieeeAddress;
    std::string nodeDescriptorStatus;
    std::string groupsStatus;
    std::vector<unsigned short> groups;
    std::string bindingsStatus;
    std::vector<DeviceBindingEntry> bindings;
};

/* Immutable data which may be safely handed from the coordinator to a web
   worker.  It deliberately contains only dashboard-visible state. */
class GatewayView {
public:
    bool online;
    bool networkForming;
    bool networkFormed;
    unsigned long serialRxBytes;
    unsigned long ashRxFrames;
    long joiningUntil;
    bool joiningPreparing;
    bool notificationsEnabled;
    std::vector<Device> devices;
    std::vector<SceneGroup> sceneGroups;
    std::vector<RemoteSceneBinding> sceneBindings;
    std::vector<NativeRemoteBinding> nativeBindings;
    std::vector<NativeBindingStatus> nativeBindingStatuses;
    std::vector<DeviceConfigurationStatus> deviceConfigurations;
    std::vector<std::string> logs;

    GatewayView();
    std::string statusJson() const;
    std::string logsJson() const;
};

class Gateway {
public:
    Gateway(const std::string& serialDevice, int serialBaud, const std::string& runtimeDirectory,
            const std::string& firebaseServiceAccount, const std::string& firebaseTopic, const std::string& caBundle,
            NotificationWorker* notificationWorker = 0,
            const std::string& historyPath = "/tmp/microhad-zigbee-native-history.bin");
    ~Gateway();
    bool start(std::string* error);
    void stop();
    void poll();
    bool isOnline() const;
    std::string runtimeDirectory() const;
    std::string historyPath() const;
    SensorHistory* historyStore();
    void setShellyTransport(ExternalHttpDeviceWorker* transport);
    bool registerShelly(const std::string& id, const std::string& name, const std::string& oldName,
                        const std::string& model, const std::vector<int>& inputIndices,
                        const std::vector<int>& temperatureIndices, const std::vector<int>& relayIndices,
                        std::string* error);
    bool registerShelly(const std::string& id, const std::string& name, const std::string& oldName,
                        const std::string& model, int temperatureIndex, std::string* error);
    bool removeShelly(const std::string& id, std::string* error);
    bool reconcileShelly(const std::vector<std::string>& ids, std::string* error);
    bool reconcileFritzClients(const std::vector<std::string>& ids, std::string* error);
    bool applyShellyEvent(const std::string& id, int metric, int channelIndex, double value, std::string* error);
    bool applyFritzEvent(const std::string& id, bool connected, bool hasSignal, int signalPercent, std::string* error);
    bool applyShellyEvent(const std::string& id, int metric, double value, std::string* error);
    bool setActorState(const std::string& address, bool on, std::string* error);
    int serialFd() const;
    bool serialNeedsWrite() const;
    bool hasImmediateWork() const;
    long nextPollDeadline() const;
    bool setJoining(unsigned int seconds, std::string* error);
    bool createFreshNetwork(std::string* error);
    bool setDeviceSettings(const std::string& ieeeAddress, const std::string& name,
                           bool hasLow, double low, bool hasHigh, double high,
                           bool hasContactTimings, unsigned long initialAlertMinutes,
                           unsigned long reminderAlertMinutes, bool hasSensorRefreshInterval,
                           unsigned long sensorRefreshIntervalMinutes, std::string* error);
    bool setDeviceValueAlerts(const std::string& ieeeAddress, const std::map<std::string, DeviceValueAlert>& alerts,
                              std::string* error);
    bool refreshDevice(const std::string& ieeeAddress, std::string* error);
    bool identifyDevice(const std::string& ieeeAddress, std::string* error);
    bool queryDeviceConfiguration(const std::string& ieeeAddress, std::string* error);
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
    void logEventLoopDiagnostic(const std::string& message);
    std::vector<Device> devices() const;
    GatewayView captureView(bool includeLogs = true) const;
    bool takeViewDirty();
    std::string statusJson() const;
    std::string logsJson() const;

private:
    friend class GatewayTestPeer;
    friend class GatewayIntegrationTestPeer;
    std::string serialDevice_;
    int serialBaud_;
    std::string runtimeDirectory_;
    std::string networkRandomSource_;
    std::string firebaseServiceAccount_;
    std::string firebaseTopic_;
    std::string caBundle_;
    SensorHistory history_;
    ExternalHttpDeviceWorker* shellyTransport_;
    NotificationWorker* notificationWorker_;
    int serialFd_;
    bool online_;
    long joinUntil_;
    long joinPermitRenewAt_;
    bool joinPermitClosePending_;
    long joinPermitCloseDeadline_;
    unsigned int pendingJoinSeconds_;
    unsigned char joinPreparationStep_;
    long joinPreparationDeadline_;
    AshTransport ash_;
    unsigned char ashSendSequence_;
    unsigned char ashReceiveSequence_;
    unsigned char ezspSequence_;
    std::vector<std::vector<unsigned char> > ashTransmitQueue_;
    std::vector<bool> ashTransmitSensitiveQueue_;
    std::vector<unsigned char> ashPendingData_;
    std::vector<unsigned char> serialPendingBytes_;
    long ashAckDeadline_;
    unsigned int ashRetries_;
    bool ashPendingSensitive_;
    void flushSerialOutput();
    void failSerial(const char* operation, int errorNumber);
    void pollAshTransmit();
    void retransmitAsh();
    unsigned char ezspVersion_;
    unsigned char zdoSequence_;
    unsigned char zclTransactionSequence_;
    bool ezspVersionNegotiated_;
    bool stackTokenWritingRequested_;
    bool networkInitRequested_;
    bool tradfriBindGroupConfigured_;
    unsigned char coordinatorInitializationStep_;
    long coordinatorInitializationDeadline_;
    long resetDeadline_;
    unsigned int serialReconnectFailures_;
    bool zigbeeDownAlertQueued_;
    long nextHeartbeatAt_;
    unsigned long serialRxBytes_;
    unsigned long ashRxFrames_;
    bool networkForming_;
    unsigned char networkFormationStep_;
    long networkFormationDeadline_;
    bool networkFormed_;
    std::vector<unsigned char> networkExtendedPan_;
    std::vector<unsigned char> networkKey_;
    std::vector<unsigned char> preconfiguredKey_;
    unsigned short networkPanId_;
    unsigned char networkChannel_;
    std::map<std::string, Device> devices_;
    std::map<unsigned short, std::string> nodeAddresses_;
    enum InterviewOperationType {
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
    std::vector<NativeRemoteBinding> nativeBindings_;
    std::map<std::string, std::string> nativeBindingStates_;
    struct SceneProvisionCommand {
        unsigned short cluster;
        unsigned short groupId;
        unsigned char sceneId;
        unsigned char endpoint;
        unsigned char transaction;
        std::vector<unsigned char> payload;
        SceneProvisionCommand() : cluster(0), groupId(0), sceneId(0), endpoint(0), transaction(0) {}
    };
    struct SceneProvisionState {
        std::vector<SceneProvisionCommand> commands;
        unsigned long index;
        long deadline;
        bool waitingForTraffic;
        SceneProvisionState() : index(0), deadline(0), waitingForTraffic(false) {}
    };
    std::map<std::string, SceneProvisionState> sceneProvisioning_;
    struct DeviceConfigurationQuery {
        unsigned char nodeDescriptorSequence;
        unsigned char groupsSequence;
        unsigned char bindingsSequence;
        long nodeDescriptorDeadline;
        long groupsDeadline;
        long bindingsDeadline;
        unsigned char bindingStartIndex;
        unsigned char bindingTotal;
        DeviceConfigurationQuery() : nodeDescriptorSequence(0), groupsSequence(0), bindingsSequence(0),
            nodeDescriptorDeadline(0), groupsDeadline(0),
            bindingsDeadline(0), bindingStartIndex(0), bindingTotal(0) {}
    };
    std::map<std::string, DeviceConfigurationStatus> deviceConfigurations_;
    std::map<std::string, DeviceConfigurationQuery> deviceConfigurationQueries_;
    struct NativeBindOperation {
        std::string ieeeAddress;
        unsigned char endpoint;
        unsigned short cluster;
        unsigned char destinationMode; /* 1 group, 3 IEEE */
        unsigned short groupId;
        std::string targetIeee;
        bool unbind;
        bool fallback;
        unsigned char sequence;
        long deadline;
        long retryAt;
        NativeBindOperation() : endpoint(0), cluster(0), destinationMode(0), groupId(0),
            unbind(false), fallback(false), sequence(0), deadline(0), retryAt(0) {}
    };
    std::vector<NativeBindOperation> nativeBindOperations_;
    struct SceneRetry {
        bool off;
        bool inFlight;
        bool retryRequested;
        bool sentOff;
        bool unicast;
        unsigned long memberIndex;
        unsigned char sceneId;
        unsigned char sentSceneId;
        unsigned char ezspSequence;
        unsigned char attempts;
        long sentAt;
        long retryAt;
        SceneRetry();
    };
    std::map<unsigned short, SceneRetry> sceneRetries_;
    std::map<unsigned short, unsigned char> selectedScenes_; /* RAM only */
    std::map<unsigned short, bool> sceneGroupOff_;
    LogRing logRing_;
    std::deque<std::string> logs_;
    unsigned long logBytes_;
    bool viewDirty_;
    bool configureSerial(std::string* error);
    bool writeBytes(const std::vector<unsigned char>& bytes, bool sensitive = false);
    void sendAshReset();
    unsigned char sendEzspCommand(unsigned short command, const std::vector<unsigned char>& payload);
    void continueCoordinatorInitialization();
    unsigned short coordinatorInitializationCommand() const;
    void enableStackTokenWriting();
    void configureTradfriBindGroup();
    unsigned char sendApsUnicast(unsigned short destination, unsigned short profile, unsigned short cluster,
                        unsigned char sourceEndpoint, unsigned char destinationEndpoint,
                        const std::vector<unsigned char>& message, unsigned char messageTag = 0);
    unsigned char sendApsMulticast(unsigned short groupId, unsigned short profile, unsigned short cluster,
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
    void startInterview(unsigned short destination, bool refreshOnly, bool forceEndpointDiscovery = false);
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
    void queueNativeBindingChanges(const std::vector<NativeRemoteBinding>& oldBindings,
                                   const std::vector<SceneGroup>& oldGroups, bool force = false);
    void pollNativeBindings();
    bool nativeBindingEnabled(const std::string& ieeeAddress) const;
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
    void queueSceneProvisioning();
    void pollSceneProvisioning();
    void wakeSceneProvisioning(const std::string& ieeeAddress);
    bool handleSceneProvisionResponse(const std::string& ieeeAddress, unsigned short cluster,
                                      unsigned char transaction, unsigned char command,
                                      const std::vector<unsigned char>& message, unsigned long argumentOffset);
    void dispatchRemoteSceneAction(const std::string& ieeeAddress, const std::string& action);
    void pollSceneRetries();
    bool sendSceneRetry(unsigned short groupId, SceneRetry* retry);
    bool scheduleSceneRetry(SceneRetry* retry, long now);
    void completeSceneRetry(unsigned short groupId, SceneRetry* retry);
    SceneGroup* findSceneGroup(unsigned short id);
    const SceneGroup* findSceneGroup(unsigned short id) const;
    void loadScenes();
    void saveScenes();
    void loadSceneRuntime();
    void saveSceneRuntime() const;
    void handleIncomingMessage(const std::vector<unsigned char>& payload);
    bool generateNetworkMaterial(std::string* error);
    void continueNetworkFormation(unsigned short completedCommand);
    void updateContactAlert(const std::string& ieeeAddress, bool wasOpen);
    void recordHistory(const Device& before, const Device& after, unsigned observed, long timestamp);
    void updateTemperatureAlert(const Device& device, bool hadTemperature, double previousTemperature);
    void updateValueAlerts(Device& device, const Device& before);
    bool emitNotification(const std::string& title, const std::string& body, std::string* error);
    std::string deviceLogLabel(unsigned short node) const;
    void addFrameLog(const std::string& direction, unsigned short node, unsigned short profile, unsigned short cluster,
                     unsigned char sourceEndpoint, unsigned char destinationEndpoint, unsigned short options,
                     unsigned short bindingAddress, unsigned char apsSequence, unsigned char messageTag,
                     int lqi, int rssi, const std::vector<unsigned char>& message);
    void addLog(const std::string& message);
    void markViewDirty();
    void addHexLog(const std::string& direction, const unsigned char* bytes, unsigned long length);
    void loadLogs();
    void loadState();
    void saveState() const;
    void savePersistentState() const;
};

#endif
