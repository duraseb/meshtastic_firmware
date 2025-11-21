#if defined(HAS_ALERTING) && HAS_ALERTING

#include "RCBAlertModule.h"
#include "mesh/wifi/WiFiAPClient.h"
#include "mesh/http/ContentHandler.h"
#include "FSCommon.h"
#include "main.h"
#include "mesh/generated/meshtastic/mesh.pb.h"
#include "mesh/Router.h"
#include "mesh/MeshService.h"
#include "SPILock.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ctime>
#include <ctype.h>
#include <vector>
#include <sstream>

RCBAlertModule *rcbAlertModule = nullptr;

RCBAlertModule::RCBAlertModule() : OSThread("RCBAlertModule")
{
    // Defaults
    fetchUrl = "https://www.gov.pl/web/rcb/komunikaty"; // fallback - expected to be an HTML page
    aiEndpoint = "https://generativelanguage.googleapis.com/v1beta/models/gemini-1.5-flash:generateContent";
#ifdef GEMINI_API_KEY
    aiApiKey = GEMINI_API_KEY; // Defined in platformio_private.ini
#else
    aiApiKey = ""; // Not defined, will use heuristic extraction
#endif
    intervalMs = 5 * 60 * 1000; // 5 minutes
    lastSeenId = loadLastSeen();
    rcbAlertModule = this;

    // Load existing alerts from disk on startup
    loadAlertsFromDisk();
}

RCBAlertModule::~RCBAlertModule() {}

bool RCBAlertModule::loadConfig()
{
    // Minimal config: allow overriding fetch URL and interval via config.network (reuse existing config for simplicity)
    if (config.network.wifi_enabled) {
        // no-op for now
    }
    return true;
}

String RCBAlertModule::httpGet(const char *url, int &httpCode)
{
    String payload = "";
    if (!isWifiAvailable())
        return payload;

    HTTPClient http;
    WiFiClientSecure client;
    client.setInsecure();
    http.begin(client, url);
    http.setTimeout(10000);
    httpCode = http.GET();
    if (httpCode > 0) {
        if (httpCode == HTTP_CODE_OK) {
            payload = http.getString();
        }
    }
    http.end();
    return payload;
}

String RCBAlertModule::extractLatestFromRSS(const String &payload, String &outId, String &outTitle, String &outLink, String &outContent)
{
    // Very small and tolerant parser: try to find <item> or <entry> and extract guid/link/title/description
    int itemPos = payload.indexOf("<item>");
    if (itemPos < 0)
        itemPos = payload.indexOf("<entry>");
    if (itemPos < 0)
        return String();

    int itemEnd = payload.indexOf("</item>", itemPos);
    if (itemEnd < 0)
        itemEnd = payload.indexOf("</entry>", itemPos);
    if (itemEnd < 0)
        return String();

    String item = payload.substring(itemPos, itemEnd);

    auto findTag = [&](const char *tag) -> String {
        String open = String("<") + tag + ">";
        String close = String("</") + tag + ">";
        int s = item.indexOf(open);
        if (s < 0)
            return String();
        s += open.length();
        int e = item.indexOf(close, s);
        if (e < 0)
            return String();
        return item.substring(s, e);
    };

    outTitle = findTag("title");
    outLink = findTag("link");
    outContent = findTag("description");
    outId = findTag("guid");
    if (outId.length() == 0) {
        // fallback to link or title
        if (outLink.length())
            outId = outLink;
        else
            outId = outTitle;
    }

    return outTitle.length() ? outTitle : String();
}

