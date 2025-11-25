#pragma once

#include "../AlertSource.h"

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
    virtual ~IMGWAlertSource() {}
    
    String getSourceId() const override { return "IMGW"; }
    String getFetchUrl() const override;
    unsigned long getFetchIntervalMs() const override;
    uint8_t getDefaultSeverity() const override { return 5; }
    
    std::vector<RawAlert> fetchAndParseAlerts(
        std::function<String(const char*, int&)> httpGetCallback) override;
    
    String buildAIPrompt(const RawAlert &rawAlert,
                        int maxMessageBytes,
                        int maxLocationChars) const override;
    
private:
    // JSON parsing helpers
    String extractJsonString(const String &json, const char *key, size_t startPos = 0);
    String extractJsonArrayFirstString(const String &json, const char *arrayKey, const char *itemKey);
    uint8_t calculateSeverity(const String &severity, const String &certainty, const String &urgency) const;
    String convertISO8601ToMeshtastic(const String &iso8601) const;
    uint32_t hashString(const String &str);
    
    // Simple JSON extraction (we avoid full JSON library to save memory)
    String findJsonValue(const String &json, const char *key, size_t startPos = 0);
};

