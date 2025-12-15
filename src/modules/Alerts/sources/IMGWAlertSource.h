#pragma once

#include "../AlertSource.h"
#include <ArduinoJson.h>
#include <WiFiClient.h>

/**
 * IMGW (Instytut Meteorologii i Gospodarki Wodnej) Alert Source
 * Fetches weather alerts from MeteoAlarm API (feeds.meteoalarm.org)
 * 
 * Configuration:
 * - Prefix: [IMGW]
 * - Default Severity: 5 (moderate)
 * - Fetch Interval: 15 minutes (weather changes less frequently than RCB)
 * - Format: JSON API with CAP-compliant alert format
 */
class IMGWAlertSource : public AlertSource {
public:
    IMGWAlertSource();
    virtual ~IMGWAlertSource() {
        freePsramBuffers();
    }
    
    String getSourceId() const override { return "IMGW"; }
    String getFetchUrl() const override;
    unsigned long getFetchIntervalMs() const override;
    uint8_t getDefaultSeverity() const override { return 5; }
    
    std::vector<RawAlert> fetchAndParseAlerts(
        std::function<String(const char*, int&)> httpGetCallback) override;

    // Streaming JSON parser
    bool parseIMGWStream(WiFiClient* stream, std::vector<RawAlert>& alerts);

    // Process already parsed JSON document
    template<typename T>
    bool processParsedJson(T &doc, std::vector<RawAlert> &alerts);

    String buildAIPrompt(const RawAlert &rawAlert,
                        int maxMessageBytes,
                        int maxLocationChars) const override;
    
private:
    // PSRAM buffers for large allocations
    static const size_t CHUNK_SIZE = 4096;  // 4KB chunks
    static const size_t JSON_BUFFER_SIZE = 8192;  // 8KB for partial JSON buffering

    char* chunkBuffer;      // PSRAM allocated chunk buffer
    char* jsonBuffer;       // PSRAM allocated JSON buffer
    size_t jsonBufferPos;   // Position in JSON buffer

    void allocatePsramBuffers();
    void freePsramBuffers();

    uint8_t calculateSeverity(const String &severity, const String &certainty, const String &urgency) const;
    String convertISO8601ToMeshtastic(const String &iso8601) const;
    uint32_t hashString(const String &str);
};

