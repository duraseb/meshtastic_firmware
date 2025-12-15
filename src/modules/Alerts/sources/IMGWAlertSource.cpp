#include "IMGWAlertSource.h"
#include "../AlertsModule.h"
#include "configuration.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

// Custom streaming JSON parser for IMGW alerts
// Since we can't fit the entire 233KB JSON in memory, we parse in chunks
// and only extract Polish alerts
bool IMGWAlertSource::parseIMGWStream(WiFiClient* stream, std::vector<AlertSource::RawAlert>& alerts) {
    LOG_INFO("IMGWAlertSource: Starting chunked streaming parsing with PSRAM buffers...");

    // Check if buffers are allocated
    if (!chunkBuffer || !jsonBuffer) {
        LOG_ERROR("IMGWAlertSource: Buffers not allocated - cannot parse");
        return false;
    }

    bool inWarningsArray = false;
    int braceDepth = 0;
    int bracketDepth = 0;
    bool inWarningObject = false;
    AlertSource::RawAlert currentAlert;

    // Reset buffer position for new parsing session
    jsonBufferPos = 0;

    while (stream->available() || stream->connected()) {
        size_t bytesRead = stream->readBytes(chunkBuffer, CHUNK_SIZE);
        if (bytesRead == 0) {
            delay(10);  // Wait for more data
            continue;
        }

        // Add chunk to our buffer (with bounds checking)
        if (jsonBufferPos + bytesRead < JSON_BUFFER_SIZE) {
            memcpy(&jsonBuffer[jsonBufferPos], chunkBuffer, bytesRead);
            jsonBufferPos += bytesRead;
        } else {
            // Buffer full, shift data to make room (keep last part)
            size_t shiftAmount = jsonBufferPos - (JSON_BUFFER_SIZE - bytesRead - 1);
            memmove(jsonBuffer, &jsonBuffer[shiftAmount], jsonBufferPos - shiftAmount);
            jsonBufferPos -= shiftAmount;
            memcpy(&jsonBuffer[jsonBufferPos], chunkBuffer, bytesRead);
            jsonBufferPos += bytesRead;
        }

        // Process the buffer character by character
        size_t pos = 0;
        while (pos < jsonBufferPos) {
            char c = jsonBuffer[pos];

            if (c == '{') {
                braceDepth++;
            if (braceDepth == 2 && inWarningsArray && !inWarningObject) {
                // Starting a new warning object
                inWarningObject = true;
                currentAlert = AlertSource::RawAlert();
                currentAlert.id = 0; // Initialize
                currentAlert.link = "";
                currentAlert.title = "";
                currentAlert.dateStr = "";
                currentAlert.intro = "";
                currentAlert.context = "";
                LOG_DEBUG("IMGWAlertSource: Found warning object");
            }
            } else if (c == '}') {
                braceDepth--;
                if (braceDepth == 1 && inWarningObject) {
                    // End of warning object
                    inWarningObject = false;
                    // Check if this alert is for Poland and has content
                    if (!currentAlert.context.isEmpty() &&
                        (!currentAlert.title.isEmpty() || !currentAlert.intro.isEmpty())) {
                        alerts.push_back(currentAlert);
                        LOG_INFO("IMGWAlertSource: Captured Polish alert: %s", currentAlert.title.c_str());
                    }
                }
            } else if (c == '[') {
                bracketDepth++;
                if (bracketDepth == 1 && strstr(jsonBuffer, "\"warnings\"") != nullptr && (char*)strstr(jsonBuffer, "\"warnings\"") < &jsonBuffer[pos]) {
                    inWarningsArray = true;
                    LOG_DEBUG("IMGWAlertSource: Found warnings array");
                }
            } else if (c == ']') {
                bracketDepth--;
                if (bracketDepth == 0 && inWarningsArray) {
                    inWarningsArray = false;
                    LOG_INFO("IMGWAlertSource: Finished processing warnings array, total Polish alerts: %d", alerts.size());
                }
            } else if (inWarningObject && c == '"') {
                // Parse string values
                size_t startPos = pos;
                pos++; // Skip opening quote

                String key = "";
                String value = "";

                // Find key
                while (pos < jsonBufferPos && jsonBuffer[pos] != '"') {
                    key += jsonBuffer[pos];
                    pos++;
                }
                if (pos >= jsonBufferPos) break;
                pos++; // Skip closing quote

                // Skip colon and whitespace
                while (pos < jsonBufferPos && (jsonBuffer[pos] == ':' || jsonBuffer[pos] == ' ' || jsonBuffer[pos] == '\t' || jsonBuffer[pos] == '\n' || jsonBuffer[pos] == '\r')) {
                    pos++;
                }

                if (pos < jsonBufferPos && jsonBuffer[pos] == '"') {
                    // String value
                    pos++; // Skip opening quote
                    while (pos < jsonBufferPos && jsonBuffer[pos] != '"') {
                        if (jsonBuffer[pos] == '\\') {
                            pos++; // Skip escape character
                            if (pos < jsonBufferPos) {
                                value += jsonBuffer[pos];
                            }
                        } else {
                            value += jsonBuffer[pos];
                        }
                        pos++;
                    }

                    // Store the value if it's a key we're interested in
                    if (strcmp(key.c_str(), "headline") == 0) {
                        currentAlert.title = value;
                    } else if (strcmp(key.c_str(), "description") == 0) {
                        currentAlert.intro = value;
                    } else if (strcmp(key.c_str(), "area") == 0) {
                        // Check if it's for Poland - store in context field
                        if (strstr(value.c_str(), "Polska") != nullptr || strstr(value.c_str(), "Poland") != nullptr) {
                            currentAlert.context = value;
                        }
                    } else if (strcmp(key.c_str(), "identifier") == 0) {
                        // Use identifier as unique ID (simple hash)
                        uint32_t hash = 0;
                        for (size_t i = 0; i < value.length(); i++) {
                            hash = hash * 31 + value[i];
                        }
                        currentAlert.id = hash;
                        currentAlert.link = value;
                    }
                }
            }

            pos++;
        }

        // Shift buffer to keep recent data for context (simplified approach)
        // In a full implementation, we'd track processed position more carefully
        if (jsonBufferPos > CHUNK_SIZE) {
            size_t keepAmount = min((size_t)CHUNK_SIZE, jsonBufferPos);
            memmove(jsonBuffer, &jsonBuffer[jsonBufferPos - keepAmount], keepAmount);
            jsonBufferPos = keepAmount;
        }
    }

    LOG_INFO("IMGWAlertSource: Streaming parsing completed, captured %d Polish alerts", alerts.size());
    return true;
}

