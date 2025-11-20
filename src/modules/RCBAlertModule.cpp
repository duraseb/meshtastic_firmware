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
    aiApiKey = GEMINI_API_KEY; // Defined in platformio_private.ini
    intervalMs = 5 * 60 * 1000; // 5 minutes
    lastSeenId = loadLastSeen();
    rcbAlertModule = this;
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

             // Extract critical info (location, alert_type, etc.) with intro text for AI extraction
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
        f.print("{\"id\":\""); f.print(a.id); f.print("\",\"title\":\""); f.print(title); f.print("\",\"link\":\""); f.print(a.link); f.print("\",\"valid_from\":\""); f.print(a.valid_from); f.print("\",\"valid_to\":\""); f.print(a.valid_to); f.print("\"}");
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
    // Very small JSON parser: find objects with id/title/link/valid_from/valid_to
    alerts.clear();
    int pos = 0;
    while (true) {
        int obj = s.indexOf('{', pos);
        if (obj < 0) break;
        int end = s.indexOf('}', obj);
        if (end < 0) break;
        String objStr = s.substring(obj+1, end);
        auto extractField = [&](const char *name)->String {
            String key = String("\"") + name + "\"\s*:\s*\"";
            int k = objStr.indexOf(key);
            if (k < 0) return String();
            int vstart = k + key.length();
            int vend = objStr.indexOf('"', vstart);
            if (vend < 0) return String();
            return objStr.substring(vstart, vend);
        };
        Alert a;
        a.id = extractField("id");
        a.title = extractField("title");
        a.link = extractField("link");
        a.valid_from = extractField("valid_from");
        a.valid_to = extractField("valid_to");
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

    int httpCode = 0;
    String payload = httpGet(fetchUrl.c_str(), httpCode);
    if (payload.length() == 0)
        return intervalMs;
    // Parse and save all alerts from the Komunikaty page
    parseAlertsFromHTML(payload);
    saveAlertsToDisk();

    String id, title, link, content;
    String titleFound = extractLatestFromRSS(payload, id, title, link, content);
    String dateStr;
    String htmlFound;
    if (titleFound.length() == 0) {
        // Could not parse RSS/Atom - try HTML scraping for alert links (alert-rcb entries)
        htmlFound = extractLatestFromHTML(payload, id, dateStr, title);
    }

    if (id.length() == 0)
        return intervalMs;

    // If we parsed HTML, dateStr may contain the date from the anchor. Check whether it matches today's date.
    if (dateStr.length()) {
        time_t now = time(nullptr);
        struct tm *lt = localtime(&now);
        char today[16] = {0};
        strftime(today, sizeof(today), "%d.%m.%Y", lt);
        String todayStr = String(today);
        if (dateStr != todayStr) {
            // Not valid for today
            return intervalMs;
        }
    }

    if (id == lastSeenId) {
        // nothing new
        return intervalMs;
    }

    // Summarize (use title/description if available) and shorten link
    String core = title;
    if (content.length()) {
        core += String(" - ") + content;
    }
    // Trim whitespace
    while (core.length() && isspace(core.charAt(core.length() - 1)))
        core.remove(core.length() - 1);

    String shortLink = shortenUrlIfNeeded(link);

    // Compose message ensuring we don't exceed data payload length from mesh constants
    const int maxPayload = meshtastic_Constants_DATA_PAYLOAD_LEN;
    String msg;
    if (shortLink.length()) {
        int need = shortLink.length() + 1; // space
        if (core.length() + need <= maxPayload) {
            msg = core + " " + shortLink;
        } else {
            int allowedCore = maxPayload - need;
            if (allowedCore <= 0) {
                msg = shortLink.substring(0, maxPayload);
            } else {
                msg = core.substring(0, allowedCore);
                // trim trailing space
                while (msg.length() && isspace(msg.charAt(msg.length() - 1)))
                    msg.remove(msg.length() - 1);
                if (msg.length() + 1 + shortLink.length() <= maxPayload)
                    msg = msg + " " + shortLink;
                else {
                    // fallback: just include truncated core
                    msg = core.substring(0, maxPayload);
                }
            }
        }
    } else {
        if (core.length() > maxPayload)
            msg = core.substring(0, maxPayload);
        else
            msg = core;
    }

    // Publish message to mesh: use broadcast. Allocate via router to match other modules.
    meshtastic_MeshPacket *p = router->allocForSending();
    p->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
    p->to = 0xffffffff;
    p->channel = 0;
    p->want_ack = true;
    p->decoded.payload.size = msg.length();
    memcpy(p->decoded.payload.bytes, msg.c_str(), p->decoded.payload.size);
    service->sendToMesh(p, RX_SRC_LOCAL, true);

    saveLastSeen(id);

    return intervalMs;
}

