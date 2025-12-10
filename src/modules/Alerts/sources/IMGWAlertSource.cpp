#include "IMGWAlertSource.h"
#include "configuration.h"
#include <ArduinoJson.h>

IMGWAlertSource::IMGWAlertSource() {
    // Constructor
}

String IMGWAlertSource::getFetchUrl() const {
    return "https://feeds.meteoalarm.org/api/v1/warnings/feeds-poland/";
}

unsigned long IMGWAlertSource::getFetchIntervalMs() const {
    return 15 * 60 * 1000; // 15 minutes (weather alerts update less frequently)
}

std::vector<AlertSource::RawAlert> IMGWAlertSource::fetchAndParseAlerts(
    std::function<String(const char*, int&)> httpGetCallback)
{
    std::vector<RawAlert> alerts;
    
    // Fetch JSON data
    int httpCode = 0;
    String payload = httpGetCallback(getFetchUrl().c_str(), httpCode);
    
    if (httpCode != 200 || payload.length() == 0) {
        LOG_WARN("IMGWAlertSource: Failed to fetch alerts (HTTP code: %d)", httpCode);
        return alerts;
    }
    
    // Parse JSON using ArduinoJson (use heap allocation to avoid stack overflow)
    DynamicJsonDocument doc(180 * 1024); // Large document for weather alert data

    DeserializationError error = deserializeJson(doc, payload);
    if (error) {
        LOG_ERROR("IMGWAlertSource: JSON parsing failed: %s", error.c_str());
        return alerts;
    }

    LOG_DEBUG("IMGWAlertSource: JSON parsed successfully");

    // Check if we have warnings array
    if (!doc.containsKey("warnings") || !doc["warnings"].is<JsonArray>()) {
        LOG_WARN("IMGWAlertSource: No warnings array found in response");
        return alerts;
    }

    JsonArray warnings = doc["warnings"];

    // Process up to 10 most recent alerts
    for (size_t i = 0; i < warnings.size() && alerts.size() < 10; i++) {
        JsonVariant warning = warnings[i];

        if (!warning.containsKey("alert")) {
            continue;
        }

        JsonVariant alert = warning["alert"];

        // Extract identifier (unique ID)
        String identifier = alert.containsKey("identifier") ? String(alert["identifier"].as<const char*>()) : "";

        // Check if this alert has Polish language info
        bool foundPolish = false;
        String headline, description, instruction, areaDesc, onset, expires;
        String severity, certainty, urgency;

        if (alert.containsKey("info") && alert["info"].is<JsonArray>()) {
            JsonArray infoArray = alert["info"];

            // Look for Polish language info block
            for (size_t j = 0; j < infoArray.size(); j++) {
                JsonVariant info = infoArray[j];

                if (info.containsKey("language") && strcmp(info["language"], "pl-PL") == 0) {
                    foundPolish = true;

                    // Extract fields from this info block
                    if (info.containsKey("headline")) headline = String(info["headline"].as<const char*>());
                    if (info.containsKey("description")) description = String(info["description"].as<const char*>());
                    if (info.containsKey("instruction")) instruction = String(info["instruction"].as<const char*>());
                    if (info.containsKey("onset")) onset = String(info["onset"].as<const char*>());
                    if (info.containsKey("expires")) expires = String(info["expires"].as<const char*>());
                    if (info.containsKey("severity")) severity = String(info["severity"].as<const char*>());
                    if (info.containsKey("certainty")) certainty = String(info["certainty"].as<const char*>());
                    if (info.containsKey("urgency")) urgency = String(info["urgency"].as<const char*>());

                    // Extract area descriptions
                    if (info.containsKey("area") && info["area"].is<JsonArray>()) {
                        JsonArray areas = info["area"];
                        for (size_t k = 0; k < areas.size(); k++) {
                            JsonVariant area = areas[k];
                            if (area.containsKey("areaDesc")) {
                                if (areaDesc.length() > 0) areaDesc += ", ";
                                areaDesc += String(area["areaDesc"].as<const char*>());

                                // Limit to avoid too long strings
                                if (areaDesc.length() > 200) {
                                    areaDesc = areaDesc.substring(0, 200) + "...";
                                    break;
                                }
                            }
                        }
                    }

                    break; // Found Polish info, no need to check other info blocks
                }
            }
        }
        
        // Only process Polish language alerts
        if (foundPolish && headline.length() > 0) {
            RawAlert rawAlert;
            rawAlert.link = identifier; // Use identifier as unique link
            rawAlert.title = headline;
            rawAlert.dateStr = onset; // Use onset time as publication date
            rawAlert.intro = description;
            rawAlert.id = hashString(identifier); // Hash of IMGW identifier (unique alert ID)

            // IMGW has structured dates - convert from ISO8601 to our format
            if (onset.length() >= 19) {
                rawAlert.structuredStartDate = onset.substring(0, 10) + " " + onset.substring(11, 19);
            }
            if (expires.length() >= 19) {
                rawAlert.structuredEndDate = expires.substring(0, 10) + " " + expires.substring(11, 19);
            }

            // Store additional metadata in context (area, severity, instructions)
            rawAlert.context = "AREA:" + areaDesc + "|SEVERITY:" + severity +
                               "|CERTAINTY:" + certainty + "|URGENCY:" + urgency;
            if (instruction.length() > 0) {
                rawAlert.context += "|INSTRUCTION:" + instruction;
            }

            alerts.push_back(rawAlert);

            LOG_DEBUG("IMGWAlertSource: Found alert: %s (valid: %s to %s)",
                     headline.c_str(), rawAlert.structuredStartDate.c_str(),
                     rawAlert.structuredEndDate.c_str());
        }
    }
    
    LOG_INFO("IMGWAlertSource: Found %d Polish language alerts", alerts.size());
    
    return alerts;
}