// Try to extract the first alert entry from the Komunikaty HTML list.
// Looks for links containing "alert-rcb" and parses the anchor text to find a leading date like 18.11.2025.
String RCBAlertModule::extractLatestFromHTML(const String &payload, String &outId, String &outDate, String &outTitle)
{
    outId = "";
    outDate = "";
    outTitle = "";

    int pos = 0;
    while (true) {
        int idx = payload.indexOf("alert-rcb", pos);
        if (idx < 0)
            break;

        // find href="..." containing this occurrence
        int hrefPos = payload.lastIndexOf("href=\"", idx);
        if (hrefPos < 0) {
            pos = idx + 1;
            continue;
        }
        int hrefStart = payload.indexOf('"', hrefPos) + 1;
        int hrefEnd = payload.indexOf('"', hrefStart);
        if (hrefStart < 0 || hrefEnd < 0) {
            pos = idx + 1;
            continue;
        }
        String link = payload.substring(hrefStart, hrefEnd);
        if (link.startsWith("/"))
            link = String("https://www.gov.pl") + link;

        // find anchor text between > and </a>
        int tagStart = payload.indexOf('>', hrefEnd);
        int tagEnd = payload.indexOf("</a>", tagStart);
        String anchor = "";
        if (tagStart >= 0 && tagEnd > tagStart)
            anchor = payload.substring(tagStart + 1, tagEnd);

        // Try to find a date in the anchor text in format dd.mm.YYYY (10 chars)
        for (int i = 0; i + 9 < anchor.length(); ++i) {
            char c0 = anchor.charAt(i);
            if (!isdigit(c0))
                continue;
            bool ok = true;
            // dd.mm.yyyy -> positions: i,i+1 digits; i+2 '.'; i+3,i+4 digits; i+5 '.'; i+6..i+9 digits
            ok = ok && isdigit(anchor.charAt(i)) && isdigit(anchor.charAt(i + 1)) && anchor.charAt(i + 2) == '.' && isdigit(anchor.charAt(i + 3)) && isdigit(anchor.charAt(i + 4)) && anchor.charAt(i + 5) == '.' && isdigit(anchor.charAt(i + 6)) && isdigit(anchor.charAt(i + 7)) && isdigit(anchor.charAt(i + 8)) && isdigit(anchor.charAt(i + 9));
            if (ok) {
                String date = anchor.substring(i, i + 10);
                outDate = date;
                outTitle = anchor;
                outId = link;
                return outTitle;
            }
        }

        pos = idx + 1;
    }

    return String();
}

 void RCBAlertModule::parseAlertsFromHTML(const String &payload)
 {
     // Look for <a href> tags that contain alert cards
     // Each card has: <div class="event"><span class="date">...</span></div>
     //               <div class="title">...</div>
     //               <div class="intro">...</div>
     // All siblings within a wrapper div inside the <a> tag

     int pos = 0;
     std::vector<Alert> found;

     while (true) {
         // Find next <a href="/web/rcb/..." tag
         int aPos = payload.indexOf("<a href=\"/web/rcb/", pos);
         if (aPos < 0) break;

         // Extract href value
         int hrefStart = payload.indexOf('"', aPos) + 1;
         int hrefEnd = payload.indexOf('"', hrefStart);
         if (hrefEnd <= hrefStart) { pos = aPos + 1; continue; }

         String link = payload.substring(hrefStart, hrefEnd);
         if (!link.startsWith("/")) link = String("/web/rcb/") + link; // fallback
         link = String("https://www.gov.pl") + link;

         // Find the closing </a> tag
         int aEnd = payload.indexOf("</a>", aPos);
         if (aEnd < 0) { pos = aPos + 1; continue; }

         // Extract content between <a> and </a>
         String cardContent = payload.substring(aPos, aEnd);

         // Extract date from <span class="date">
         String dateStr = "";
         int dateSpanPos = cardContent.indexOf("<span class=\"date\">");
         if (dateSpanPos >= 0) {
             int dateStart = cardContent.indexOf('>', dateSpanPos) + 1;
             int dateEnd = cardContent.indexOf("</span>", dateStart);
             if (dateEnd > dateStart) {
                 dateStr = cardContent.substring(dateStart, dateEnd);
                 dateStr.trim();
             }
         }

         // Extract title from <div class="title">
         String title = "";
         int titleDivPos = cardContent.indexOf("<div class=\"title\">");
         if (titleDivPos >= 0) {
             int titleStart = cardContent.indexOf('>', titleDivPos) + 1;
             int titleEnd = cardContent.indexOf("</div>", titleStart);
             if (titleEnd > titleStart) {
                 title = cardContent.substring(titleStart, titleEnd);
                 title.trim();
             }
         }

         // Extract intro from <div class="intro">
         String intro = "";
         int introDivPos = cardContent.indexOf("<div class=\"intro\">");
         if (introDivPos >= 0) {
             int introStart = cardContent.indexOf('>', introDivPos) + 1;
             int introEnd = cardContent.indexOf("</div>", introStart);
             if (introEnd > introStart) {
                 intro = cardContent.substring(introStart, introEnd);
                 intro.trim();
             }
         }

         // Create alert only if we have link and title
         if (link.length() > 0 && title.length() > 0) {
             Alert a;
             a.id = link;
             a.link = link;
             a.title = title;
             a.valid_from = dateStr;  // publication date from page HTML
             a.valid_to = String();
             a.location = String();
             a.message = String();
             a.alert_type = String();
             a.severity = 0;
             a.addedAt = 0;
             a.lastSent = 0;

             // Extract critical info (location, alert_type, message, etc.) with intro text for AI extraction
             extractCriticalInfo(a, dateStr, intro);

             if (!alertExists(a.id)) found.push_back(a);
         }

         pos = aEnd + 4; // move past </a>
     }

     // Merge found alerts into the main alerts vector, avoiding duplicates
     for (auto &a : found) {
         if (!alertExists(a.id)) {
             alerts.push_back(a);
         }
     }
 }bool RCBAlertModule::alertExists(const String &id)
{
    for (auto &a : alerts) {
        if (a.id == id)
            return true;
    }
    return false;
}

