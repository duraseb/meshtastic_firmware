#pragma once

#include "../AlertSource.h"

/**
 * RCB (Rządowe Centrum Bezpieczeństwa) Alert Source
 * Scrapes alerts from www.gov.pl/web/rcb/komunikaty
 * 
 * Configuration:
 * - Prefix: [RCB]
 * - Default Severity: 3 (moderate importance)
 * - Fetch Interval: 5 minutes
 * - Format: HTML scraping with AI extraction
 */
class RCBAlertSource : public AlertSource {
public:
    RCBAlertSource();
    virtual ~RCBAlertSource() {}
    
    String getSourceId() const override { return "RCB"; }
    String getFetchUrl() const override;
    unsigned long getFetchIntervalMs() const override;
    uint8_t getDefaultSeverity() const override { return 3; }
    
    std::vector<RawAlert> fetchAndParseAlerts(
        std::function<String(const char*, int&)> httpGetCallback) override;
    
    RawAlert fetchFullAlertContent(const RawAlert& rawAlert,
        std::function<String(const char*, int&)> httpGetCallback) override;
    
    String buildAIPrompt(const RawAlert &rawAlert,
                        int maxMessageBytes,
                        int maxLocationChars) const override;
    
private:
    // HTML parsing helpers
    String extractLatestFromRSS(const String &payload, String &outId, 
                                String &outTitle, String &outLink, String &outContent);
    String extractTextBetween(const String &html, const char *startTag, 
                             const char *endTag, size_t startPos = 0);
    uint32_t hashString(const String &str);
};

