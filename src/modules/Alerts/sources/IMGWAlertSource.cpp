#include "configuration.h"

#if defined(HAS_ALERTING) && HAS_ALERTING && !MESHTASTIC_EXCLUDE_ALERT_IMGW

#include "IMGWAlertSource.h"
#include "../AlertsModule.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <map>
#include <utility>

// Deduplicate alerts by message content, merging areas with semicolons
void IMGWAlertSource::deduplicateAlerts(std::vector<TempAlert>& rawAlerts, std::vector<RawAlert>& outputAlerts) {
    // Use a map to group alerts by message hash
    std::map<std::pair<uint64_t, size_t>, TempAlert> deduped;

    for (auto& tempAlert : rawAlerts) {
        auto key = std::make_pair(tempAlert.messageHash, tempAlert.messageLength);

        if (deduped.find(key) == deduped.end()) {
            // First occurrence of this message - add it
            deduped[key] = tempAlert;
        } else {
            // Duplicate message - merge areas with semicolon
            String existingArea = "";
            String newArea = "";

            // Extract AREA from existing alert's context
            int areaPos = deduped[key].alert.context.indexOf("AREA:");
            if (areaPos >= 0) {
                int nextPipe = deduped[key].alert.context.indexOf("|", areaPos);
                if (nextPipe >= 0) {
                    existingArea = deduped[key].alert.context.substring(areaPos + 5, nextPipe);
                } else {
                    existingArea = deduped[key].alert.context.substring(areaPos + 5);
                }
            }

            // Extract AREA from new alert's context
            areaPos = tempAlert.alert.context.indexOf("AREA:");
            if (areaPos >= 0) {
                int nextPipe = tempAlert.alert.context.indexOf("|", areaPos);
                if (nextPipe >= 0) {
                    newArea = tempAlert.alert.context.substring(areaPos + 5, nextPipe);
                } else {
                    newArea = tempAlert.alert.context.substring(areaPos + 5);
                }
            }

            // Merge areas with semicolon if both exist and are different
            if (existingArea.length() > 0 && newArea.length() > 0 && existingArea != newArea) {
                if (existingArea.indexOf(newArea) < 0) { // Avoid duplicates
                    existingArea += "; " + newArea;
                }
            } else if (newArea.length() > 0 && existingArea.length() == 0) {
                existingArea = newArea;
            }

            // Update the context with merged area
            String newContext = "AREA:" + existingArea;
            int nextPipe = deduped[key].alert.context.indexOf("|");
            if (nextPipe >= 0) {
                newContext += deduped[key].alert.context.substring(nextPipe);
            }
            deduped[key].alert.context = newContext;
        }
    }

    // Convert back to output format and keep only the first 15 after deduplication
    for (auto& pair : deduped) {
        outputAlerts.push_back(pair.second.alert);
        // Stop after 15 alerts to limit memory usage
        if (outputAlerts.size() >= 15) {
            break;
        }
    }
}