bool RCBAlertModule::saveAlertsToDisk()
{
    concurrency::LockGuard g(spiLock);
    File f = FSCom.open("/rcb_alerts.json", FILE_O_WRITE);
    if (!f)
        return false;
    // Very small JSON writer
    f.print("[");
    bool first = true;
    for (auto &a : alerts) {
        if (!first) f.print(",\n");
        first = false;
        // escape minimal
        String title = a.title;
        title.replace("\"", "\\\"");
        String message = a.message;
        message.replace("\"", "\\\"");
        String location = a.location;
        location.replace("\"", "\\\"");
        f.print("{\"id\":\""); f.print(a.id);
        f.print("\",\"title\":\""); f.print(title);
        f.print("\",\"link\":\""); f.print(a.link);
        f.print("\",\"valid_from\":\""); f.print(a.valid_from);
        f.print("\",\"valid_to\":\""); f.print(a.valid_to);
        f.print("\",\"location\":\""); f.print(location);
        f.print("\",\"message\":\""); f.print(message);
        f.print("\",\"severity\":"); f.print(a.severity);
        f.print(",\"addedAt\":"); f.print(a.addedAt);
        f.print(",\"lastSent\":"); f.print(a.lastSent);
        f.print("}");
    }
    f.print("\n]");
    f.flush();
    f.close();
    return true;
}

bool RCBAlertModule::loadAlertsFromDisk()
{
    concurrency::LockGuard g(spiLock);
    File f = FSCom.open("/rcb_alerts.json", FILE_O_READ);
    if (!f)
        return false;
    String s = f.readString();
    f.close();
    // Very small JSON parser: find objects with all alert fields
    alerts.clear();
    int pos = 0;
    while (true) {
        int obj = s.indexOf('{', pos);
        if (obj < 0) break;
        int end = s.indexOf('}', obj);
        if (end < 0) break;
        String objStr = s.substring(obj+1, end);
        auto extractField = [&](const char *name)->String {
            String key = String("\"") + name + "\":\"";
            int k = objStr.indexOf(key);
            if (k < 0) {
                // Try numeric field
                key = String("\"") + name + "\":";
                k = objStr.indexOf(key);
                if (k < 0) return String();
                int vstart = k + key.length();
                // Skip whitespace
                while (vstart < objStr.length() && (objStr.charAt(vstart) == ' ' || objStr.charAt(vstart) == '\t')) {
                    vstart++;
                }
                int vend = vstart;
                while (vend < objStr.length() && objStr.charAt(vend) >= '0' && objStr.charAt(vend) <= '9') {
                    vend++;
                }
                return objStr.substring(vstart, vend);
            }
            int vstart = k + key.length();
            int vend = vstart;
            while (vend < objStr.length()) {
                if (objStr.charAt(vend) == '"' && objStr.charAt(vend - 1) != '\\') {
                    break;
                }
                vend++;
            }
            if (vend >= objStr.length()) return String();
            return objStr.substring(vstart, vend);
        };
        Alert a;
        a.id = extractField("id");
        a.title = extractField("title");
        a.link = extractField("link");
        a.valid_from = extractField("valid_from");
        a.valid_to = extractField("valid_to");
        a.location = extractField("location");
        a.message = extractField("message");
        String severityStr = extractField("severity");
        a.severity = severityStr.length() > 0 ? atoi(severityStr.c_str()) : 0;
        String addedAtStr = extractField("addedAt");
        a.addedAt = addedAtStr.length() > 0 ? strtoul(addedAtStr.c_str(), nullptr, 10) : millis();
        String lastSentStr = extractField("lastSent");
        a.lastSent = lastSentStr.length() > 0 ? strtoul(lastSentStr.c_str(), nullptr, 10) : 0;
        a.alert_type = a.title; // Restore from title

        // If message is empty, use title
        if (a.message.length() == 0) {
            a.message = a.title;
        }

        if (a.id.length()) alerts.push_back(a);
        pos = end + 1;
    }
    return true;
}

String RCBAlertModule::summarizeText(const String &text)
{
    // Placeholder: if aiEndpoint is configured, call it; otherwise return truncated text
    if (aiEndpoint.length() == 0)
        return text.substring(0, min((int)text.length(), 200));

    // Simple POST to aiEndpoint with API key header and 'text' field - assume JSON
    // Note: keep implementation simple to avoid heavy JSON libs
    int httpCode = 0;
    HTTPClient http;
    WiFiClientSecure client;
    client.setInsecure();
    http.begin(client, aiEndpoint.c_str());
    http.addHeader("Content-Type", "application/json");
    if (aiApiKey.length())
        http.addHeader("Authorization", String("Bearer ") + aiApiKey);

    String body = String("{\"text\":\"") + text + "\"}";
    httpCode = http.POST(body);
    String resp = "";
    if (httpCode > 0 && httpCode == HTTP_CODE_OK) {
        resp = http.getString();
    }
    http.end();
    if (resp.length())
        return resp.substring(0, min((int)resp.length(), 200));
    return text.substring(0, min((int)text.length(), 200));
}

