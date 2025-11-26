#pragma once

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
};

