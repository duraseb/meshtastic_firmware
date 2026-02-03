#include "POZAlertSource.h"
#include "../AlertsModule.h"
#include "configuration.h"
#include "RTC.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

// Forward declaration for alertsModule
extern AlertsModule *alertsModule;

// Low-memory streaming JSON parser using ArduinoJson's "deserialization in chunks" feature
// See: https://arduinojson.org/v7/how-to/deserialize-a-very-large-document/
//
// ArduinoJson automatically stops reading at closing brace of each object, allowing us to
// parse array elements one at a time. Combined with filtering, this uses only ~20KB memory.
//
// Memory usage: ~20KB total (16KB JsonDocument + 256B filter + overhead)
bool POZAlertSource::parsePOZStream(WiFiClient* stream, DynamicJsonDocument& doc, std::vector<AlertSource::RawAlert>& alerts) {
    LOG_INFO("POZAlertSource: Starting chunked deserialization (ArduinoJson streaming)...");

    // Create filter for ArduinoJson - extracts only needed fields from each event
    StaticJsonDocument<256> filter;
    JsonObject eventFilter = filter.to<JsonObject>();
    eventFilter["id"] = true;
    eventFilter["title"] = true;
    eventFilter["date_from"] = true;
    eventFilter["hour_from"] = true;
    eventFilter["date_to"] = true;
    eventFilter["hour_to"] = true;
    eventFilter["miejsce"] = true;

    int eventsProcessed = 0;

    // Step 1: Navigate to the events array using Stream::find()
    // ArduinoJson doc: "you can use Stream::find()" to jump to array start
    LOG_DEBUG("POZAlertSource: Searching for events array...");

    if (!stream->find("\"events\":[")) {
        LOG_WARN("POZAlertSource: Could not find events array in response");
        return false;
    }

    LOG_DEBUG("POZAlertSource: Found events array, starting chunked deserialization");

    // Step 2: Parse each event object one at a time
    // ArduinoJson automatically stops reading at the closing brace of each object
    do {
        DeserializationError error = deserializeJson(doc, *stream, DeserializationOption::Filter(filter));

        if (error) {
            if (error == DeserializationError::EmptyInput) {
                // End of array or no more data
                break;
            }
            LOG_DEBUG("POZAlertSource: Event #%d parse error: %s", eventsProcessed + 1, error.c_str());
            continue;
        }

        eventsProcessed++;

        // Extract event data
        String id = doc.containsKey("id") ? String(doc["id"].as<const char*>()) : "";
        String title = doc.containsKey("title") ? String(doc["title"].as<const char*>()) : "";
        String date_from = doc.containsKey("date_from") ? String(doc["date_from"].as<const char*>()) : "";
        String hour_from = doc.containsKey("hour_from") ? String(doc["hour_from"].as<const char*>()) : "";
        String date_to = doc.containsKey("date_to") ? String(doc["date_to"].as<const char*>()) : "";
        String hour_to = doc.containsKey("hour_to") ? String(doc["hour_to"].as<const char*>()) : "";
        String miejsce = doc.containsKey("miejsce") ? String(doc["miejsce"].as<const char*>()) : "";

        if (id.length() == 0 || title.length() == 0) {
            LOG_DEBUG("POZAlertSource: Skipping event with missing id or title");
            continue;
        }

        // Determine expiry date - use date_to/hour_to if available, otherwise end of date_from day
        String expiryDate = date_to.length() > 0 ? date_to : date_from;
        String expiryTime;
        if (date_to.length() > 0) {
            // If date_to exists, use hour_to (or default to end of day)
            expiryTime = hour_to.length() > 0 ? hour_to : "23:59:59";
        } else {
            // If no date_to, use end of date_from day (not hour_from!)
            expiryTime = "23:59:59";
        }

        if (expiryDate.length() == 0) {
            LOG_DEBUG("POZAlertSource: Skipping event %s - no expiry date", id.c_str());
            continue;
        }

        // Build expiry datetime string
        String expiryDateTime = expiryDate;
        if (expiryTime.length() > 0) {
            expiryDateTime += " " + expiryTime;
        } else {
            expiryDateTime += " 23:59:59"; // Default to end of day if no time specified
        }

        // Check if event has already expired
        time_t expiryTimestamp = alertsModule->parseDateString(expiryDateTime);
        time_t currentTime = getTime(false);

        if (expiryTimestamp <= currentTime) {
            LOG_WARN("POZAlertSource: Skipping expired event %s (expiry: %s, current: %u, expiry_ts: %u)", 
                    id.c_str(), expiryDateTime.c_str(), currentTime, expiryTimestamp);
            continue;
        }

        LOG_DEBUG("POZAlertSource: Event is valid - expiry: %s (timestamp: %u > current: %u)", 
                expiryDateTime.c_str(), expiryTimestamp, currentTime);

        // Create alert
        RawAlert rawAlert;
        rawAlert.link = id;
        rawAlert.id = hashString(id);
        rawAlert.title = title;

        // Build message in the requested format: {date_from} {hour_from} {title} {miejsce}
        String message = date_from + " " + hour_from + " " + title + " " + miejsce;
        rawAlert.intro = message;

        // Set structured dates
        if (date_from.length() >= 10 && hour_from.length() >= 5) {
            rawAlert.structuredStartDate = date_from + " " + hour_from;
        }
        if (expiryDate.length() >= 10) {
            rawAlert.structuredEndDate = expiryDate + " " + expiryTime;
        }

        alerts.push_back(rawAlert);

        LOG_INFO("POZAlertSource: Added valid event %s: %s (expiry: %s, structured dates: %s-%s)", 
                id.c_str(), title.c_str(), expiryDateTime.c_str(), 
                rawAlert.structuredStartDate.c_str(), rawAlert.structuredEndDate.c_str());

        // Feed watchdog periodically during large response processing
        if (eventsProcessed % 50 == 0) {
            yield();
        }

    // Step 3: Skip comma between array elements, stop at closing bracket
    // ArduinoJson doc: "you can use Stream::findUntil()" to skip between elements
    } while (stream->findUntil(",", "]"));

    LOG_INFO("POZAlertSource: Streaming complete, processed %d events, kept %d valid events, skipped %d expired",
             eventsProcessed, alerts.size(), eventsProcessed - (int)alerts.size());
    return alerts.size() > 0 || eventsProcessed > 0;
}

