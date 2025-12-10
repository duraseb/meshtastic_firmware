#pragma once

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
    static constexpr int MIN_HOUR_OF_DAY = 20; // Don't fetch before 20:00

    // Message length limits (same as other modules)
    static constexpr int MAX_MESSAGE_BYTES = 200; // Leave room for formatting
    static constexpr int MAX_TOTAL_BYTES = 233;   // Meshtastic payload limit

    /**
     * Check if current time is within allowed fetch window
     * @return true if we can fetch now, false if too early in day
     */
    bool isWithinFetchWindow() const;

    /**
     * Build the AI prompt for weather forecast generation
     * @return Complete prompt string
     */
    String buildWeatherPrompt() const;

    /**
     * Extract the message from AI response
     * @param aiResponse Raw AI response
     * @return Cleaned message or empty string on failure
     */
    String extractMessageFromResponse(const String& aiResponse) const;

    /**
     * Validate message length and format
     * @param message Message to validate
     * @return true if valid, false otherwise
     */
    bool validateMessage(const String& message) const;
};