String IMGWAlertSource::buildAIPrompt(const RawAlert &rawAlert,
                                      int maxMessageBytes,
                                      int maxLocationChars) const
{
    // Parse metadata from context string
    String area, onset, expires, severity, certainty, urgency, instruction;
    
    int pos = 0;
    int nextPos = rawAlert.context.indexOf("|", pos);
    while (nextPos >= 0 || pos < rawAlert.context.length()) {
        String part = (nextPos >= 0) ? rawAlert.context.substring(pos, nextPos) : rawAlert.context.substring(pos);
        
        if (part.startsWith("AREA:")) area = part.substring(5);
        else if (part.startsWith("ONSET:")) onset = part.substring(6);
        else if (part.startsWith("EXPIRES:")) expires = part.substring(8);
        else if (part.startsWith("SEVERITY:")) severity = part.substring(9);
        else if (part.startsWith("CERTAINTY:")) certainty = part.substring(10);
        else if (part.startsWith("URGENCY:")) urgency = part.substring(8);
        else if (part.startsWith("INSTRUCTION:")) instruction = part.substring(12);
        
        if (nextPos < 0) break;
        pos = nextPos + 1;
        nextPos = rawAlert.context.indexOf("|", pos);
    }
    
    // Calculate severity based on CAP fields
    uint8_t calculatedSeverity = calculateSeverity(severity, certainty, urgency);
    
    // Convert ISO8601 dates to our format
    String startDate = convertISO8601ToMeshtastic(onset);
    String endDate = convertISO8601ToMeshtastic(expires);
    
    // For IMGW, structured dates come from RawAlert, but GPT still evaluates severity
    String prompt = "Jesteś parserem alertów pogodowych IMGW. Zwróć TYLKO ten format (bez dodatkowego tekstu):\n\n"
                    "message|||___|||IGNORE|||___|||IGNORE|||___|||where|||___|||severity\n\n"
                    "UWAGA: Daty są już ustalone (wpisz IGNORE). Oceń severity na podstawie treści.\n\n"
                    "ZASADY:\n\n"
                    "message: Max " + String(maxMessageBytes) + " bajtów UTF-8\n"
                    "- Z HEADLINE i DESCRIPTION (dodaj INSTRUCTION jeśli się zmieści)\n"
                    "- Zachowaj kluczowe fakty: zjawisko, intensywność, skutki\n"
                    "- Skróty: woj., pow., gm., godz.\n"
                    "- NIE WYMYŚLAJ nic\n"
                    "- NIE używaj: |||___|||\n\n"
                    "where: Max " + String(maxLocationChars) + " znaków\n"
                    "- Z AREA (PRZEPISZ dokładnie, skróć jeśli za długa)\n"
                    "- Wiele powiatów: wypisz lub użyj 'woj. [nazwa]'\n\n"
                    "severity: Liczba 0-10 - oceń całościowo\n"
                    "- Weź pod uwagę SOURCE SEVERITY: " + severity + "\n"
                    "- Weź pod uwagę SOURCE CERTAINTY: " + certainty + "\n"
                    "- Weź pod uwagę SOURCE URGENCY: " + urgency + "\n"
                    "- Bazowa wartość (obliczona z powyższych): " + String(calculatedSeverity) + "\n"
                    "- Oceń treść komunikatu (HEADLINE, DESCRIPTION, INSTRUCTION)\n"
                    "- Uwzględnij rzeczywiste zagrożenie opisane w tekście\n"
                    "- Ostateczna wartość to Twoja ocena na podstawie WSZYSTKICH powyższych\n\n"
                    "PRZYKŁAD:\n"
                    "Opady śniegu 21.11, umiarkowane, pokrywa do 15 cm. Utrudnienia na drogach.|||___|||IGNORE|||___|||IGNORE|||___|||" + 
                    area + "|||___|||" + String(calculatedSeverity) + "\n\n"
                    "AREA: " + area + "\n"
                    "HEADLINE: " + rawAlert.title + "\n"
                    "DESCRIPTION: " + rawAlert.intro;
    
    if (instruction.length() > 0 && instruction.length() < 200) {
        prompt += "\nINSTRUCTION: " + instruction;
    }
    
    return prompt;
}