// Forward declaration for alertsModule
extern AlertsModule *alertsModule;

// HTTP timeout constant (matches AlertsModule)
static constexpr unsigned long HTTP_TIMEOUT_MS = 10000;

String POZAlertSource::getFetchUrl() const {
    return "https://www.poznan.pl/mim/public/events-json/events.json?p=1&day=week&api=afisz";
}

unsigned long POZAlertSource::getFetchIntervalMs() const {
    return 60 * 60 * 1000; // 60 minutes - events don't change rapidly
}

std::vector<AlertSource::RawAlert> POZAlertSource::fetchAndParseAlerts(
    std::function<String(const char*, int&)> httpGetCallback)
{
    std::vector<RawAlert> alerts;

    // Use streaming JSON parsing with direct HTTP piping for memory efficiency
    LOG_DEBUG("POZAlertSource: Starting streaming JSON parse from %s", getFetchUrl().c_str());

    // Use AlertsModule's streaming HTTP utility with shared JSON document
    auto jsonProcessor = [&](WiFiClient* stream, DynamicJsonDocument& doc) -> bool {
        if (!stream) {
            LOG_ERROR("POZAlertSource: Stream is null");
            return false;
        }

        LOG_INFO("POZAlertSource: Starting true streaming JSON parsing...");

        // Use our custom streaming parser that processes JSON in chunks
        // and only extracts Poznań events without loading the entire response
        bool success = this->parsePOZStream(stream, doc, alerts);

        if (!success) {
            LOG_ERROR("POZAlertSource: Streaming JSON parsing failed");
            return false;
        }

        LOG_INFO("POZAlertSource: Streaming JSON parsing completed successfully");

        return true;
    };

    // Call the streaming HTTP method from AlertsModule with shared document
    if (!alertsModule->httpGetStream(getFetchUrl().c_str(), jsonProcessor)) {
        LOG_ERROR("POZAlertSource: Streaming HTTP request failed");
        return alerts;
    }

    return alerts;
}

String POZAlertSource::getPreprocessedMessage(const RawAlert &rawAlert, int maxMessageBytes) const
{
    // For POZ events, we return the pre-formatted message directly
    // This bypasses AI processing entirely
    return rawAlert.intro;
}

String POZAlertSource::buildAIPrompt(const RawAlert &rawAlert,
                                      int maxMessageBytes,
                                      int maxLocationChars) const
{
    // POZ events use direct processing via getPreprocessedMessage()
    // This method is here for completeness but should not be called for POZ alerts
    return "";
}

uint32_t POZAlertSource::hashString(const String &str)
{
    uint32_t hash = 5381;
    for (size_t i = 0; i < str.length(); i++) {
        hash = ((hash << 5) + hash) + str.charAt(i);
    }
    return hash;
}