String RCBAlertModule::shortenUrlIfNeeded(const String &url)
{
    // Use TinyURL API for simplicity
    if (url.length() == 0)
        return url;
    String api = String("https://tinyurl.com/api-create.php?url=") + url;
    int httpCode = 0;
    String resp = httpGet(api.c_str(), httpCode);
    if (httpCode == HTTP_CODE_OK && resp.length())
        return resp;
    return url;
}

bool RCBAlertModule::saveLastSeen(const String &id)
{
    concurrency::LockGuard g(spiLock);
    File f = FSCom.open("/rcb_last_seen.txt", FILE_O_WRITE);
    if (!f)
        return false;
    f.print(id);
    f.flush();
    f.close();
    lastSeenId = id;
    return true;
}

String RCBAlertModule::loadLastSeen()
{
    concurrency::LockGuard g(spiLock);
    File f = FSCom.open("/rcb_last_seen.txt", FILE_O_READ);
    if (!f)
        return String();
    String s = f.readString();
    f.close();
    return s;
}

int32_t RCBAlertModule::runOnce()
{
    if (!isWifiAvailable())
        return 60000; // check again in 60s

    unsigned long now = millis();
    bool alertsUpdated = false;

    // First, check existing alerts for periodic re-sending
    for (auto &alert : alerts) {
        if (!isAlertValid(alert)) {
            continue; // Skip invalid alerts
        }

        unsigned long interval = getSendInterval(alert.severity);
        if (alert.lastSent == 0 || (now - alert.lastSent) >= interval) {
            // Time to re-send this alert
            if (sendAlertToMesh(alert)) {
                alert.lastSent = now;
                alertsUpdated = true;
            }
        }
    }

    // Then, fetch and parse new alerts from the web
    int httpCode = 0;
    String payload = httpGet(fetchUrl.c_str(), httpCode);
    if (payload.length() == 0) {
        // If fetch failed, still return shorter interval to check for re-sends
        return 60000; // 1 minute
    }

    // Parse and save all alerts from the Komunikaty page
    size_t alertsBefore = alerts.size();
    parseAlertsFromHTML(payload);

    // Send newly discovered alerts immediately
    for (size_t i = alertsBefore; i < alerts.size(); i++) {
        Alert &alert = alerts[i];
        if (sendAlertToMesh(alert)) {
            alert.lastSent = now;
            alertsUpdated = true;
        }
    }

    // Save alerts to disk if updated
    if (alertsUpdated || alerts.size() != alertsBefore) {
        saveAlertsToDisk();
    }

    // Return interval for next check (use minimum of fetch interval and shortest alert interval)
    unsigned long minInterval = intervalMs;
    for (const auto &alert : alerts) {
        if (isAlertValid(alert)) {
            unsigned long alertInterval = getSendInterval(alert.severity);
            if (alert.lastSent > 0) {
                unsigned long timeUntilNext = alertInterval - (now - alert.lastSent);
                if (timeUntilNext < minInterval) {
                    minInterval = timeUntilNext;
                }
            } else {
                // New alert, should send soon
                minInterval = 10000; // 10 seconds
            }
        }
    }

    return minInterval > 0 ? minInterval : intervalMs;
}

