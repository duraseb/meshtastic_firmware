#include "RCBAlertSource.h"
#include "configuration.h"
#include <ctime>

RCBAlertSource::RCBAlertSource() {
    // Constructor
}

String RCBAlertSource::getFetchUrl() const {
    return "https://www.gov.pl/web/rcb/komunikaty";
}

unsigned long RCBAlertSource::getFetchIntervalMs() const {
    return 5 * 60 * 1000; // 5 minutes
}

std::vector<AlertSource::RawAlert> RCBAlertSource::fetchAndParseAlerts(
    std::function<String(const char*, int&)> httpGetCallback)
{
    std::vector<RawAlert> alerts;
    
    // Fetch the main page
    int httpCode = 0;
    String payload = httpGetCallback(getFetchUrl().c_str(), httpCode);
    
    if (httpCode != 200 || payload.length() == 0) {
        LOG_WARN("RCBAlertSource: Failed to fetch alerts page (HTTP code: %d)", httpCode);
        return alerts;
    }
    
    // Parse HTML page for alert cards
    // Look for <a href="/web/rcb/..." tags that contain alert cards
    int pos = 0;
    int foundCount = 0;

    while (true) {
        // Find next <a href="/web/rcb/..." tag
        int aPos = payload.indexOf("<a href=\"/web/rcb/", pos);
        if (aPos < 0) break;

        // Extract href value
        int hrefStart = payload.indexOf('"', aPos) + 1;
        int hrefEnd = payload.indexOf('"', hrefStart);
        if (hrefEnd <= hrefStart) { 
            pos = aPos + 1; 
            continue; 
        }

        String link = payload.substring(hrefStart, hrefEnd);
        if (!link.startsWith("/")) {
            link = String("/web/rcb/") + link;
        }
        link = String("https://www.gov.pl") + link;

        // Find the closing </a> tag
        int aEnd = payload.indexOf("</a>", aPos);
        if (aEnd < 0) { 
            pos = aPos + 1; 
            continue; 
        }

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

        // Only process if we have link, title, and title contains "Alert RCB"
        if (link.length() > 0 && title.length() > 0 && title.indexOf("Alert RCB") >= 0) {
            // Calculate unique alert ID from the URL (used for duplicate checking)
            uint32_t id = hashString(link);
            
            // For first pass, just create a stub with minimal info
            // Full content will be fetched later only for new alerts
            RawAlert alert;
            alert.link = link;
            alert.id = id;
            alert.title = title;
            alert.dateStr = dateStr;
            alert.intro = "";  // Will be populated by fetchFullAlertContent if needed
            alert.context = "";
            
            alerts.push_back(alert);
            foundCount++;
            
            pos = aEnd + 1;
        } else {
            pos = aPos + 1;
        }
    }
    
    LOG_INFO("RCBAlertSource: Found %d alerts in page", foundCount);
    return alerts;
}

AlertSource::RawAlert RCBAlertSource::fetchFullAlertContent(const RawAlert& rawAlert,
    std::function<String(const char*, int&)> httpGetCallback)
{
    RawAlert fullAlert = rawAlert;
    
    // Fetch the individual article page to get full content
    int httpCode = 0;
    String articleHtml = httpGetCallback(rawAlert.link.c_str(), httpCode);
    
    if (httpCode == 200 && articleHtml.length() > 0) {
        // Extract title from Twitter meta tag
        String twitterTitle = extractTextBetween(articleHtml, 
            "<meta name=\"twitter:title\" content=\"", "\"");
        
        // Extract intro from Twitter description meta tag
        String twitterDesc = extractTextBetween(articleHtml,
            "<meta name=\"twitter:description\" content=\"", "\"");
        
        // Extract full context from <article> tag
        String articleContext = extractTextBetween(articleHtml,
            "<article", "</article>");
        
        // Clean article context from HTML tags
        articleContext.replace("<p>", "");
        articleContext.replace("</p>", " ");
        articleContext.replace("<br>", " ");
        articleContext.replace("<br/>", " ");
        articleContext.replace("<strong>", "");
        articleContext.replace("</strong>", "");
        articleContext.replace("<ul>", " ");
        articleContext.replace("</ul>", " ");
        articleContext.replace("<li>", "- ");
        articleContext.replace("</li>", " ");
        articleContext.replace("<div", " ");
        articleContext.replace("</div>", " ");
        articleContext.replace("<span", " ");
        articleContext.replace("</span>", " ");
        // Remove everything between < and > (remaining tags)
        int tagStart = 0;
        while ((tagStart = articleContext.indexOf('<')) >= 0) {
            int tagEnd = articleContext.indexOf('>', tagStart);
            if (tagEnd >= 0) {
                articleContext.remove(tagStart, tagEnd - tagStart + 1);
            } else {
                break;
            }
        }
        articleContext.trim();
        
        // Use Twitter title if available, fallback to original title
        String finalTitle = twitterTitle.length() > 0 ? twitterTitle : rawAlert.title;
        
        // Remove "Alert RCB" prefix and redundant text
        finalTitle.replace("Alert RCB - ", "");
        finalTitle.replace("Alert RCB-", "");
        finalTitle.replace("Alert RCB ", "");
        finalTitle.replace(" - Rządowe Centrum Bezpieczeństwa - Portal Gov.pl", "");
        finalTitle.replace(" - Portal Gov.pl", "");
        finalTitle.trim();
        
        // Use Twitter description if available
        String finalIntro = twitterDesc.length() > 0 ? twitterDesc : "";
        finalIntro.trim();
        
        // Update full alert with extracted content
        fullAlert.title = finalTitle;
        fullAlert.intro = finalIntro;
        fullAlert.context = articleContext;
        
        LOG_DEBUG("RCBAlertSource: Fetched full content for: %s", finalTitle.c_str());
    } else {
        LOG_WARN("RCBAlertSource: Failed to fetch article %s (code: %d)", rawAlert.link.c_str(), httpCode);
    }
    
    return fullAlert;
}

