#pragma once

#if defined(HAS_ALERTING) && HAS_ALERTING

#include <Arduino.h>
#include <vector>

// Forward declaration - full definition in AlertsModule.h
struct Alert;

/**
 * Base class for alert sources.
 * Each source (RCB, IMGW, etc.) implements this interface to provide:
 * - Source-specific fetching and parsing logic
 * - Custom AI prompts for information extraction
 * - Source configuration (prefix, URLs, intervals, severity)
 */
class AlertSource {
public:
    virtual ~AlertSource() {}
    
    /**
     * Get the source identifier (e.g., "RCB", "IMGW")
     * Used as message prefix: [RCB], [IMGW], etc.
     */
    virtual String getSourceId() const = 0;
    
    /**
     * Get the URL to fetch alerts from
     */
    virtual String getFetchUrl() const = 0;
    
    /**
     * Get the fetch interval in milliseconds
     * How often to check for new alerts from this source
     */
    virtual unsigned long getFetchIntervalMs() const = 0;
    
    /**
     * Get the default severity for alerts from this source (0-10)
     * Used if severity cannot be determined from the alert data
     */
    virtual uint8_t getDefaultSeverity() const = 0;
    
    /**
     * Returns the channel name this source's alerts should be sent to.
     * Override to use a dedicated channel. Default returns empty string,
     * meaning the global alert channel (alertChannelName) will be used.
     */
    virtual String getChannelName() const { return ""; }
    
    /**
     * Returns an info/notification string to broadcast on the primary channel.
     * Called approximately every 60 seconds by the main loop.
     * The provider should track its own cadence internally and return
     * an empty string when it has nothing to broadcast.
     * Non-empty return value will be sent once to the primary channel.
     */
    virtual String getInfoPrompt() { return ""; }
    
    /**
     * Fetch and parse alerts from the source.
     * This is source-specific: RCB scrapes HTML, IMGW parses JSON, etc.
     * 
     * @param httpGetCallback Callback to perform HTTP GET requests
     * @return Vector of raw alert data ready for AI processing
     */
    struct RawAlert {
        uint32_t id;          // Unique identifier hash for duplicate detection (URL, UUID, RSS GUID, etc.)
        String link;          // URL or identifier string (source-specific)
        String title;         // Alert title
        String dateStr;       // Publication date string
        String intro;         // Main alert content/description
        String context;       // Additional context (optional)
        
        // Optional: Structured dates from source (if available)
        // If both are set, orchestrator will use them directly without asking AI
        String structuredStartDate;  // Format: "YYYY-MM-DD HH:MM:SS" (empty if not available)
        String structuredEndDate;    // Format: "YYYY-MM-DD HH:MM:SS" (empty if not available)
    };
    virtual std::vector<RawAlert> fetchAndParseAlerts(
        std::function<String(const char*, int&)> httpGetCallback) = 0;
    
    /**
     * Fetch full content for a specific alert (optional - used by sources that need two-phase fetching).
     * For sources like RCB that list alerts on one page but need to fetch individual pages,
     * this method is called only for new alerts after duplicate checking.
     * 
     * @param rawAlert The alert stub with at least link and id populated
     * @param httpGetCallback Callback for HTTP GET requests
     * @return Updated RawAlert with full content, or empty intro if fetch failed
     */
    virtual RawAlert fetchFullAlertContent(const RawAlert& rawAlert,
        std::function<String(const char*, int&)> httpGetCallback) {
        // Default implementation: return as-is (for sources that fetch everything in first pass)
        return rawAlert;
    }
    
    /**
     * Get pre-processed message for alerts that don't need AI processing.
     * If this returns a non-empty string, it will be used directly as the message.
     * If empty, the normal AI processing flow will be used.
     *
     * @param rawAlert The raw alert data
     * @param maxMessageBytes Maximum bytes for the message field
     * @return Pre-processed message string, or empty string to use AI processing
     */
    virtual String getPreprocessedMessage(const RawAlert &rawAlert, int maxMessageBytes) const {
        return ""; // Default: use AI processing
    }

    /**
     * Build AI prompt for extracting structured information from this alert.
     * Each source can customize the prompt based on its specific needs.
     *
     * @param rawAlert The raw alert data
     * @param maxMessageBytes Maximum bytes for the message field
     * @param maxLocationChars Maximum characters for the location field
     * @return The complete prompt string for the AI
     */
    virtual String buildAIPrompt(const RawAlert &rawAlert,
                                  int maxMessageBytes,
                                  int maxLocationChars) const = 0;
    
    /**
     * Optional: Perform source-specific cleanup or validation on extracted data
     * Called after AI extraction, before saving the alert.
     * Default implementation does nothing.
     * 
     * @param alert The alert with AI-extracted data
     * @return true if alert is valid and should be saved, false to skip
     */
    virtual bool validateAndCleanup(Alert &alert) {
        return true; // Default: accept all alerts
    }
};

#endif // HAS_ALERTING
