#pragma once

#if defined(HAS_ALERTING) && HAS_ALERTING

#include "concurrency/Periodic.h"
#include "configuration.h"
#include <Arduino.h>

class RCBAlertModule : public concurrency::OSThread {
  public:
    RCBAlertModule();
    virtual ~RCBAlertModule();
    int32_t runOnce() override;

  private:
    String fetchUrl;
    String aiEndpoint;
    String aiApiKey;
    unsigned long intervalMs;
    String lastSeenId;
  // In-memory cache of fetched alerts
  struct Alert {
    String id;
    String title;
    String link;
    String valid_from; // ISO-ish or original string
    String valid_to;
    String location;   // Extracted powiat/region
    String alert_type; // Short alert summary
  };

  // Stored alerts
  std::vector<Alert> alerts;

    bool loadConfig();
    String httpGet(const char *url, int &httpCode);
    String extractLatestFromRSS(const String &payload, String &outId, String &outTitle, String &outLink, String &outContent);
    String summarizeText(const String &text);
    String shortenUrlIfNeeded(const String &url);
    bool saveLastSeen(const String &id);
    String loadLastSeen();
  // Parse all alerts from Komunikaty HTML
  void parseAlertsFromHTML(const String &payload);
  // Persist alerts to disk and load them
  bool saveAlertsToDisk();
  bool loadAlertsFromDisk();
  // Helper to check duplicate
  bool alertExists(const String &id);

  // Extract critical info from alert (location, date_from, date_to, alert_type)
  void extractCriticalInfo(Alert &alert, const String &pubDateStr, const String &intro = "");

  // AI-based extraction with heuristic fallback
  bool callAIForExtraction(const String &title, const String &intro, const String &pubDate,
                           String &outWhat, String &outWhen, String &outWhere);

  // Parse JSON response from AI service
  bool parseAIResponse(const String &response, String &outWhat, String &outWhen, String &outWhere);

  // Heuristic extraction fallback when AI is not available
  bool extractInfoHeuristic(const String &title, const String &intro, const String &pubDateStr,
                            String &outWhat, String &outWhen, String &outWhere);
};

#endif // HAS_ALERTING