bool RCBAlertModule::callAIForExtraction(const String &title, const String &intro, const String &pubDate, String &outMessage, String &outStart, String &outEnd, String &outWhere, uint8_t &outSeverity)
{
    // Try to extract using AI service if configured
    // Otherwise fall back to heuristic extraction

    if (aiEndpoint.length() == 0 || aiApiKey.length() == 0) {
        // AI endpoint not configured, use heuristic extraction
        outSeverity = 5; // Default to middle severity
        return extractInfoHeuristic(title, intro, pubDate, outMessage, outStart, outEnd, outWhere);
    }

    // Build extraction prompt using the optimized Polish prompt (exactly as provided by user)
    String prompt = "Jesteś parserem alertów. Twoim jedynym zadaniem jest zwrócić dokładnie ten JSON (nic więcej, zero dodatkowego tekstu):\n\n"
                    "{\"message\":\"...\",\"start\":\"YYYY-MM-DD hh:mm:ss\",\"end\":\"YYYY-MM-DD hh:mm:ss\",\"where\":\"...\",\"severity\":0}\n\n"
                    "Zasady (bardzo krótkie):\n\n"
                    "- \"message\":\n"
                    "  - jeśli cały oryginalny alert (TITLE + INTRO) ≤ 200 znaków → wklej go dosłownie\n"
                    "  - jeśli dłuższy → zrób zwięzłe streszczenie po polsku (max 200 znaków), zachowaj sens zagrożenia, lokalizację i zalecenia. Używaj maksymalnie 5 skrótów (np. gm., pow., woj.).\n\n"
                    "- \"where\": tylko nazwa geograficzna (max 30 znaków). Zawsze podaj powiat jeśli jest. Skracaj: gm., pow., woj.\n\n"
                    "- \"start\" i \"end\": format YYYY-MM-DD hh:mm:ss\n"
                    "  - szukaj dat/godzin w tekście\n"
                    "  - jeśli brak → użyj PUB_DATE jako obu dat\n"
                    "  - jeśli tylko data → start 00:00:00, end 23:59:59\n\n"
                    "- \"severity\": 0-10 gdzie 0=krytyczne (wojna, katastrofa na dużą skalę), 10=bardzo lokalne i mało ważne\n\n"
                    "- skup się głównie na INTRO, TITLE traktuj jako kontekst/dodatek\n\n"
                    "PUB_DATE: " + pubDate + "\n"
                    "TITLE: " + title + "\n"
                    "INTRO: " + intro;

    // Call AI endpoint with POST
    HTTPClient http;
    WiFiClientSecure client;
    client.setInsecure();

    // Add API key to URL as query parameter for Gemini API
    String url = aiEndpoint;
    if (aiApiKey.length() > 0) {
        url += "?key=" + aiApiKey;
    }
    http.begin(client, url.c_str());
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(15000); // 15 second timeout for AI service

    // Build Gemini API request format
    String body = "{\"contents\":[{\"parts\":[{\"text\":\"";

    // Escape prompt for JSON
    for (int i = 0; i < prompt.length(); i++) {
        char c = prompt.charAt(i);
        if (c == '"') {
            body += "\\\"";
        } else if (c == '\\') {
            body += "\\\\";
        } else if (c == '\n') {
            body += "\\n";
        } else if (c == '\r') {
            body += "\\r";
        } else {
            body += c;
        }
    }
    body += "\"}]}]}";

    int httpCode = http.POST(body);
    bool success = false;

    if (httpCode == HTTP_CODE_OK) {
        String response = http.getString();
        success = parseAIResponse(response, outMessage, outStart, outEnd, outWhere, outSeverity);
    }
    http.end();

    if (success) {
        return true;
    }

    // Fall back to heuristic extraction if AI fails
    outSeverity = 5; // Default to middle severity
    return extractInfoHeuristic(title, intro, pubDate, outMessage, outStart, outEnd, outWhere);
}

bool RCBAlertModule::parseAIResponse(const String &response, String &outMessage, String &outStart, String &outEnd, String &outWhere, uint8_t &outSeverity)
{
    // Gemini API response format: {"candidates":[{"content":{"parts":[{"text":"{\"message\":\"...\",\"start\":\"...\",\"end\":\"...\",\"where\":\"...\"}"}]}}]}
    // We need to extract the text field, then parse the inner JSON

    int textStart = response.indexOf("\"text\":\"");
    if (textStart < 0) {
        return false;
    }
    textStart += 8; // Move past "text":"

    // Find the end of the escaped JSON string by tracking escaped quotes
    int textEnd = textStart;
    bool escaped = false;
    while (textEnd < response.length()) {
        char c = response.charAt(textEnd);
        if (escaped) {
            escaped = false;
        } else if (c == '\\') {
            escaped = true;
        } else if (c == '"') {
            // Check if this is the closing quote (not escaped)
            // Look ahead to see if we're at the end of the text field
            int nextChar = textEnd + 1;
            while (nextChar < response.length() && (response.charAt(nextChar) == ' ' || response.charAt(nextChar) == '\t' || response.charAt(nextChar) == '\n' || response.charAt(nextChar) == '\r')) {
                nextChar++;
            }
            if (nextChar < response.length() && (response.charAt(nextChar) == '}' || response.charAt(nextChar) == ']' || response.charAt(nextChar) == ',')) {
                // This looks like the end of the text field
                break;
            }
        }
        textEnd++;
    }

    if (textEnd >= response.length()) return false;

    String innerJson = response.substring(textStart, textEnd);
    // Unescape inner JSON string
    innerJson.replace("\\\"", "\"");
    innerJson.replace("\\n", "\n");
    innerJson.replace("\\r", "\r");
    innerJson.replace("\\t", "\t");
    innerJson.replace("\\\\", "\\");

    // Now parse the inner JSON for message, start, end, where
    auto extractField = [&](const char *fieldName) -> String {
        String key = String("\"") + fieldName + "\"";
        int keyPos = innerJson.indexOf(key);
        if (keyPos < 0) return String();

        int colonPos = innerJson.indexOf(':', keyPos);
        if (colonPos < 0) return String();

        int valueStart = innerJson.indexOf('"', colonPos);
        if (valueStart < 0) return String();
        valueStart++;

        int valueEnd = valueStart;
        while (valueEnd < innerJson.length()) {
            if (innerJson.charAt(valueEnd) == '"' && innerJson.charAt(valueEnd - 1) != '\\') {
                break;
            }
            valueEnd++;
        }

        if (valueEnd >= innerJson.length()) return String();

        String value = innerJson.substring(valueStart, valueEnd);
        return value;
    };

    outMessage = extractField("message");
    outStart = extractField("start");
    outEnd = extractField("end");
    outWhere = extractField("where");

    // Extract severity (numeric field, not quoted)
    String severityKey = String("\"severity\":");
    int severityPos = innerJson.indexOf(severityKey);
    if (severityPos >= 0) {
        int valueStart = severityPos + severityKey.length();
        // Skip whitespace
        while (valueStart < innerJson.length() && (innerJson.charAt(valueStart) == ' ' || innerJson.charAt(valueStart) == '\t')) {
            valueStart++;
        }
        int valueEnd = valueStart;
        while (valueEnd < innerJson.length() && innerJson.charAt(valueEnd) >= '0' && innerJson.charAt(valueEnd) <= '9') {
            valueEnd++;
        }
        if (valueEnd > valueStart) {
            String severityStr = innerJson.substring(valueStart, valueEnd);
            int sev = atoi(severityStr.c_str());
            if (sev >= 0 && sev <= 10) {
                outSeverity = sev;
            } else {
                outSeverity = 5; // Default if out of range
            }
        } else {
            outSeverity = 5; // Default if not found
        }
    } else {
        outSeverity = 5; // Default if not found
    }

    return (outMessage.length() > 0 && outStart.length() > 0 && outEnd.length() > 0 && outWhere.length() > 0);
}