// Forward declaration for alertsModule
extern AlertsModule *alertsModule;

// HTTP timeout constant (matches AlertsModule)
static constexpr unsigned long HTTP_TIMEOUT_MS = 10000;

// PSRAM buffer allocation/deallocation
void IMGWAlertSource::allocatePsramBuffers() {
    #ifdef BOARD_HAS_PSRAM
    if (ESP.getPsramSize() > 0) {
        LOG_INFO("IMGWAlertSource: Allocating %d KB chunk buffer in PSRAM", CHUNK_SIZE/1024);
        chunkBuffer = static_cast<char*>(ps_malloc(CHUNK_SIZE));
        if (!chunkBuffer) {
            LOG_ERROR("IMGWAlertSource: Failed to allocate chunk buffer in PSRAM");
        }

        LOG_INFO("IMGWAlertSource: Allocating %d KB JSON buffer in PSRAM", JSON_BUFFER_SIZE/1024);
        jsonBuffer = static_cast<char*>(ps_malloc(JSON_BUFFER_SIZE));
        if (!jsonBuffer) {
            LOG_ERROR("IMGWAlertSource: Failed to allocate JSON buffer in PSRAM");
            // Try to free chunk buffer if JSON buffer failed
            if (chunkBuffer) {
                free(chunkBuffer);
                chunkBuffer = nullptr;
            }
        }
    } else {
        LOG_WARN("IMGWAlertSource: PSRAM not available, falling back to heap allocation");
    }
    #endif

    // Fallback to heap allocation if PSRAM not available or allocation failed
    if (!chunkBuffer) {
        LOG_INFO("IMGWAlertSource: Using heap allocation for chunk buffer");
        chunkBuffer = static_cast<char*>(malloc(CHUNK_SIZE));
    }
    if (!jsonBuffer) {
        LOG_INFO("IMGWAlertSource: Using heap allocation for JSON buffer");
        jsonBuffer = static_cast<char*>(malloc(JSON_BUFFER_SIZE));
    }

    if (!chunkBuffer || !jsonBuffer) {
        LOG_ERROR("IMGWAlertSource: Failed to allocate buffers - parsing will not work");
    }
}

void IMGWAlertSource::freePsramBuffers() {
    if (chunkBuffer) {
        #ifdef BOARD_HAS_PSRAM
        if (ESP.getPsramSize() > 0) {
            free(chunkBuffer);  // ps_free() is just free() for PSRAM
        } else {
            free(chunkBuffer);
        }
        #else
        free(chunkBuffer);
        #endif
        chunkBuffer = nullptr;
    }

    if (jsonBuffer) {
        #ifdef BOARD_HAS_PSRAM
        if (ESP.getPsramSize() > 0) {
            free(jsonBuffer);
        } else {
            free(jsonBuffer);
        }
        #else
        free(jsonBuffer);
        #endif
        jsonBuffer = nullptr;
    }

    jsonBufferPos = 0;
}

// Note: PSRAM is available on ESP32-S3 but we'll use heap for now
// Future optimization could use PSRAM for very large JSON documents

IMGWAlertSource::IMGWAlertSource() :
    chunkBuffer(nullptr),
    jsonBuffer(nullptr),
    jsonBufferPos(0)
{
    // Allocate PSRAM buffers
    allocatePsramBuffers();
}

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

    // Use AlertsModule's streaming HTTP utility
    auto jsonProcessor = [&](WiFiClient* stream) -> bool {
        if (!stream) {
            LOG_ERROR("IMGWAlertSource: Stream is null");
            return false;
        }

        LOG_INFO("IMGWAlertSource: Starting true streaming JSON parsing...");

        // Use our custom streaming parser that processes JSON in chunks
        // and only extracts Polish alerts without loading the entire 233KB response
        bool success = this->parseIMGWStream(stream, alerts);

        if (!success) {
            LOG_ERROR("IMGWAlertSource: Streaming JSON parsing failed");
            return false;
        }

        LOG_INFO("IMGWAlertSource: Streaming JSON parsing completed successfully");

        return true;
    };

    // Call the streaming HTTP method from AlertsModule
    if (!alertsModule->httpGetStream(getFetchUrl().c_str(), jsonProcessor)) {
        LOG_ERROR("IMGWAlertSource: Streaming HTTP request failed");
        return alerts;
    }

    // The JSON processing is now handled by processParsedJson callback
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

template<typename T>
bool IMGWAlertSource::processParsedJson(T &doc, std::vector<RawAlert> &alerts)
{
    // Check if we have warnings array
    if (!doc.containsKey("warnings")) {
        LOG_WARN("IMGWAlertSource: No warnings key found in response");
        return false;
    }

    JsonArray warnings = doc["warnings"];
    if (warnings.isNull()) {
        LOG_WARN("IMGWAlertSource: Warnings is not an array");
        return false;
    }

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
    return true;
}

