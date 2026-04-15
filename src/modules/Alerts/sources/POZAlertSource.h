#pragma once

#if defined(HAS_ALERTING) && HAS_ALERTING && !MESHTASTIC_EXCLUDE_ALERT_POZ

#include "configuration.h"
#include "../AlertSource.h"
#include <ArduinoJson.h>
#include <WiFiClient.h>

/**
 * POZ (Poznań Events) Alert Source
 * Fetches cultural events from Poznań city events API
 *
 * Configuration:
 * - Prefix: [POZ]
 * - Default Severity: 6 (informational/cultural events)
 * - Fetch Interval: 60 minutes (events don't change rapidly)
 * - Format: JSON API with event array
 */
class POZAlertSource : public AlertSource {
public:
    POZAlertSource() = default;
    virtual ~POZAlertSource() = default;

    String getSourceId() const override { return "POZ"; }
    String getFetchUrl() const override;
    unsigned long getFetchIntervalMs() const override;
    uint8_t getDefaultSeverity() const override { return 10; }
    String getChannelName() const override { return "PoznanEvent"; }
    String getInfoPrompt() override;

    std::vector<RawAlert> fetchAndParseAlerts(
        std::function<String(const char*, int&)> httpGetCallback) override;

    // Streaming JSON parser using ArduinoJson chunked deserialization
    // See: https://arduinojson.org/v7/how-to/deserialize-a-very-large-document/
    bool parsePOZStream(WiFiClient* stream, DynamicJsonDocument& doc, std::vector<RawAlert>& alerts);

    String getPreprocessedMessage(const RawAlert &rawAlert, int maxMessageBytes) const override;

    String buildAIPrompt(const RawAlert &rawAlert,
                        int maxMessageBytes,
                        int maxLocationChars) const override;

private:
    static constexpr unsigned long INFO_INITIAL_DELAY_MS = 60 * 60 * 1000;
    static constexpr unsigned long INFO_INTERVAL_MS = 12UL * 60 * 60 * 1000;
    uint32_t hashString(const String &str);
    unsigned long lastInfoBroadcast = 0;
};

#endif // HAS_ALERTING && !MESHTASTIC_EXCLUDE_ALERT_POZ
