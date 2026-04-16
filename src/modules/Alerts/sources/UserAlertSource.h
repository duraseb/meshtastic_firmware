#pragma once

#if defined(HAS_ALERTING) && HAS_ALERTING && !MESHTASTIC_EXCLUDE_ALERT_INTERACTIVE

#include "../AlertSource.h"

/**
 * Minimal AlertSource for user-created alerts.
 * Alerts are injected via AlertsModule::addExternalAlert() by the AlertManager,
 * not fetched from an HTTP endpoint. This source exists to provide identity
 * (source ID, default severity) for the AlertsModule pipeline.
 */
class UserAlertSource : public AlertSource {
public:
    String getSourceId() const override { return "USR"; }
    String getFetchUrl() const override { return ""; }
    unsigned long getFetchIntervalMs() const override { return ULONG_MAX; }
    uint8_t getDefaultSeverity() const override { return 5; }

    std::vector<RawAlert> fetchAndParseAlerts(
        std::function<String(const char *, int &)> httpGetCallback) override
    {
        return {}; // Never fetches — alerts are injected directly
    }

    String buildAIPrompt(const RawAlert &rawAlert,
                         int maxMessageBytes,
                         int maxLocationChars) const override
    {
        return ""; // Never called — user provides all fields
    }
};

#endif // HAS_ALERTING && !MESHTASTIC_EXCLUDE_ALERT_INTERACTIVE
