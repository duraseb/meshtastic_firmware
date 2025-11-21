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
    String valid_from; // YYYY-MM-DD hh:mm:ss format
    String valid_to;   // YYYY-MM-DD hh:mm:ss format
    String location;   // Extracted powiat/region
    String alert_type; // Short alert summary
    String message;    // Processed message for sending (max 200 bytes)
    uint8_t severity; // 0=critical (war, large disaster) to 10=very local/unimportant
    unsigned long lastSent; // Last time this alert was sent (millis)
    unsigned long addedAt;  // When this alert was first added (millis)
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
  // Returns severity in outSeverity (0-10, where 0=critical, 10=least important)
  bool callAIForExtraction(const String &title, const String &intro, const String &pubDate,
                           String &outMessage, String &outStart, String &outEnd, String &outWhere, uint8_t &outSeverity);

  // Parse JSON response from AI service
  bool parseAIResponse(const String &response, String &outMessage, String &outStart, String &outEnd, String &outWhere, uint8_t &outSeverity);

  // Heuristic extraction fallback when AI is not available
  // Note: severity is not extracted in heuristic mode, will use base severity only
  bool extractInfoHeuristic(const String &title, const String &intro, const String &pubDateStr,
                            String &outMessage, String &outStart, String &outEnd, String &outWhere);

  // Determine base severity from source/content (0-10, where 0=critical, 10=least important)
  uint8_t determineBaseSeverity(const String &title, const String &intro);

  // Combine base severity with AI-extracted severity (if available)
  uint8_t combineSeverity(uint8_t baseSeverity, uint8_t aiSeverity);

  // Get send interval based on severity (in milliseconds)
  // Severity 0 = 30 minutes, severity 10 = 4 hours, proportional in between
  unsigned long getSendInterval(uint8_t severity);

  // Check if alert is still valid (within valid_from and valid_to dates)
  bool isAlertValid(const Alert &alert);

  // Send alert to mesh network
  bool sendAlertToMesh(const Alert &alert);

  // Calculate UTF-8 byte length of a string
  size_t utf8ByteLength(const String &str);

  // Parse date string (YYYY-MM-DD hh:mm:ss or DD.MM.YYYY formats) to time_t
  time_t parseDateString(const String &dateStr);
};

#endif // HAS_ALERTING