bool RCBAlertModule::extractInfoHeuristic(const String &title, const String &intro, const String &pubDateStr, String &outMessage, String &outStart, String &outEnd, String &outWhere)
{
    // Fallback heuristic extraction when AI is not available
    outWhere = "Poland";
    outStart = pubDateStr;
    outEnd = pubDateStr;
    outMessage = title;

    String fullText = title + " " + intro;

    // Extract location (powiat/region)
    // Look for "powiecie XXXX" or "powiat XXXX"
    int powPos = fullText.indexOf("pow");
    if (powPos >= 0) {
        int endPos = powPos + 8;
        while (endPos < fullText.length() && fullText.charAt(endPos) != ' ') {
            endPos++;
        }
        if (endPos < fullText.length()) {
            int locEnd = endPos + 1;
            while (locEnd < fullText.length() && fullText.charAt(locEnd) != ')' && fullText.charAt(locEnd) != '-' && fullText.charAt(locEnd) != '(') {
                locEnd++;
            }
            String location = fullText.substring(endPos + 1, locEnd);
            location.trim();
            if (location.length() > 0) {
                outWhere = String("powiat ") + location;
            }
        }
    }

    // Check for wojewodztwo (woj.)
    if (fullText.indexOf("woj.") >= 0) {
        int wojPos = fullText.indexOf("woj.");
        int locEnd = wojPos + 5;
        while (locEnd < fullText.length() && fullText.charAt(locEnd) != ')' && fullText.charAt(locEnd) != '-' && fullText.charAt(locEnd) != '(') {
            locEnd++;
        }
        String location = fullText.substring(wojPos + 4, locEnd);
        location.trim();
        if (location.length() > 0) {
            outWhere = String("woj. ") + location;
        }
    }

    // Extract message: remove "Alert RCB - " prefix and trailing date, combine with intro
    outMessage = title;
    outMessage.replace("Alert RCB - ", "");

    // Remove trailing date patterns like "(19.11)"
    int parenPos = outMessage.lastIndexOf('(');
    if (parenPos >= 0) {
        String trailing = outMessage.substring(parenPos);
        if (trailing.indexOf('.') >= 0 && trailing.indexOf(')') > trailing.indexOf('.')) {
            outMessage = outMessage.substring(0, parenPos);
            outMessage.trim();
        }
    }

    // Add intro if available and message is short
    if (intro.length() > 0 && outMessage.length() < 150) {
        outMessage += " - " + intro;
        // Truncate to reasonable length
        if (outMessage.length() > 200) {
            outMessage = outMessage.substring(0, 197) + "...";
        }
    }

    return true;
}

void RCBAlertModule::extractCriticalInfo(Alert &alert, const String &pubDateStr, const String &intro)
{
    // Extract location, date_from, date_to, message, and alert_type from alert title and intro using AI or heuristics
    String message, start, end, where;
    uint8_t aiSeverity = 5; // Default

    // Determine base severity from source/content
    uint8_t baseSeverity = determineBaseSeverity(alert.title, intro);

    // Try AI extraction, fall back to heuristics if not available
    bool aiSuccess = callAIForExtraction(alert.title, intro, pubDateStr, message, start, end, where, aiSeverity);

    // Combine base severity with AI-extracted severity
    alert.severity = combineSeverity(baseSeverity, aiSeverity);

    alert.location = where;
    alert.alert_type = alert.title; // Keep original title as alert type
    alert.message = message.length() > 0 ? message : alert.title; // Use extracted message or fallback to title

    // Ensure message doesn't exceed 200 bytes (we'll add geolocation later)
    const int maxMessageBytes = 200;
    if (utf8ByteLength(alert.message) > maxMessageBytes) {
        // Truncate to fit
        int truncatePos = 0;
        size_t byteCount = 0;
        while (truncatePos < alert.message.length() && byteCount < maxMessageBytes - 3) {
            char c = alert.message.charAt(truncatePos);
            if ((c & 0x80) == 0) {
                byteCount++; // ASCII
            } else if ((c & 0xE0) == 0xC0) {
                byteCount += 2; // 2-byte UTF-8
            } else if ((c & 0xF0) == 0xE0) {
                byteCount += 3; // 3-byte UTF-8
            } else if ((c & 0xF8) == 0xF0) {
                byteCount += 4; // 4-byte UTF-8
            } else {
                byteCount++; // Invalid, treat as single byte
            }
            if (byteCount <= maxMessageBytes - 3) {
                truncatePos++;
            } else {
                break;
            }
        }
        alert.message = alert.message.substring(0, truncatePos) + "...";
    }

    alert.valid_from = start.length() > 0 ? start : pubDateStr;
    alert.valid_to = end.length() > 0 ? end : pubDateStr;
    alert.addedAt = millis();
    alert.lastSent = 0; // Never sent yet
}

