#pragma once

#if defined(HAS_ALERTING) && HAS_ALERTING

#include "concurrency/Periodic.h"
#include "configuration.h"
#include "mesh/Channels.h"
#include "AlertSource.h"
#include "DynamicSource.h"
#include <Arduino.h>
#include <vector>

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

    // AI provider configuration (primary and fallbacks)
    struct AIProvider {
        String name;
        String endpoint;
        String apiKey;
        String requestFormat; // "gemini", "groq", or "mistral"
        String model;         // Model name for providers that need it (Groq, Mistral)
    };
    static constexpr int MAX_AI_PROVIDERS = 4;
    AIProvider aiProviders[MAX_AI_PROVIDERS];
    int currentAIProviderIndex;

    unsigned long intervalMs;
    
    // HTTP/Network settings
    static constexpr unsigned long HTTP_TIMEOUT_MS = 10000;
    static constexpr unsigned long AI_TIMEOUT_MS = 15000;

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
    static constexpr unsigned long SEVERITY_0_INTERVAL_MS = 30 * 60 * 1000; // 30 minutes for critical (severity 0)
    static constexpr unsigned long SEVERITY_10_INTERVAL_MS = 1 * 60 * 60 * 1000; // 1 hour for least important (severity 10)
    static constexpr uint8_t DEFAULT_SOURCE_SEVERITY = 3;

    // New alert send delay
    static constexpr unsigned long NEW_ALERT_SEND_DELAY_MS = 10000; // Delay for new alerts (10 seconds)

    // Storage and cleanup settings
    static constexpr unsigned long CLEANUP_INTERVAL_MS = 60 * 60 * 1000;
    static constexpr unsigned long ALERT_RETENTION_DAYS = 30; // Keep processed markers for duplicate detection

    // Channel settings
    String alertChannelName; // Channel name (empty string = use default/primary channel)
    int8_t lastKnownChannelIndex = -2; // -2 = unknown, -1 = not found, 0+ = channel index

    // Storage paths
    static constexpr const char* ALERTS_DIR = "/alerts";
    
    // ===== End Configuration Variables =====

    // Performance and throttling settings
    static constexpr unsigned long ALERT_PROCESSING_YIELD_MS = 100;
    static constexpr unsigned long RESEND_CHECK_YIELD_MS = 50;
    static constexpr unsigned long MAX_RUNONCE_INTERVAL_MS = 15000;
    static constexpr int MAX_ALERTS_PER_CYCLE = 1;

    // Memory management settings
    // No longer limited - we only keep valid (non-expired) alerts in memory
    static constexpr int MAX_ALERTS_IN_MEMORY = 200; // Reasonable upper limit for edge cases
    static constexpr unsigned long MEMORY_CHECK_INTERVAL_MS = 60000;

    // Debugging settings
    static constexpr bool PURGE_ALERTS_ON_BOOT = false;
    
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

    // Memory management
    unsigned long lastMemoryCheckTime;
    size_t lastReportedMemoryUsage;

    // Dynamic source processing state
    int currentDynamicSourceIndex;

    // ===== Core Module Functions =====
    bool loadConfig();

    // HTTP helper (used by sources via callback)
    String httpGet(const char *url, int &httpCode);

    // ===== Alert Storage and Management =====
    // Persist alerts to disk and load them (using individual files)
    bool saveAlertToDisk(const Alert &alert);
    bool saveAlertToFile(const Alert &alert, uint32_t id, const String &dateStr = "");
    bool loadAlertsFromDisk();

    // Clean up old alert files (older than 30 days)
    void cleanupOldAlerts();

    // Purge all alerts (delete all alert files and clear in-memory alerts)
    void purgeAllAlerts();

    // Helper to check duplicate
    bool alertExists(uint32_t id);
    bool isAlertProcessed(uint32_t id);

    // Get filename for alert based on date and alert ID
    String getAlertFilename(uint32_t id, const String &dateStr = "");

    // Extract date from filename (YYYYMMDD format)
    String extractDateFromFilename(const String &filename);

    // ===== AI Processing =====
    // Call AI to extract structured data from raw alert
    // Uses the source's buildAIPrompt() to get the prompt
    bool callAIForExtraction(AlertSource* source, const AlertSource::RawAlert &rawAlert,
                           String &outMessage, String &outStart, String &outEnd,
                           String &outWhere, uint8_t &outSeverity);

    // Provider-specific AI calls
    bool callGeminiAPI(const AIProvider &provider, const String &prompt,
                     String &outMessage, String &outStart, String &outEnd,
                     String &outWhere, uint8_t &outSeverity);
    bool callMistralAPI(const AIProvider &provider, const String &prompt,
                       String &outMessage, String &outStart, String &outEnd,
                       String &outWhere, uint8_t &outSeverity);
    bool callGroqAPI(const AIProvider &provider, const String &prompt,
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

    // ===== Utility Functions =====
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
};

#endif // HAS_ALERTING
