#pragma once

#if defined(HAS_ALERTING) && HAS_ALERTING

#include "../DynamicSource.h"
#include "../AIService.h"
#include "concurrency/Lock.h"
#ifdef ARCH_ESP32
#include <freertos/task.h>
#endif
#include "mesh/generated/meshtastic/mesh.pb.h"
class WiFiClient;

/**
 * Dynamic source for AI-generated Polish weather forecasts from famous Polish figures.
 *
 * Fetches current date, finds a famous Polish person born on the next day,
 * gets weather forecast for Poznań, and formats it in the style of that person.
 *
 * Uses AI to generate forecasts in the style of Polish historical figures.
 */
class AiWeatherSource : public DynamicSource {
public:
    AiWeatherSource();
    virtual ~AiWeatherSource();

    String getSourceId() const override { return "AI_WEATHER"; }
    String getFetchUrl() const override { return ""; } // Not needed for AI source
    String getChannelName() const override { return "PoznanEvent"; }
    unsigned long getFetchIntervalMs() const override;

    /**
     * Returns whether the last fetch attempt failed (no forecast produced).
     * Used to determine if a shorter retry interval should be used.
     */
    bool didLastFetchFail() const { return lastFetchFailed; }

    String fetchAndFormat(
        std::function<String(const char*, int&)> httpGetCallback) override;

    bool isAsyncFetchInProgress() const;
    bool isAsyncFetchReady() const;

    /**
     * Get the minimum hour of day for fetches
     * @return configured minimum hour or default (20)
     */
     static int getMinHourOfDay();

private:
#ifdef ARCH_ESP32
    // Build and execute weather+person lookup asynchronously to keep main run loop responsive.
    bool startAsyncFetchForDate(int month, int day, const String& birthDate, const String& tomorrowDate);
    void stopAsyncFetch();

    // Worker entrypoint for background task.
    static void asyncFetchTask(void *pvParameters);

    // Execute HTTP + parsing work from background task.
    void runAsyncFetch();

    // Set async result state safely.
    void setAsyncResultReady(const String& weatherJson, const String& birthPersonHints);
    void setAsyncResultError(const String& error);

    enum class AsyncFetchState {
        IDLE,
        RUNNING,
        READY,
        FAILED,
    };

    // Background fetch state and synchronization
    mutable concurrency::Lock asyncStateLock;
    volatile AsyncFetchState asyncState = AsyncFetchState::IDLE;
    TaskHandle_t asyncTaskHandle = nullptr;
    bool asyncStopRequested = false;
    int asyncMonth = 0;
    int asyncDay = 0;
    String asyncBirthDate;
    String asyncTomorrowDate;
    String asyncWeatherJson;
    String asyncBirthPersonHints;
    String asyncError;
#endif

    // Configuration constants
    // 22h interval means the timer elapses around 18:xx the next day; isWithinFetchWindow()
    // then holds until 20:00, so fetches always land in the 20:00–22:00 window.
    static constexpr unsigned long DEFAULT_FETCH_INTERVAL_MS = 22 * 60 * 60 * 1000; // 22 hours
    static constexpr unsigned long RETRY_FETCH_INTERVAL_MS = 30 * 60 * 1000;        // 30 min on failure

    // Set by fetchAndFormat (main loop only, no lock needed)
    bool lastFetchFailed = false;
    static constexpr int DEFAULT_MIN_HOUR_OF_DAY = 20; // Don't fetch before 20:00
    static constexpr unsigned long WIKIDATA_QUERY_TIMEOUT_MS = 65000; // Wikidata can be slow
    static constexpr int WIKIDATA_QUERY_MAX_RETRIES = 5;
    static constexpr int WIKIDATA_QUERY_LIMIT = 30;
    static constexpr size_t WIKIDATA_MAX_RESPONSE_BYTES = 70000;
    static constexpr size_t WIKIDATA_MAX_PROMPT_CHARS = 3000;
    static constexpr size_t WIKIDATA_MAX_CANDIDATES = 100;

    /**
     * Clean footnote/reference markers from AI response
     * @param input the raw AI response
     * @return cleaned response without footnote markers
     */
    String cleanFootnotes(const String& input) const;

    // Message length limits (same as alert sources - let system handle payload sizing)
    static constexpr int MAX_MESSAGE_BYTES = meshtastic_Constants_DATA_PAYLOAD_LEN - 15; // Slightly shorter than full payload

    /**
     * Check if current time is within allowed fetch window
     * @return true if we can fetch now, false if too early in day
     */
    bool isWithinFetchWindow() const;

    /**
     * Build the AI prompt for weather forecast generation
     * @param weatherJson Real weather data JSON from Open-Meteo API
     * @param tomorrowDate Formatted date string for tomorrow (Polish format)
     * @param birthDate Formatted date string for birth (Polish format)
     * @param birthPersonHints Formatted list of historical figures born on birthDate
     * @return Complete prompt string
     */
    String buildWeatherPrompt(const String& weatherJson, const String& tomorrowDate, const String& birthDate, const String& birthPersonHints) const;

    // Build a SPARQL query for people born on month/day in Poland with cultural occupations
    String buildWikidataBirthdayQuery(int month, int day) const;

    // Percent-encode query text for URL use
    String urlEncode(const String& value) const;

    // Fetch and extract Wikidata results as formatted hint list
    bool fetchBirthPersonHints(int month, int day, const String& birthDate, String& outHints) const;

    // Streamed HTTP fetch with watchdog-safe polling and query-oriented limits
    String httpGetLongTimeout(const String& url, int& httpCode, unsigned long timeoutMs) const;

    // Parse Wikidata SPARQL JSON output into compact prompt hints
    bool parseBirthPersonHints(const String& payload, const String& birthDate, String& outHints) const;
    bool parseBirthPersonHints(WiFiClient* stream, const String& birthDate, String& outHints) const;

    /**
     * Extract weather forecast from AI response text
     * Expected format: "{Name}: {weather forecast}"
     * @param fullResponse Complete AI response text
     * @return Extracted weather forecast or empty string if not found
     */
    String extractWeatherForecast(const String& fullResponse) const;

};
#endif // HAS_ALERTING