uint8_t RCBAlertModule::determineBaseSeverity(const String &title, const String &intro)
{
    String combined = title + " " + intro;
    combined.toLowerCase();

    // Critical (0): war, large-scale disasters
    if (combined.indexOf("wojna") >= 0 || combined.indexOf("katastrofa") >= 0 ||
        combined.indexOf("trzęsienie") >= 0 || combined.indexOf("powódź") >= 0) {
        return 0; // Critical
    }

    // Very high (1-2): life-threatening situations
    if (combined.indexOf("woda niezdatna") >= 0 || combined.indexOf("woda niezdatna do spożycia") >= 0 ||
        combined.indexOf("woda niezdatna do picia") >= 0) {
        return 1; // Very high
    }

    // High (3-4): severe weather, health hazards
    if (combined.indexOf("intensywne opady") >= 0 || combined.indexOf("burza") >= 0 ||
        combined.indexOf("smog") >= 0 || combined.indexOf("zła jakość powietrza") >= 0 ||
        combined.indexOf("pm10") >= 0) {
        return 3; // High
    }

    // Medium (5-6): warnings, exercises
    if (combined.indexOf("ćwiczenie") >= 0 || combined.indexOf("ostrzeżenie") >= 0) {
        return 6; // Medium
    }

    // Low (7-8): vaccinations, informational
    if (combined.indexOf("szczepienie") >= 0) {
        return 8; // Low
    }

    // Very low (9-10): very local, not very important
    return 9; // Default to low importance
}

uint8_t RCBAlertModule::combineSeverity(uint8_t baseSeverity, uint8_t aiSeverity)
{
    // Combine base severity (from source/content analysis) with AI-extracted severity
    // Use weighted average: 60% base, 40% AI (rounded)
    // This allows AI to adjust but keeps source analysis as primary
    uint16_t combined = (baseSeverity * 6 + aiSeverity * 4) / 10;
    if (combined > 10) combined = 10;
    return (uint8_t)combined;
}

unsigned long RCBAlertModule::getSendInterval(uint8_t severity)
{
    // Severity 0 = 30 minutes, severity 10 = 4 hours
    // Linear interpolation: interval = 30min + (severity * (4h - 30min) / 10)
    // 4 hours = 240 minutes, 30 minutes = 30 minutes
    // interval = 30 + (severity * 21) minutes
    unsigned long baseIntervalMs = 30 * 60 * 1000; // 30 minutes
    unsigned long maxIntervalMs = 4 * 60 * 60 * 1000; // 4 hours
    unsigned long rangeMs = maxIntervalMs - baseIntervalMs;

    // Calculate proportional interval
    unsigned long intervalMs = baseIntervalMs + (severity * rangeMs / 10);

    return intervalMs;
}

bool RCBAlertModule::isAlertValid(const Alert &alert)
{
    time_t now = time(nullptr);
    time_t validFrom = parseDateString(alert.valid_from);
    time_t validTo = parseDateString(alert.valid_to);

    // If parsing failed, assume alert is valid if valid_to is empty or in the future
    if (validFrom == 0 && validTo == 0) {
        // Try to parse valid_to, if it's empty assume it's valid
        if (alert.valid_to.length() == 0) {
            return true; // No end date, assume valid
        }
        return true; // Can't parse, assume valid for safety
    }

    return (now >= validFrom && now <= validTo);
}

