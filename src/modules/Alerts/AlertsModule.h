#pragma once

#if defined(HAS_ALERTING) && HAS_ALERTING

#include "concurrency/Periodic.h"
#include "configuration.h"
#include "mesh/Channels.h"
#include "AlertSource.h"
#include "DynamicSource.h"
#include <Arduino.h>
#include <vector>
#include <unordered_set>
#include <deque>
#include <ArduinoJson.h>

// Alert structure - shared between AlertsModule and AlertSource
struct Alert {
    uint32_t id;       // Unique identifier hash (for file naming and duplicate detection)
    String title;
    String link;       // URL or identifier (for display/debugging, not saved to disk)
    String valid_from; // YYYY-MM-DD hh:mm:ss format
    String valid_to;   // YYYY-MM-DD hh:mm:ss format
    String location;   // Extracted powiat/region
    String alert_type;
    String message;    // Processed message for sending
    String source;     // Source identifier (e.g., "RCB")
    uint8_t severity;  // 0=critical (war, large disaster) to 10=very local/unimportant
    unsigned long lastSent;
    unsigned long addedAt;
    unsigned long nextSendAt;
};

class AlertsModule : public concurrency::OSThread {
  public:
    AlertsModule();
    virtual ~AlertsModule();
    int32_t runOnce() override;

  private:
    // ===== Configuration Variables =====
    // These can be easily changed and later moved to config files

      // Debugging settings
      static constexpr bool PURGE_ALERTS_ON_BOOT = false;

    // Alert sources (pluggable architecture) - persistent alerts with AI processing
    static constexpr int MAX_ALERT_SOURCES = 5;
    AlertSource* sources[MAX_ALERT_SOURCES];
    int numSources;
    int currentSourceIndex;
    unsigned long sourceLastFetchTime[MAX_ALERT_SOURCES];

    // Dynamic sources (pluggable architecture) - periodic data, no persistence, no AI
    static constexpr int MAX_DYNAMIC_SOURCES = 5;
    DynamicSource* dynamicSources[MAX_DYNAMIC_SOURCES];
    int numDynamicSources;
    unsigned long dynamicSourceLastFetchTime[MAX_DYNAMIC_SOURCES];


    unsigned long intervalMs;
    
    // HTTP/Network settings
    static constexpr unsigned long HTTP_TIMEOUT_MS = 10000;

    // Shared JSON document for streaming parsers (avoids per-source allocation)
    static constexpr size_t SHARED_JSON_DOC_SIZE = 16384;
    DynamicJsonDocument sharedJsonDoc;

    // Time synchronization settings
    static constexpr uint32_t MIN_VALID_EPOCH = 1577836800UL; // 2020-01-01 00:00:00 UTC
    static constexpr unsigned long TIME_SYNC_WAIT_MS = 30000;
    static constexpr unsigned long WIFI_UNAVAILABLE_WAIT_MS = 30000;
    static constexpr unsigned long FETCH_FAIL_WAIT_MS = 60000;

    // Message settings
    static constexpr int MAX_LOCATION_BYTES = 35; // Maximum bytes for location suffix "[location]"
    static constexpr int MESSAGE_SAFETY_MARGIN = 10;
    static constexpr int MAX_LOCATION_CHARS = 30;

    // Severity-based send intervals
    static constexpr unsigned long SEVERITY_MIN_INTERVAL_SEC = 30 * 60; // 30 minutes for critical (severity 0)
    static constexpr unsigned long SEVERITY_MAX_INTERVAL_SEC = 12 * 60 * 60; // 12 hours for least important (severity 10)
    static constexpr uint8_t DEFAULT_SOURCE_SEVERITY = 3;

    // New alert send delay
    static constexpr unsigned long NEW_ALERT_SEND_DELAY_MS = 10000; // Delay for new alerts (10 seconds)

    // Storage and cleanup settings
    static constexpr unsigned long CLEANUP_INTERVAL_MS = 60 * 60 * 1000;
    static constexpr unsigned long ALERT_RETENTION_DAYS = 5; // Keep alerts for duplicate detection
    static constexpr int MAX_FILES_ON_DISK = 200; // Maximum alerts kept in alerts.bin
    static constexpr size_t MIN_FREE_SPACE_BYTES = 50 * 1024; // Minimum 50KB free space required

    // Channel settings
    String alertChannelName; // Channel name (empty string = use default/primary channel)
    int8_t lastKnownChannelIndex = -2; // -2 = unknown, -1 = not found, 0+ = channel index