bool RCBAlertModule::callAIForExtraction(const String &title, const String &intro, const String &pubDate, String &outWhat, String &outWhen, String &outWhere)
{
    // Try to extract using AI service if configured
    // Otherwise fall back to heuristic extraction

    if (aiEndpoint.length() == 0) {
        // AI endpoint not configured, use heuristic extraction
        return extractInfoHeuristic(title, intro, pubDate, outWhat, outWhen, outWhere);
    }

    // Build extraction prompt focusing on intro as primary content
    String prompt = "You are a precise emergency alert parser for Polish RCB alerts. Your task is to extract ONLY the most critical information.\n\n"
                    "Rules:\n"
                    "- Focus primarily on the INTRO text — it contains the main warning and details.\n"
                    "- Use TITLE only as additional context (e.g., hazard type or broader area).\n"
                    "- For \"where\": extract ONLY the real geographical location (gmina, powiat, miasto, wieś, województwo, etc.). Use the associated administrative area (gmina and powiat) if found.\n"
                    "- Dates/Times:\n"
                    "  - Look for explicit start and end date/time of the hazard in the text.\n"
                    "  - Keep Polish date/number formatting exactly as in the source.\n"
                    "  - If no specific validity period is mentioned, use the value PUB_DATE value for BOTH \"start\" and \"end\".\n"
                    "  - If only a start is mentioned, use it for \"start\" and PUB_DATE value (or mentioned end) for \"end\".\n"
                    "- Message: concise summary of the action/advice + hazard in Polish (100 - 210 characters total, including spaces). Use the original wording as much as possible. Include the dates in the message if they are specified in the alert.\n\n"
                    "Return EXCLUSIVELY this JSON (no explanations, no extra text, no markdown):\n"
                    {\"message\": \"concise warning in Polish with action/hazard and dates if available\", \"start\": \"DD.MM.RRRR or DD.MM.RRRR HH:MM\", \"end\": \"DD.MM.RRRR or DD.MM.RRRR HH:MM\", \"where\": \"geographical area only, max 30 chars\"}\n\n"
                    "TITLE: " + title + "\n"
                    "INTRO: " + intro + "\n"
                    "PUB_DATE: " + pubDate;

    // Call AI endpoint with POST
    HTTPClient http;
    WiFiClientSecure client;
    client.setInsecure();
    http.begin(client, aiEndpoint.c_str());
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
        success = parseAIResponse(response, outWhat, outWhen, outWhere);
    }
    http.end();

    if (success) {
        return true;
    }

    // Fall back to heuristic extraction if AI fails
    return extractInfoHeuristic(title, intro, pubDate, outWhat, outWhen, outWhere);
}

bool RCBAlertModule::parseAIResponse(const String &response, String &outWhat, String &outWhen, String &outWhere)
{
    // Gemini API response format: {"candidates":[{"content":{"parts":[{"text":"{\"what\":\"...\",\"when\":\"...\",\"where\":\"...\"}"}]}}]}
    // We need to extract the text field, then parse the inner JSON

    int textStart = response.indexOf("\"text\":\"");
    if (textStart < 0) return false;
    textStart += 8; // Move past "text":"

    int textEnd = textStart;
    while (textEnd < response.length()) {
        if (response.charAt(textEnd) == '"' && response.charAt(textEnd - 1) != '\\') {
            break;
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

    // Now parse the inner JSON for what, when, where
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

    outWhat = extractField("what");
    outWhen = extractField("when");
    outWhere = extractField("where");

    return (outWhat.length() > 0 && outWhen.length() > 0 && outWhere.length() > 0);
}

bool RCBAlertModule::extractInfoHeuristic(const String &title, const String &intro, const String &pubDateStr, String &outWhat, String &outWhen, String &outWhere)
{
    // Fallback heuristic extraction when AI is not available
    outWhere = "Poland";
    outWhen = pubDateStr;
    outWhat = title;

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

    // Extract alert_type: remove "Alert RCB - " prefix and trailing date
    outWhat = title;
    outWhat.replace("Alert RCB - ", "");

    // Remove trailing date patterns like "(19.11)"
    int parenPos = outWhat.lastIndexOf('(');
    if (parenPos >= 0) {
        String trailing = outWhat.substring(parenPos);
        if (trailing.indexOf('.') >= 0 && trailing.indexOf(')') > trailing.indexOf('.')) {
            outWhat = outWhat.substring(0, parenPos);
            outWhat.trim();
        }
    }

    return true;
}

void RCBAlertModule::extractCriticalInfo(Alert &alert, const String &pubDateStr, const String &intro)
{
    // Extract location, date_from, date_to, and alert_type from alert title and intro using AI or heuristics
    String what, when, where;

    // Try AI extraction, fall back to heuristics if not available
    callAIForExtraction(alert.title, intro, pubDateStr, what, when, where);

    alert.location = where;
    alert.alert_type = what;
    alert.valid_from = pubDateStr;
    // valid_to would need more sophisticated date parsing - for now leave it empty
}

#endif // HAS_ALERTING
