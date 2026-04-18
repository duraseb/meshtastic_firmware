#pragma once

#if defined(HAS_ALERTING) && HAS_ALERTING

#include <Arduino.h>
#include <functional>

/**
 * Base class for dynamic data sources.
 *
 * Unlike AlertSource which handles alert-based data requiring AI processing,
 * persistence, and resending logic, DynamicSource is for simple periodic data
 * that gets fetched, parsed locally, and sent immediately without storage.
 *
 * Examples: weather reports, sensor readings, status updates
 */
class DynamicSource {
public:
    virtual ~DynamicSource() {}

    /**
     * Get the source identifier (e.g., "SYNOP", "SENSOR")
     * Used for logging and message prefixes if needed
     */
    virtual String getSourceId() const = 0;

    /**
     * Get the URL to fetch data from
     */
    virtual String getFetchUrl() const = 0;
    
    /**
     * Returns the channel name this source's alerts should be sent to.
     * Override to use a dedicated channel. Default returns empty string,
     * meaning the global alert channel (alertChannelName) will be used.
     */
    virtual String getChannelName() const { return ""; }
    
    /**
     * Returns an info/notification string to broadcast on the primary channel.
     * Called approximately every 60 seconds by the main loop.
     * The provider should track its own cadence internally and return
     * an empty string when it has nothing to broadcast.
     * Non-empty return value will be sent once to the primary channel.
     */
    virtual String getInfoPrompt() { return ""; }

    /**
     * Get the fetch interval in milliseconds
     * How often to check for new data from this source
     */
    virtual unsigned long getFetchIntervalMs() const = 0;

    /**
     * Fetch, parse, and format data into a ready-to-send message.
     * No AI processing - parsing is done locally.
     * No persistence - data is sent immediately and not stored.
     *
     * @param httpGetCallback Callback to perform HTTP GET requests
     * @return Ready-to-send message string, or empty string if fetch/parse failed
     */
    virtual String fetchAndFormat(
        std::function<String(const char*, int&)> httpGetCallback) = 0;

    /**
     * Check if this source requires WiFi for operation.
     * Default is true. Override to false for local data sources.
     */
    virtual bool requiresWiFi() const { return true; }

    /**
     * Whether the source is currently eligible to fetch. Sources can use this
     * to implement time-of-day windows or other gating rules. When this returns
     * false, the module will retry again shortly instead of treating the fetch
     * attempt as consumed (which would defer by the full getFetchIntervalMs()).
     */
    virtual bool isReadyToFetch() { return true; }
};

#endif // HAS_ALERTING