    // Broadcasting settings
    static constexpr unsigned long BROADCAST_INTERVAL_MS = 8 * 60 * 60 * 1000; // 8 hours
    static constexpr unsigned long BROADCAST_INITIAL_DELAY_MS = 20 * 60 * 1000; // 20 minutes after boot

    // Storage paths
    static constexpr const char* ALERTS_DIR = "/alerts";
    static constexpr const char* ALERTS_DATA_FILE = "/alerts/alerts.bin";
    static constexpr const char* ALERTS_DATA_FILE_TMP = "/alerts/alerts.bin.tmp";
    static constexpr const char* PROCESSED_IDS_FILE = "/alerts/processed_ids.bin";
    static constexpr const char* PROCESSED_IDS_FILE_TMP = "/alerts/processed_ids.bin.tmp";
    
    // ===== End Configuration Variables =====

    // Performance and throttling settings
    static constexpr unsigned long ALERT_PROCESSING_YIELD_MS = 100;
    static constexpr unsigned long RESEND_CHECK_YIELD_MS = 500;
    static constexpr unsigned long MAX_RUNONCE_INTERVAL_MS = 15000;
    static constexpr unsigned long ALERT_PROCESSING_THROTTLE_MS = 2000; // Minimum 2 seconds between alert processing starts
    static constexpr int MAX_ALERTS_PER_CYCLE = 1;

    // Memory management settings
    // We only keep valid (non-expired) alerts in memory
    static constexpr int MAX_ALERTS_IN_MEMORY = 400; // Reasonable upper limit for edge cases
    static constexpr int MAX_PENDING_ALERTS = 30; // Limit pending alerts queue
    static constexpr unsigned long MEMORY_CHECK_INTERVAL_MS = 60000;
    static constexpr size_t MAX_PROCESSED_IDS_CACHE = 800; // Limit processed IDs cache size

    // Logging throttling settings
    static constexpr unsigned long PENDING_ALERT_LOG_INTERVAL_MS = 3 * 60 * 1000; // Log pending alerts every 3 minutes

    // ===== Alert Storage Format =====
    // Binary format for disk storage (fixed size for fast I/O)
    struct AlertBinary {
        uint32_t id;
        char title[128];
        char message[256];
        char location[64];
        char source[16];
        char valid_from[32];
        char valid_to[32];
        uint8_t severity;
        uint32_t addedAt;
        uint32_t lastSent;
        uint32_t nextSendAt;
    };
    // Total: 548 bytes fixed size (19% smaller than previous 676 bytes)

    struct AlertStorageHeader {
        uint32_t magic;
        uint16_t version;
        uint16_t reserved;
        uint32_t alertCount;
    };

    static constexpr uint32_t ALERTS_STORAGE_MAGIC = 0x41524c54; // "ALRT"
    static constexpr uint16_t ALERTS_STORAGE_VERSION = 1;

    struct ProcessedRefRecord {
        uint32_t id;
        uint32_t seenAt;
    };

    struct ProcessedRefsHeader {
        uint32_t magic;
        uint16_t version;
        uint16_t reserved;
        uint32_t refCount;
    };

    static constexpr uint32_t PROCESSED_IDS_MAGIC = 0x50524f43; // "PROC"
    static constexpr uint16_t PROCESSED_IDS_VERSION = 1;
    
    // ===== State Management =====
    enum class ModuleState {
        INIT,
        IDLE,
        FETCHING_PAGE,
        PARSING_ALERTS,
        PROCESSING_ALERT,
        FETCHING_ARTICLE,
        CALLING_AI,
        SAVING_ALERT,
        SENDING_ALERT,
        FETCHING_DYNAMIC,  // Fetching from a dynamic source (weather, etc.)
        CLEANUP
    };

    ModuleState currentState;
    bool initializationDone;
    unsigned long lastFetchTime;
    unsigned long lastCleanupTime;

    // Processing state for current alert
    struct ProcessingContext {
        bool active;
        AlertSource* source;
        AlertSource::RawAlert rawAlert;
        Alert alert;
        unsigned long stateStartTime;
    };
    ProcessingContext processingCtx;

    // Pending alerts queue (alerts found but not yet processed)
    struct PendingAlert {
        AlertSource* source;
        AlertSource::RawAlert rawAlert;
        bool needsFullFetch;  // True if full content still needs to be fetched
    };
    std::vector<PendingAlert> pendingAlerts;

    // Stored alerts
    std::vector<Alert> alerts;

    // Cache of processed alert IDs for fast duplicate checking (prevents slow filesystem scans)
    std::unordered_set<uint32_t> processedAlertIds;
    std::deque<uint32_t> processedAlertIdOrder;