// Low-memory streaming JSON parser using ArduinoJson's "deserialization in chunks" feature
// See: https://arduinojson.org/v7/how-to/deserialize-a-very-large-document/
//
// ArduinoJson automatically stops reading at closing brace of each object, allowing us to
// parse array elements one at a time. Combined with filtering, this uses only ~20KB memory.
//
// Memory usage: ~20KB total (16KB JsonDocument + 256B filter + overhead)
bool IMGWAlertSource::parseIMGWStream(WiFiClient* stream, DynamicJsonDocument& doc, std::vector<AlertSource::RawAlert>& alerts) {
    LOG_INFO("IMGWAlertSource: Starting chunked deserialization (ArduinoJson streaming)...");

    auto updateFnv1a = [&](uint64_t hash, const String &value) -> uint64_t {
        const uint64_t FNV_PRIME = 1099511628211ULL;
        for (size_t i = 0; i < value.length(); i++) {
            hash ^= static_cast<uint8_t>(value.charAt(i));
            hash *= FNV_PRIME;
        }
        return hash;
    };

    auto updateSeparator = [&](uint64_t hash) -> uint64_t {
        const uint64_t FNV_PRIME = 1099511628211ULL;
        hash ^= static_cast<uint8_t>('|');
        hash *= FNV_PRIME;
        return hash;
    };

    auto appendAreaDesc = [&](String &target, const String &addition) {
        if (addition.length() == 0) return;
        if (target.length() == 0) {
            target = addition;
            return;
        }
        target += ", ";
        target += addition;
    };

    // Create filter for ArduinoJson - extracts only needed fields from each warning
    StaticJsonDocument<256> filter;
    JsonObject alertFilter = filter["alert"].to<JsonObject>();
    alertFilter["identifier"] = true;
    JsonObject infoFilter = alertFilter["info"][0].to<JsonObject>();
    infoFilter["language"] = true;
    infoFilter["headline"] = true;
    infoFilter["description"] = true;
    infoFilter["instruction"] = true;
    infoFilter["onset"] = true;
    infoFilter["expires"] = true;
    infoFilter["severity"] = true;
    infoFilter["certainty"] = true;
    infoFilter["urgency"] = true;
    infoFilter["area"][0]["areaDesc"] = true;

    int warningsProcessed = 0;

    // Temporary vector to collect all alerts before deduplication
    std::vector<TempAlert> rawAlerts;

    // Step 1: Navigate to the warnings array using Stream::find()
    // ArduinoJson doc: "you can use Stream::find()" to jump to array start
    LOG_DEBUG("IMGWAlertSource: Searching for warnings array...");

    if (!stream->find("\"warnings\":[")) {
        LOG_WARN("IMGWAlertSource: Could not find warnings array in response");
        return false;
    }

    LOG_DEBUG("IMGWAlertSource: Found warnings array, starting chunked deserialization");

    // Step 2: Parse each warning object one at a time
    // ArduinoJson automatically stops reading at the closing brace of each object
    do {
        DeserializationError error = deserializeJson(doc, *stream, DeserializationOption::Filter(filter));

        if (error) {
            if (error == DeserializationError::EmptyInput) {
                // End of array or no more data
                break;
            }
            LOG_DEBUG("IMGWAlertSource: Warning #%d parse error: %s", warningsProcessed + 1, error.c_str());
            continue;
        }

        warningsProcessed++;

        // Extract Polish alert if present
        if (!doc.containsKey("alert")) continue;
        
        JsonObject alert = doc["alert"];
        String identifier = alert.containsKey("identifier") ? 
                          String(alert["identifier"].as<const char*>()) : "";

        if (!alert.containsKey("info") || !alert["info"].is<JsonArray>()) continue;
        
        JsonArray infoArray = alert["info"];
        
        for (size_t j = 0; j < infoArray.size(); j++) {
            JsonObject info = infoArray[j];
            
            // Check for Polish language
            if (!info.containsKey("language") || 
                strcmp(info["language"].as<const char*>(), "pl-PL") != 0) {
                continue;
            }
            
            // Found Polish info block - extract alert
            String headline = info.containsKey("headline") ? 
                             String(info["headline"].as<const char*>()) : "";
            
            if (headline.length() == 0) break;
            
            RawAlert rawAlert;
            rawAlert.link = identifier;
            rawAlert.id = hashString(identifier);
            rawAlert.title = headline;
            
            String description = info.containsKey("description") ?
                                String(info["description"].as<const char*>()) : "";
            rawAlert.intro = description;
            
            String onset = info.containsKey("onset") ? 
                          String(info["onset"].as<const char*>()) : "";
            String expires = info.containsKey("expires") ? 
                            String(info["expires"].as<const char*>()) : "";
            
            // meteoalarm.org returns Polish alert timestamps already in local
            // Polish time (CET/CEST), not UTC — despite an ISO 8601 `Z` suffix
            // in some responses. Keep them as-is so the displayed/scheduled
            // times match the user's wall clock.
            if (onset.length() >= 19) {
                rawAlert.structuredStartDate = onset.substring(0, 10) + " " + onset.substring(11, 19);
                rawAlert.dateStr = onset;
            }
            if (expires.length() >= 19) {
                rawAlert.structuredEndDate = expires.substring(0, 10) + " " + expires.substring(11, 19);
            }
            
            // Build context from area descriptions
            String areaDesc = "";
            if (info.containsKey("area") && info["area"].is<JsonArray>()) {
                JsonArray areas = info["area"];
                for (size_t k = 0; k < areas.size(); k++) {
                    if (areas[k].containsKey("areaDesc")) {
                        if (areaDesc.length() > 0) areaDesc += ", ";
                        areaDesc += String(areas[k]["areaDesc"].as<const char*>());
                    }
                }
            }
            
            String severity = info.containsKey("severity") ? 
                             String(info["severity"].as<const char*>()) : "";
            String certainty = info.containsKey("certainty") ? 
                              String(info["certainty"].as<const char*>()) : "";
            String urgency = info.containsKey("urgency") ? 
                            String(info["urgency"].as<const char*>()) : "";
            String instruction = info.containsKey("instruction") ? 
                                String(info["instruction"].as<const char*>()) : "";
            
            String instructionKey = "";
            String contextSuffix = "|SEVERITY:" + severity +
                                  "|CERTAINTY:" + certainty + "|URGENCY:" + urgency;
            if (instruction.length() > 0 && instruction.length() < 200) {
                instructionKey = instruction;
                contextSuffix += "|INSTRUCTION:" + instruction;
            }

            uint64_t messageHash = 14695981039346656037ULL;
            size_t messageLength = 0;
            // Exclude headline from hash to allow grouping alerts with same description but different areas
            messageHash = updateFnv1a(messageHash, description);
            messageLength += description.length();
            messageHash = updateSeparator(messageHash);
            messageHash = updateFnv1a(messageHash, instructionKey);
            messageLength += instructionKey.length();
            messageHash = updateSeparator(messageHash);
            messageHash = updateFnv1a(messageHash, severity);
            messageLength += severity.length();
            messageHash = updateSeparator(messageHash);
            messageHash = updateFnv1a(messageHash, certainty);
            messageLength += certainty.length();
            messageHash = updateSeparator(messageHash);
            messageHash = updateFnv1a(messageHash, urgency);
            messageLength += urgency.length();
            messageHash = updateSeparator(messageHash);
            messageHash = updateFnv1a(messageHash, onset);
            messageLength += onset.length();
            messageHash = updateSeparator(messageHash);
            messageHash = updateFnv1a(messageHash, expires);
            messageLength += expires.length();

            // Store alert data for later deduplication
            TempAlert tempAlert;
            tempAlert.alert = rawAlert;
            tempAlert.alert.context = "AREA:" + areaDesc + contextSuffix;
            tempAlert.messageHash = messageHash;
            tempAlert.messageLength = messageLength;

            rawAlerts.push_back(tempAlert);
            
            break;  // Found Polish, no need to check other languages
        }

        // Feed watchdog periodically during large response processing
        if (warningsProcessed % 50 == 0) {
            yield();
        }

    // Step 3: Skip comma between array elements, stop at closing bracket
    // ArduinoJson doc: "you can use Stream::findUntil()" to skip between elements
    } while (stream->findUntil(",", "]"));

    // Step 4: Deduplicate alerts by message content and merge areas
    deduplicateAlerts(rawAlerts, alerts);

    LOG_INFO("IMGWAlertSource: Streaming complete, kept %d deduplicated Polish alerts from %d warnings",
             alerts.size(), warningsProcessed);
    return alerts.size() > 0 || warningsProcessed > 0;
}