uint8_t IMGWAlertSource::calculateSeverity(const String &severity, const String &certainty, const String &urgency) const
{
    // CAP severity: Minor (Yellow), Moderate (Orange), Severe (Red), Extreme (Violet)
    // CAP certainty: Observed, Likely, Possible, Unlikely
    // CAP urgency: Immediate, Expected, Future, Past
    //
    // Our scale: 0=info, 3=minor, 5=moderate, 8=severe, 10=extreme
    
    int base = 5; // Default moderate
    
    // Base severity mapping
    if (severity.indexOf("Minor") >= 0) base = 3;
    else if (severity.indexOf("Moderate") >= 0) base = 5;
    else if (severity.indexOf("Severe") >= 0) base = 8;
    else if (severity.indexOf("Extreme") >= 0) base = 10;
    
    // Certainty modifier
    float certMod = 0;
    if (certainty.indexOf("Observed") >= 0) certMod = 1.0;
    else if (certainty.indexOf("Likely") >= 0) certMod = 0.5;
    else if (certainty.indexOf("Possible") >= 0) certMod = 0;
    else if (certainty.indexOf("Unlikely") >= 0) certMod = -1.0;
    
    // Urgency modifier
    float urgMod = 0;
    if (urgency.indexOf("Immediate") >= 0) urgMod = 1.0;
    else if (urgency.indexOf("Expected") >= 0) urgMod = 0.5;
    else if (urgency.indexOf("Future") >= 0) urgMod = 0;
    else if (urgency.indexOf("Past") >= 0) urgMod = -2.0;
    
    // Calculate final severity
    int final = base + (int)(certMod + urgMod);
    
    // Clamp to 0-10
    if (final < 0) final = 0;
    if (final > 10) final = 10;
    
    return (uint8_t)final;
}

String IMGWAlertSource::convertISO8601ToMeshtastic(const String &iso8601) const
{
    // Input: 2025-11-21T05:00:00+01:00
    // Output: 2025-11-21 05:00:00
    
    if (iso8601.length() < 19) return iso8601; // Invalid format
    
    // Extract date and time parts
    String result = iso8601.substring(0, 10) + " " + iso8601.substring(11, 19);
    return result;
}

uint32_t IMGWAlertSource::hashString(const String &str)
{
    uint32_t hash = 5381;
    for (size_t i = 0; i < str.length(); i++) {
        hash = ((hash << 5) + hash) + str.charAt(i);
    }
    return hash;
}