bool RCBAlertModule::sendAlertToMesh(const Alert &alert)
{
    if (!isWifiAvailable()) {
        return false;
    }

    // Prepare message with geolocation suffix
    // Message body is limited to 200 bytes, then we add location
    String msg = alert.message;

    // Add location if available (format: " [location]")
    if (alert.location.length() > 0) {
        String locationSuffix = " [" + alert.location + "]";
        size_t msgBytes = utf8ByteLength(msg);
        size_t suffixBytes = utf8ByteLength(locationSuffix);
        const int maxPayload = meshtastic_Constants_DATA_PAYLOAD_LEN;

        // Check if we can fit the location suffix
        if (msgBytes + suffixBytes <= maxPayload) {
            msg += locationSuffix;
        } else {
            // Truncate message to make room for location
            int availableBytes = maxPayload - suffixBytes - 3; // -3 for "..."
            if (availableBytes > 0) {
                // Truncate message to fit
                int truncatePos = 0;
                size_t byteCount = 0;
                while (truncatePos < msg.length() && byteCount < availableBytes) {
                    char c = msg.charAt(truncatePos);
                    if ((c & 0x80) == 0) {
                        byteCount++; // ASCII
                    } else if ((c & 0xE0) == 0xC0) {
                        byteCount += 2; // 2-byte UTF-8
                    } else if ((c & 0xF0) == 0xE0) {
                        byteCount += 3; // 3-byte UTF-8
                    } else if ((c & 0xF8) == 0xF0) {
                        byteCount += 4; // 4-byte UTF-8
                    } else {
                        byteCount++; // Invalid, treat as single byte
                    }
                    if (byteCount <= availableBytes) {
                        truncatePos++;
                    } else {
                        break;
                    }
                }
                msg = msg.substring(0, truncatePos) + "..." + locationSuffix;
            } else {
                // Can't fit location, just use truncated message
                msg = msg.substring(0, maxPayload - 3) + "...";
            }
        }
    }

    // Final check: ensure total message fits in payload
    const int maxPayload = meshtastic_Constants_DATA_PAYLOAD_LEN;
    size_t msgBytes = utf8ByteLength(msg);
    if (msgBytes > maxPayload) {
        // Truncate to fit, being careful with UTF-8
        int truncatePos = 0;
        size_t byteCount = 0;
        while (truncatePos < msg.length() && byteCount < maxPayload - 3) {
            char c = msg.charAt(truncatePos);
            if ((c & 0x80) == 0) {
                byteCount++; // ASCII
            } else if ((c & 0xE0) == 0xC0) {
                byteCount += 2; // 2-byte UTF-8
            } else if ((c & 0xF0) == 0xE0) {
                byteCount += 3; // 3-byte UTF-8
            } else if ((c & 0xF8) == 0xF0) {
                byteCount += 4; // 4-byte UTF-8
            } else {
                byteCount++; // Invalid, treat as single byte
            }
            if (byteCount <= maxPayload - 3) {
                truncatePos++;
            } else {
                break;
            }
        }
        msg = msg.substring(0, truncatePos) + "...";
    }

    // Publish message to mesh: use broadcast
    meshtastic_MeshPacket *p = router->allocForSending();
    p->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
    p->to = 0xffffffff;
    p->channel = 0;
    p->want_ack = true;
    
    // Set priority based on severity (0-2 = HIGH, 3-10 = RELIABLE)
    if (alert.severity <= 2) {
        p->priority = meshtastic_MeshPacket_Priority_HIGH;
    } else {
        p->priority = meshtastic_MeshPacket_Priority_RELIABLE;
    }
    
    p->decoded.payload.size = utf8ByteLength(msg);
    memcpy(p->decoded.payload.bytes, msg.c_str(), p->decoded.payload.size);
    service->sendToMesh(p, RX_SRC_LOCAL, true);

    return true;
}

size_t RCBAlertModule::utf8ByteLength(const String &str)
{
    // String.length() in Arduino returns byte length, which is what we need for UTF-8
    return str.length();
}

time_t RCBAlertModule::parseDateString(const String &dateStr)
{
    if (dateStr.length() == 0) {
        return 0;
    }

    struct tm tm = {0};
    int day, month, year, hour = 0, minute = 0, second = 0;

    // Try YYYY-MM-DD hh:mm:ss format first (from AI)
    if (sscanf(dateStr.c_str(), "%d-%d-%d %d:%d:%d", &year, &month, &day, &hour, &minute, &second) >= 3) {
        tm.tm_mday = day;
        tm.tm_mon = month - 1; // tm_mon is 0-11
        tm.tm_year = year - 1900; // tm_year is years since 1900
        tm.tm_hour = hour;
        tm.tm_min = minute;
        tm.tm_sec = second;
        tm.tm_isdst = -1; // Let system determine DST

        return mktime(&tm);
    }

    // Try DD.MM.YYYY or DD.MM.YYYY HH:MM format (from HTML parsing)
    if (sscanf(dateStr.c_str(), "%d.%d.%d %d:%d", &day, &month, &year, &hour, &minute) >= 3 ||
        sscanf(dateStr.c_str(), "%d.%d.%d", &day, &month, &year) >= 3) {
        tm.tm_mday = day;
        tm.tm_mon = month - 1; // tm_mon is 0-11
        tm.tm_year = year - 1900; // tm_year is years since 1900
        tm.tm_hour = hour;
        tm.tm_min = minute;
        tm.tm_sec = 0;
        tm.tm_isdst = -1; // Let system determine DST

        return mktime(&tm);
    }

    return 0; // Parsing failed
}

#endif // HAS_ALERTING