    // Memory management
    unsigned long lastMemoryCheckTime;
    size_t lastReportedMemoryUsage;

    // Logging throttling
    unsigned long lastPendingAlertLogTime;

    // Dynamic source processing state
    int currentDynamicSourceIndex;

    // Broadcasting state
    unsigned long nextBroadcastTimeMs;
    bool broadcastingEnabled;

  public:
    // ===== Core Module Functions =====
    bool loadConfig();

    // HTTP helper (used by sources via callback)
    String httpGet(const char *url, int &httpCode);

    // Streaming HTTP helper with shared JSON document (for maximum memory efficiency)
    bool httpGetStream(const char *url, std::function<bool(WiFiClient* stream, DynamicJsonDocument& doc)> jsonProcessor);

    // ===== Alert Storage and Management =====
    // Persist alerts to disk and load them (using alert storage container)
    bool saveAlertToDisk(const Alert &alert);
    bool saveAlertToFile(const Alert &alert, uint32_t id, const String &dateStr = "");
    bool loadAlertsFromDisk();

    bool saveAlertsToSingleFile();
    bool saveProcessedIdsToSingleFile();
    bool loadAlertsFromSingleFile();
    bool loadLegacyAlertFiles();
    bool loadProcessedIdsFromSingleFile();
    bool cleanupTempFilesFromAlertsDir();
    void upsertAlertInMemory(const Alert &alert);
    Alert toAlert(const AlertBinary &binAlert);
    bool fillAlertBinary(const Alert &alert, AlertBinary &binAlert);

    // Clean up old alert files (older than retention period)
    void cleanupOldAlerts();

    // Enforce storage limit by trimming alerts vector and rewriting alerts.bin
    void enforceFileLimits();

    // Check if there's enough free space on filesystem
    bool hasEnoughFreeSpace();

    // Purge all alerts (delete all alert files and clear in-memory alerts)
    void purgeAllAlerts();

    // Helper to check duplicate
    bool alertExists(uint32_t id);
    bool isAlertProcessed(uint32_t id);

    // ===== AI Processing =====
    // Call AI to extract structured data from raw alert
    // Uses the source's buildAIPrompt() to get the prompt
    bool callAIForExtraction(AlertSource* source, const AlertSource::RawAlert &rawAlert,
                           String &outMessage, String &outStart, String &outEnd,
                           String &outWhere, uint8_t &outSeverity);


    // Parse AI response
    bool parseAIResponse(const String &response, String &outMessage, String &outStart,
                        String &outEnd, String &outWhere, uint8_t &outSeverity);


    // ===== Alert Sending and Scheduling =====
    // Get send interval based on severity (in milliseconds)
    unsigned long getSendInterval(uint8_t severity);

    // Check if alert is still valid (within valid_from and valid_to dates)
    bool isAlertValid(const Alert &alert);

    // Find Alert channel by name, returns -1 if not found
    int8_t findAlertChannel();

    // Send alert to mesh network
    bool sendAlertToMesh(const Alert &alert);

    // Send a simple message to mesh network (for dynamic sources)
    bool sendMessageToMesh(const String &message);

    // ===== Broadcasting Functions =====
    // Check if broadcasting should be enabled (when alert channel is configured)
    bool shouldBroadcastInfo();

    // Get the encryption key for the alert channel as base64 string
    String getChannelEncryptionKey();

    // Broadcast channel information to primary channel
    bool broadcastInfoMessage();

    // Simple base64 encoder for PSK display
    String base64Encode(const uint8_t* data, size_t length);

    // ===== Utility Functions =====
    // State machine helpers
    static const char* stateName(ModuleState state);
    void transitionToState(ModuleState nextState, const char *reason);

    // Calculate UTF-8 byte length of a string
    size_t utf8ByteLength(const String &str);

    // Parse date string (YYYY-MM-DD hh:mm:ss or DD.MM.YYYY formats) to time_t
    time_t parseDateString(const String &dateStr);

    // Check if WiFi is available and time is synced
    bool isWifiConnectedAndTimeSynced();

    // Check if WiFi is available for network operations
    bool isWifiAvailable();

    // Hash a link string for duplicate detection
    uint32_t hashLink(const String &link);

    // Processed ID cache helpers (bounded, prevents unbounded growth)
    void cacheProcessedAlertId(uint32_t id);
    void removeProcessedAlertId(uint32_t id);

    // Ensure payload fits meshtastic limit (byte-based, defensive)
    size_t clampMessageToPayload(String &message, size_t maxPayloadBytes);
};

#endif // HAS_ALERTING