// Forward declaration for alertsModule
extern AlertsModule *alertsModule;

// HTTP timeout constant (matches AlertsModule)
static constexpr unsigned long HTTP_TIMEOUT_MS = 10000;

String IMGWAlertSource::getFetchUrl() const {
    return "https://feeds.meteoalarm.org/api/v1/warnings/feeds-poland/";
}

unsigned long IMGWAlertSource::getFetchIntervalMs() const {
    return 60 * 60 * 1000;
}

std::vector<AlertSource::RawAlert> IMGWAlertSource::fetchAndParseAlerts(
    std::function<String(const char*, int&)> httpGetCallback)
{
    std::vector<RawAlert> alerts;

    // Use streaming JSON parsing with direct HTTP piping for memory efficiency
    LOG_DEBUG("IMGWAlertSource: Starting streaming JSON parse from %s", getFetchUrl().c_str());

    // Use AlertsModule's streaming HTTP utility with shared JSON document
    auto jsonProcessor = [&](WiFiClient* stream, DynamicJsonDocument& doc) -> bool {
        if (!stream) {
            LOG_ERROR("IMGWAlertSource: Stream is null");
            return false;
        }

        LOG_INFO("IMGWAlertSource: Starting true streaming JSON parsing...");

        // Use our custom streaming parser that processes JSON in chunks
        // and only extracts Polish alerts without loading the entire 233KB response
        bool success = this->parseIMGWStream(stream, doc, alerts);

        if (!success) {
            LOG_ERROR("IMGWAlertSource: Streaming JSON parsing failed");
            return false;
        }

        LOG_INFO("IMGWAlertSource: Streaming JSON parsing completed successfully");

        return true;
    };

    // Call the streaming HTTP method from AlertsModule with shared document
    if (!alertsModule->httpGetStream(getFetchUrl().c_str(), jsonProcessor)) {
        LOG_ERROR("IMGWAlertSource: Streaming HTTP request failed");
        return alerts;
    }

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
                    "message:{message}|||___|||where:{where}|||___|||severity:{severity}\n\n"
                    "ZASADY:\n\n"
                    "{message}: Max " + String(maxMessageBytes) + " bajtów UTF-8\n"
                    "- Z HEADLINE i DESCRIPTION (dodaj INSTRUCTION jeśli się zmieści)\n"
                    "- Zachowaj kluczowe fakty: zjawisko, intensywność, skutki\n"
                    "- Skróty: woj., pow., gm., godz.\n"
                    "- NIE WYMYŚLAJ nic\n\n"
                    "{where}: Max " + String(maxLocationChars) + " znaków\n"
                    "- Z AREA (PRZEPISZ dokładnie, skróć jeśli za długa)\n"
                    "- Wiele powiatów: wypisz lub użyj 'woj. [nazwa]'\n\n"
                    "{severity}: Liczba 0-10 - oceń całościowo\n"
                    "- Weź pod uwagę SOURCE SEVERITY: " + severity + "\n"
                    "- Weź pod uwagę SOURCE CERTAINTY: " + certainty + "\n"
                    "- Weź pod uwagę SOURCE URGENCY: " + urgency + "\n"
                    "- Bazowa wartość (obliczona z powyższych): " + String(calculatedSeverity) + "\n"
                    "- Oceń treść komunikatu (HEADLINE, DESCRIPTION, INSTRUCTION)\n"
                    "- Uwzględnij rzeczywiste zagrożenie opisane w tekście\n"
                    "- Ostateczna wartość to Twoja ocena na podstawie WSZYSTKICH powyższych\n\n"
                    "PRZYKŁAD:\n"
                    "message:Opady śniegu 21.11, umiarkowane, pokrywa do 15 cm. Utrudnienia na drogach.|||___|||where:" +
                    area + "|||___|||severity:" + String(calculatedSeverity) + "\n\n"
                    "\n"
                    "AREA: " + area + "\n"
                    "HEADLINE: " + rawAlert.title + "\n"
                    "DESCRIPTION: " + rawAlert.intro;
    
    if (instruction.length() > 0 && instruction.length() < 300) {
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
    // Our scale: 0=critical (extreme), 10=informational

    int base = 10; // Default to informational

    // Base severity mapping (lower = more critical)
    if (severity.indexOf("Extreme") >= 0) base = 0;
    else if (severity.indexOf("Severe") >= 0) base = 2;
    else if (severity.indexOf("Moderate") >= 0) base = 5;
    else if (severity.indexOf("Minor") >= 0) base = 8;
    
    // Certainty modifier (higher certainty = lower severity number = more critical)
    float certMod = 0;
    if (certainty.indexOf("Observed") >= 0) certMod = -2.0;  // High certainty = more critical
    else if (certainty.indexOf("Likely") >= 0) certMod = -0.6; // Medium certainty
    else if (certainty.indexOf("Possible") >= 0) certMod = 0;   // Low certainty
    else if (certainty.indexOf("Unlikely") >= 0) certMod = 1.0; // Very low certainty = less critical

    // Urgency modifier (higher urgency = lower severity number = more critical)
    float urgMod = 0;
    if (urgency.indexOf("Immediate") >= 0) urgMod = -2.0; // High urgency = more critical
    else if (urgency.indexOf("Expected") >= 0) urgMod = -0.6; // Medium urgency
    else if (urgency.indexOf("Future") >= 0) urgMod = 0;     // Future urgency
    else if (urgency.indexOf("Past") >= 0) urgMod = 2.0;     // Past urgency = less critical
    
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

#endif // HAS_ALERTING && !MESHTASTIC_EXCLUDE_ALERT_IMGW
