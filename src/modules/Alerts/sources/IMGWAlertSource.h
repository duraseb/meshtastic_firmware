#pragma once

#if defined(HAS_ALERTING) && HAS_ALERTING

#include "configuration.h"
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
    IMGWAlertSource() = default;
    virtual ~IMGWAlertSource() = default;
    
    String getSourceId() const override { return "IMGW"; }
    String getFetchUrl() const override;
    unsigned long getFetchIntervalMs() const override;
    uint8_t getDefaultSeverity() const override { return 5; }
    
    std::vector<RawAlert> fetchAndParseAlerts(
        std::function<String(const char*, int&)> httpGetCallback) override;

    // Streaming JSON parser using ArduinoJson chunked deserialization
    // See: https://arduinojson.org/v7/how-to/deserialize-a-very-large-document/
    bool parseIMGWStream(WiFiClient* stream, DynamicJsonDocument& doc, std::vector<RawAlert>& alerts);

    String buildAIPrompt(const RawAlert &rawAlert,
                        int maxMessageBytes,
                        int maxLocationChars) const override;
    
private:
    // Temporary structure for deduplication
    struct TempAlert {
        RawAlert alert;
        uint64_t messageHash;
        size_t messageLength;
    };

    void deduplicateAlerts(std::vector<TempAlert>& rawAlerts, std::vector<RawAlert>& outputAlerts);
    uint8_t calculateSeverity(const String &severity, const String &certainty, const String &urgency) const;
    String convertISO8601ToMeshtastic(const String &iso8601) const;
    uint32_t hashString(const String &str);
};

#endif // HAS_ALERTING
