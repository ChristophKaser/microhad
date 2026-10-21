#ifndef MICROHAD_ZIGBEE_GATEWAY_H
#define MICROHAD_ZIGBEE_GATEWAY_H

#include <map>
#include "thermostat.h"
#include <string>
#include <vector>
#include <deque>
#include <stdint.h>

#include "ash.h"
#include "network_backup.h"
#include "sensor_history.h"

class ZigguratClient;
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

struct MatterEndpoint {
    std::string id;
    std::string node;
    std::string state;
    unsigned short endpoint;
    bool online;
    bool metadataComplete;
    std::string manufacturer;
    std::string model;
    int rssiDbm;
    std::vector<unsigned int> servers;
    std::vector<unsigned int> clients;
    std::vector<unsigned int> deviceTypes;
    MatterEndpoint() : endpoint(0), online(false), metadataComplete(false), rssiDbm(1) {}
};

struct MatterReading {
    std::string node;
    unsigned short endpoint;
    unsigned int cluster;
    unsigned int attribute;
    bool known;
    bool booleanValue;
    double numberValue;
    std::string textValue;
    std::string unit;
    MatterReading() : endpoint(0), cluster(0), attribute(0), known(false), booleanValue(false), numberValue(0) {}
};

struct MatterButtonEvent {
    std::string node;
    unsigned short endpoint;
    unsigned char event;
    int position;
    int pressCount;
    MatterButtonEvent() : endpoint(0), event(0), position(-1), pressCount(-1) {}
};

struct DeviceValueAlert {
    bool hasLow;
    bool hasHigh;
    double low;
    double high;
    double hysteresis;
    std::string unit;
    bool lowActive;
    bool highActive;
    DeviceValueAlert() : hasLow(false), hasHigh(false), low(0), high(0), hysteresis(0), lowActive(false), highActive(false) {}
};

struct Device {
    TemperatureObservation temperatureObservation;
    ThermostatState thermostat;
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
    bool hasWaterLeak;
    bool waterLeakKnown;
    bool waterDetected;
    bool hasTemperature;
    bool temperatureKnown;
    double temperatureCelsius;
    bool hasHumidity;
    bool humidityKnown;
    double humidityPercent;
    bool hasAirQuality;
    bool airQualityKnown;
    std::string airQuality;
    bool hasCo2;
    bool co2Known;
    double co2Ppm;
    std::string co2Unit;
    bool hasPm25;
    bool pm25Known;
    double pm25MicrogramsM3;
    std::string pm25Unit;
    bool hasOccupancy;
    bool occupied;
    bool hasBattery;
    int batteryPercent;
    bool hasLight;
    bool lightOn;
    bool lightKnown;
    bool hasBrightness;
    int brightnessPercent;
    bool brightnessKnown;
    bool hasRemote;
    bool hasActor;
    bool actorKnown;
    bool actorOn;
    bool hasPower;
    bool powerKnown;
    double powerWatts;
    int linkQuality;
    int matterRssiDbm; /* 1 means unavailable; valid direct Thread RSSI is -128..0 */
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
    bool contactTriggerMode;
    unsigned long contactTriggerCooldownSeconds;
    long contactTriggerUntil; /* transient status snapshot only; never persisted */
    unsigned long sensorRefreshIntervalMinutes;
    std::map<std::string, DeviceValueAlert> valueAlerts;
    std::string manufacturerName;
    std::string modelId;
    std::string softwareBuildId; /* read during interview; persisted for remote binding compatibility */
    bool initialInterviewComplete;
    std::string configuredSignature; /* last successfully applied reporting/binding configuration */
    std::vector<EndpointDescriptor> endpoints;
    std::string matterState;
    std::string matterMetadataState;
    std::string matterNode;
    int matterEndpoint;
    bool online;
    std::vector<unsigned int> matterServers;
    std::vector<unsigned int> matterClients;
    std::vector<unsigned int> matterDeviceTypes;
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
    std::string coordinatorBackend;
    std::string settingsJson;
    std::string matterCountryCode;
    std::string matterRegulatoryLocation;
    bool online;
    bool networkForming;
    bool networkFormed;
    unsigned char observedNetworkChannel;
    unsigned long serialRxBytes;
    unsigned long ashRxFrames;
    long serialOpenedAt;
    long joiningUntil;
    bool joiningPreparing;
    bool notificationsEnabled;
    std::vector<Device> devices;
    std::vector<SceneGroup> sceneGroups;
    std::vector<RemoteSceneBinding> sceneBindings;
    std::vector<NativeRemoteBinding> nativeBindings;
    std::vector<NativeBindingStatus> nativeBindingStatuses;
    std::vector<DeviceConfigurationStatus> deviceConfigurations;
#ifdef MICROHAD_TESTING
    std::vector<std::string> logs;
#endif

