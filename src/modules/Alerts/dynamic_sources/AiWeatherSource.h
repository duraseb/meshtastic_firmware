#pragma once

#if defined(HAS_ALERTING) && HAS_ALERTING

#include "../DynamicSource.h"
#include "../AIService.h"

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
    unsigned long getFetchIntervalMs() const override;

    String fetchAndFormat(
        std::function<String(const char*, int&)> httpGetCallback) override;

private:
    // Configuration constants
    static constexpr unsigned long DEFAULT_FETCH_INTERVAL_MS = 24 * 60 * 60 * 1000; // 24 hours
    static constexpr int DEFAULT_MIN_HOUR_OF_DAY = 20; // Don't fetch before 20:00

    /**
     * Get the minimum hour of day for fetches
     * @return configured minimum hour or default (20)
     */
    static int getMinHourOfDay();

    /**
     * Clean footnote/reference markers from AI response
     * @param input the raw AI response
     * @return cleaned response without footnote markers
     */
    String cleanFootnotes(const String& input) const;

    // Message length limits (same as alert sources - let system handle payload sizing)
    static constexpr int MAX_MESSAGE_BYTES = 218; // Slightly increased buffer for AI-generated content
    static constexpr int MAX_TOTAL_BYTES = 233;   // Meshtastic payload limit

    /**
     * Check if current time is within allowed fetch window
     * @return true if we can fetch now, false if too early in day
     */
    bool isWithinFetchWindow() const;

    /**
     * Build the AI prompt for weather forecast generation
     * @param weatherJson Real weather data JSON from Open-Meteo API
     * @param tomorrowDate Formatted date string for tomorrow (Polish format)
     * @param apiDate Date string formatted for API URL (day_month)
     * @return Complete prompt string
     */
    String buildWeatherPrompt(const String& weatherJson, const String& tomorrowDate, const String& apiDate) const;

    /**
     * Extract weather forecast from AI response text
     * Expected format: "{Name}: {weather forecast}"
     * @param fullResponse Complete AI response text
     * @return Extracted weather forecast or empty string if not found
     */
    String extractWeatherForecast(const String& fullResponse) const;

};
#endif // HAS_ALERTING