String RCBAlertSource::buildAIPrompt(const RawAlert &rawAlert,
                                     int maxMessageBytes,
                                     int maxLocationChars) const
{
    // Clean up title and intro - remove "Alert RCB" prefix as it carries no meaning
    String cleanTitle = rawAlert.title;
    cleanTitle.replace("Alert RCB - ", "");
    cleanTitle.replace("Alert RCB-", "");
    cleanTitle.replace("Alert RCB ", "");
    cleanTitle.trim();
    
    String cleanIntro = rawAlert.intro;
    cleanIntro.replace("Alert RCB - ", "");
    cleanIntro.replace("Alert RCB-", "");
    cleanIntro.replace("Alert RCB ", "");
    cleanIntro.trim();
    
    String prompt = "Jesteś parserem alertów. Zwróć TYLKO ten format (bez dodatkowego tekstu):\n\n"
                    "message|||___|||start|||___|||end|||___|||where|||___|||severity\n\n"
                    "ZASADY:\n\n"
                    "message: Wyciągnij fakty z tekstu (max " + String(maxMessageBytes) + " bajtów UTF-8). "
                    "PRZEPISZ dokładnie: rodzaj zagrożenia, lokalizację, daty/godziny (jeśli są!), zalecenia"
                    "NIE DODAWAJ niczego od siebie! NIE WYMYŚLAJ informacji! "
                    "Możesz skrócić, ale ZACHOWAJ wszystkie fakty. "
                    "Skróty: gm., pow., woj., godz., tel. "
                    "Format dat może być: '22-23.11', 'od 22.11 godz. 18', itp. "
                    "NIE używaj: |||___|||\n\n"
                    "where: Nazwa geograficzna z TITLE i INTRO (max " + String(maxLocationChars) + " znaków).\n"
                    "- ZAWSZE znajdź lokalizację w tekście!\n"
                    "- Szukaj: nazwy województw, powiatów, gmin, miast\n"
                    "- Priorytet: powiat > województwo > gmina > miasto\n"
                    "- Wiele powiatów? Wypisz je lub podaj województwo\n"
                    "- NIE pisz 'brak', 'brak danych' - ZNAJDŹ lokalizację w tekście!\n\n"
                    "start i end: Format YYYY-MM-DD hh:mm:ss\n"
                    "- Szukaj dat w tekście\n"
                    "- Brak dat? → użyj PUB_DATE\n"
                    "- Tylko data? → start 00:00:00, end 23:59:59\n\n"
                    "severity: Liczba 0-10\n"
                    "0=krytyczne (wojna, katastrofa duża)\n"
                    "10=lokalne, małe, ćwiczenia\n\n"
                    "PRZYKŁAD (PRZEPISZ fakty, NIE WYMYŚLAJ):\n"
                    "Opady śniegu 22-23.11. Możliwe utrudnienia. Zachowaj ostrożność na drogach i chodnikach. Śledź komunikaty pogodowe.|||___|||2025-11-22 00:00:00|||___|||2025-11-23 23:59:59|||___|||woj. małopolskie|||___|||4\n\n"
                    "PUB_DATE: " + rawAlert.dateStr + "\n"
                    "TITLE: " + cleanTitle + "\n"
                    "INTRO: " + cleanIntro;
    
    if (rawAlert.context.length() > 0) {
        prompt += "\n\nCONTEXT: " + rawAlert.context;
    }
    
    return prompt;
}

String RCBAlertSource::extractLatestFromRSS(const String &payload, String &outId,
                                            String &outTitle, String &outLink, String &outContent)
{
    // Find the first <item> block
    int itemStart = payload.indexOf("<item>");
    if (itemStart < 0) {
        return "";
    }
    int itemEnd = payload.indexOf("</item>", itemStart);
    if (itemEnd < 0) {
        itemEnd = payload.length();
    }
    
    String item = payload.substring(itemStart, itemEnd);
    
    // Extract pubDate
    String pubDate = extractTextBetween(item, "<pubDate>", "</pubDate>");
    
    // Extract guid
    outId = extractTextBetween(item, "<guid>", "</guid>");
    
    // Extract title
    outTitle = extractTextBetween(item, "<title>", "</title>");
    
    // Extract link
    outLink = extractTextBetween(item, "<link>", "</link>");
    
    // Extract description
    outContent = extractTextBetween(item, "<description>", "</description>");
    
    return pubDate;
}

String RCBAlertSource::extractTextBetween(const String &html, const char *startTag,
                                          const char *endTag, size_t startPos)
{
    int start = html.indexOf(startTag, startPos);
    if (start < 0) {
        return "";
    }
    start += strlen(startTag);
    
    int end = html.indexOf(endTag, start);
    if (end < 0) {
        return "";
    }
    
    return html.substring(start, end);
}

uint32_t RCBAlertSource::hashString(const String &str)
{
    uint32_t hash = 5381;
    for (size_t i = 0; i < str.length(); i++) {
        hash = ((hash << 5) + hash) + str.charAt(i);
    }
    return hash;
}