    GatewayView();
    std::string statusJson() const;
#ifdef MICROHAD_TESTING
    std::string logsJson() const;
#endif
};

class Gateway {
public:
    Gateway(const std::string& serialDevice, int serialBaud, const std::string& runtimeDirectory,
            const std::string& firebaseServiceAccount, const std::string& firebaseTopic, const std::string& caBundle,
            NotificationWorker* notificationWorker = 0,
            const std::string& historyPath = "/tmp/microhad-history.bin",
            const std::string& historyFlashPath = "");
    ~Gateway();
#if MICROHAD_ZIGGURAT_ENABLED
    void useZiggurat(const std::string& socketPath);
#endif
    void configureBroadcastQuorum(unsigned int value);
    void configureMatterCommissioningRegion(const std::string& countryCode, const std::string& location);
    bool setBroadcastQuorum(unsigned int value, std::string* error);
    bool setMatterCommissioningRegion(const std::string& countryCode, const std::string& location,
                                      std::string* error);
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
    bool reconcileMatterDevices(const std::vector<MatterEndpoint>& endpoints, std::string* error);
    bool applyMatterReading(const MatterReading& reading, std::string* error);
    bool applyMatterButtonEvent(const MatterButtonEvent& event, std::string* error);
    bool reconcileFritzClients(const std::vector<std::string>& ids, std::string* error);
    bool applyShellyEvent(const std::string& id, int metric, int channelIndex, double value, std::string* error);
    bool applyFritzEvent(const std::string& id, bool connected, bool hasSignal, int signalPercent, std::string* error);
    bool applyShellyEvent(const std::string& id, int metric, double value, std::string* error);
    bool setActorState(const std::string& address, bool on, std::string* error);
    int serialFd() const;
    bool serialNeedsWrite() const;
    bool hasImmediateWork() const;
    /* Coordinator-only: clears inactive timers before choosing a deadline. */
    long nextPollDeadline(std::string* source = 0);
    bool setJoining(unsigned int seconds, std::string* error);
    bool createFreshNetwork(std::string* error);
    bool exportNetworkBackup(const std::string& id, std::string* error);
    bool setDeviceSettings(const std::string& ieeeAddress, const std::string& name,
                           bool hasLow, double low, bool hasHigh, double high,
                           bool hasContactTimings, unsigned long initialAlertMinutes,
                           unsigned long reminderAlertMinutes, bool hasSensorRefreshInterval,
                           unsigned long sensorRefreshIntervalMinutes, std::string* error,
                           bool hasContactTriggerSettings = false, bool contactTriggerMode = false,
                           unsigned long contactTriggerCooldownSeconds = 60);
    bool setDeviceValueAlerts(const std::string& ieeeAddress, const std::map<std::string, DeviceValueAlert>& alerts,
                              std::string* error);
    bool deleteDevice(const std::string& address, bool local, std::string* error);
    bool deviceDeletionStatus(const std::string& address, std::string* json, std::string* error);
    bool matterDeletionTarget(const std::string& address, std::string* node, std::string* error);
    bool removeMatterNode(const std::string& node, std::string* error);
    bool reconcileMatterNodes(const std::vector<std::string>& nodes, std::string* error);
    bool refreshDevice(const std::string& ieeeAddress, std::string* error);
    bool reconfigureDevice(const std::string& ieeeAddress, std::string* error);
    bool identifyDevice(const std::string& ieeeAddress, std::string* error);
    bool queryDeviceConfiguration(const std::string& ieeeAddress, std::string* error);
    bool setThermostat(const std::string& address, const std::string& field,
                       const std::string& value, std::string* error, std::string* matterRequest = 0);
    bool thermostatResult(const std::string& node, unsigned short endpoint, const std::string& field, const std::string& error);
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
#if MICROHAD_ZIGGURAT_ENABLED
    ZigguratClient* ziggurat_;
    std::string zigguratSocket_;
    struct ZigguratSend {
        unsigned char sequence;
        bool unicast;
        ZigguratSend() : sequence(0), unicast(false) {}
    };
    std::map<unsigned int, ZigguratSend> zigguratSends_;
    std::deque<ZigguratSend> zigguratFailedSends_;
    void pollZiggurat();
    void zigguratSendResult(unsigned char sequence, bool unicast, unsigned char status);
#endif
    std::string serialDevice_;
    int serialBaud_;
    std::string runtimeDirectory_;
    unsigned int broadcastQuorum_;
    std::string matterCountryCode_;
    std::string matterRegulatoryLocation_;
    unsigned int appliedBroadcastQuorum_;
    unsigned int broadcastQuorumRequest_;
    unsigned int sentBroadcastQuorum_;
    std::string broadcastQuorumState_;
    std::string broadcastQuorumError_;
    bool loadGatewaySettings(std::string* error);
    void applyBroadcastQuorum();
    std::string gatewaySettingsJson() const;
    std::string networkRandomSource_;
    std::string networkRandomReadinessSource_;
    std::string firebaseServiceAccount_;
    std::string firebaseTopic_;
    std::string caBundle_;
    SensorHistory history_;
    ExternalHttpDeviceWorker* shellyTransport_;
    NotificationWorker* notificationWorker_;
    int serialFd_;
    long serialOpenedAt_;
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
    NetworkBackupExport networkBackup_;
    NetworkBackupExport::Query networkBackupQuery_;
    unsigned char networkBackupSequence_;
    long networkBackupDeadline_;
    long networkBackupStarted_;
    void advanceNetworkBackup();
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
    struct SecurityDiagnosticQuery {
        unsigned char value;
        long requestedAt;
        std::string reason;
    };
    std::map<unsigned char, SecurityDiagnosticQuery> securityDiagnosticQueries_;
    std::map<unsigned char, std::pair<unsigned long, long> > securityCounterSamples_;
    long lastSecurityDiagnosticsAt_;
    unsigned int unsupportedSecurityValues_;
    int lastNetworkKeySequence_;
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
    unsigned char observedNetworkChannel_;
    std::map<std::string, Device> devices_;
    std::map<unsigned short, std::string> nodeAddresses_;
    struct ThermostatOperation {
        std::string address;
        std::string field;
        unsigned short cluster;
        unsigned char endpoint;
        unsigned char command;
        unsigned char sequence;
        unsigned int attempts;
        long deadline;
        bool readback;
        std::vector<unsigned char> payload;
        ThermostatOperation() : cluster(0), endpoint(0), command(0), sequence(0), attempts(0), deadline(0), readback(false) {}
    };
    std::deque<ThermostatOperation> thermostatOperations_;
    void queueThermostatReadings(const Device& device);
    void pollThermostats();
    long thermostatDeadline() const;
    bool handleThermostatMessage(const std::string& address, unsigned char endpoint, unsigned short cluster,
        unsigned char frameControl, unsigned short manufacturer, unsigned char sequence, unsigned char command,
        const std::vector<unsigned char>& payload);
    void queueThermostatOperation(const Device& device, const std::string& field, unsigned short cluster,
        unsigned char command, const std::vector<unsigned char>& payload);
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
        bool configurationPending;
        bool forceConfiguration;
        bool configurationFailed;
        std::string configurationSignature;
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
    struct RemoteCommand {
        std::vector<unsigned char> signature;
        int64_t receivedAt;
    };
    std::map<std::string, std::deque<RemoteCommand> > remoteCommands_;
    std::map<unsigned short, long> lastActiveEndpointRequestAt_;
    std::map<std::string, long> contactOpenedAt_;
    std::map<std::string, long> nextContactAlertAt_;
    std::map<std::string, long> nextContactTriggerAt_;
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
#ifdef MICROHAD_TESTING
    std::deque<std::string> logs_;
#endif
    bool viewDirty_;
    mutable bool matterInventoryDirty_;
    mutable time_t matterInventorySaveAt_;
    struct DeviceDeletion {
        std::string state;
        std::string error;
        unsigned short node;
        unsigned char sequence;
        long deadline;
        DeviceDeletion() : node(0xffff), sequence(0), deadline(0) {}
    };
    std::map<std::string, DeviceDeletion> deviceDeletions_;
    bool removeDeviceRecords(const std::vector<std::string>& addresses, std::string* error);
    void completeDeviceDeletion(const std::string& address);
    bool configureSerial(std::string* error);
    bool writeBytes(const std::vector<unsigned char>& bytes, bool sensitive = false);
    void sendAshReset();
    unsigned char sendEzspCommand(unsigned short command, const std::vector<unsigned char>& payload);
    void continueCoordinatorInitialization();
    unsigned short coordinatorInitializationCommand() const;
    void enableStackTokenWriting();
    void requestSecurityDiagnostics(const std::string& reason);
    void logSecurityCounterSnapshot(const char* reason);
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
    void startInterview(unsigned short destination, bool refreshOnly, bool forceEndpointDiscovery = false,
                        bool forceConfiguration = false);
    std::string deviceConfigurationSignature(const Device& device) const;
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
    bool sensorRefreshEligible(unsigned short nodeId) const;
    void resetInactiveDeadlines();
    void resetInactiveDeadline(long* deadline, const std::string& source);
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
                           unsigned short manufacturer, unsigned char command, const std::vector<unsigned char>& arguments,
                           int transaction = -1);
    bool logMatterButtonEvent(const MatterButtonEvent& event);
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
    void handleApsMessage(unsigned short sender, unsigned short profile, unsigned short cluster,
                          unsigned char sourceEndpoint, unsigned char destinationEndpoint,
                          unsigned short options, unsigned short group, unsigned char sequence,
                          unsigned char messageType, unsigned char lqi, signed char rssi,
                          const std::vector<unsigned char>& message);
    void handleIncomingMessage(const std::vector<unsigned char>& payload);
    bool generateNetworkMaterial(std::string* error);
    void continueNetworkFormation(unsigned short completedCommand);
    void updateContactAlert(const std::string& ieeeAddress, bool wasOpen, bool hadKnownState);
    void recordHistory(const Device& before, const Device& after, unsigned observed, long timestamp);
    void updateTemperatureAlert(const Device& device, bool hadTemperature, double previousTemperature);
    void updateValueAlerts(Device& device, const Device& before);
    bool emitNotification(const std::string& title, const std::string& body, std::string* error,
                          const std::string& androidChannelId = "");
    bool emitStateNotification(const std::string& title, const std::string& body, const std::string& key,
                               bool clear, std::string* error);
    std::string deviceLogLabel(unsigned short node) const;
    void addFrameLog(const std::string& direction, unsigned short node, unsigned short profile, unsigned short cluster,
                     unsigned char sourceEndpoint, unsigned char destinationEndpoint, unsigned short options,
                     unsigned short bindingAddress, unsigned char apsSequence, unsigned char messageTag,
                     int lqi, int rssi, const std::vector<unsigned char>& message);
    void addLog(const std::string& message);
    void markViewDirty();
    void addHexLog(const std::string& direction, const unsigned char* bytes, unsigned long length);
    void loadState();
    void saveState() const;
    std::string resolveDeviceId(const std::string& id) const;
    void savePersistentState() const;
};

#endif
