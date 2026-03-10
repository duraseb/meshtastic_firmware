#if defined(HAS_ALERTING) && HAS_ALERTING

#include "AlertsModule.h"
#include "AIService.h"
#include "sources/RCBAlertSource.h"
#include "sources/IMGWAlertSource.h"
#include "sources/POZAlertSource.h"
#include "dynamic_sources/IMGWSynopSource.h"
#include "dynamic_sources/AiWeatherSource.h"
#include "mesh/wifi/WiFiAPClient.h"
#include "FSCommon.h"
#include "main.h"
#include "mesh/generated/meshtastic/mesh.pb.h"
#include "mesh/Router.h"
#include "mesh/MeshService.h"
#include "mesh/Channels.h"
#include "memGet.h"
#include "mesh/NodeDB.h"
#include "SPILock.h"
#include "RTC.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ctime>
#include <ctype.h>
#include <vector>
#include <algorithm>
#include <sstream>
#include <ArduinoJson.h>

#ifdef ARCH_ESP32
#include "esp_task_wdt.h"
#include "esp_heap_caps.h"
#include "freertos/task.h"
#endif

#ifndef DISABLE_NTP
#include <NTPClient.h>
#include <WiFiUdp.h>
#endif

// Helper to reset watchdog timer (architecture-aware)
static inline void feedWatchdog()
{
#ifdef ARCH_ESP32
    esp_task_wdt_reset();
#endif
}

AlertsModule *alertsModule = nullptr;

AlertsModule::AlertsModule() : OSThread("AlertsModule"), sharedJsonDoc(SHARED_JSON_DOC_SIZE)
{
    LOG_INFO("[AlertsModule] Initializing Multi-Source Alert System");

#ifdef ALERT_CHANNEL_NAME
    alertChannelName = ALERT_CHANNEL_NAME;
#else
    alertChannelName = "Alert";
#endif

    currentState = ModuleState::INIT;
    initializationDone = false;
    lastFetchTime = 0;
    lastCleanupTime = 0;
    intervalMs = 5 * 60 * 1000; // Default 5 minute check interval

    lastMemoryCheckTime = 0;
    lastPendingAlertLogTime = 0;
    lastReportedMemoryUsage = 0;

    processingCtx.active = false;
    processingCtx.source = nullptr;
    processingCtx.stateStartTime = 0;

    numSources = 0;
    currentSourceIndex = 0;

    // Initialize broadcasting state
    nextBroadcastTimeMs = BROADCAST_INITIAL_DELAY_MS; // First broadcast after initial delay
    broadcastingEnabled = false;

    // Register RCB source
    sources[numSources] = new RCBAlertSource();
    sourceLastFetchTime[numSources] = 0;
    LOG_INFO("[AlertsModule] Registered source: %s (fetch every %lu min)",
             sources[numSources]->getSourceId().c_str(),
             sources[numSources]->getFetchIntervalMs() / 60000);
    numSources++;

    // Register IMGW source
    sources[numSources] = new IMGWAlertSource();
    sourceLastFetchTime[numSources] = 0;
    LOG_INFO("[AlertsModule] Registered source: %s (fetch every %lu min)",
             sources[numSources]->getSourceId().c_str(),
             sources[numSources]->getFetchIntervalMs() / 60000);
    numSources++;

    // Register POZ source
    sources[numSources] = new POZAlertSource();
    sourceLastFetchTime[numSources] = 0;
    LOG_INFO("[AlertsModule] Registered source: %s (fetch every %lu min)",
             sources[numSources]->getSourceId().c_str(),
             sources[numSources]->getFetchIntervalMs() / 60000);
    numSources++;

    LOG_INFO("[AlertsModule] Total alert sources registered: %d", numSources);

    // Initialize AI service
    if (aiService == nullptr) {
        aiService = new AIService();
    }

    // Register dynamic sources (periodic data, no AI, no persistence)
    numDynamicSources = 0;
    currentDynamicSourceIndex = 0;

    // Register IMGW SYNOP weather source
    dynamicSources[numDynamicSources] = new IMGWSynopSource();
    dynamicSourceLastFetchTime[numDynamicSources] = 0;
    LOG_INFO("[AlertsModule] Registered dynamic source: %s (fetch every %lu min)",
             dynamicSources[numDynamicSources]->getSourceId().c_str(),
             dynamicSources[numDynamicSources]->getFetchIntervalMs() / 60000);
    numDynamicSources++;

    // Register AI Weather source
    dynamicSources[numDynamicSources] = new AiWeatherSource();
    dynamicSourceLastFetchTime[numDynamicSources] = 0;
    LOG_INFO("[AlertsModule] Registered dynamic source: %s (fetch every %lu hours, after %02d:00)",
             dynamicSources[numDynamicSources]->getSourceId().c_str(),
             dynamicSources[numDynamicSources]->getFetchIntervalMs() / (60 * 60 * 1000),
             AiWeatherSource::getMinHourOfDay());
    numDynamicSources++;

    LOG_INFO("[AlertsModule] Total dynamic sources registered: %d", numDynamicSources);

    // Log AI service status
    if (aiService != nullptr) {
        int configuredCount = aiService->getConfiguredProviderCount();
        if (configuredCount == 0) {
            LOG_ERROR("[AlertsModule] ==============================================================================");
            LOG_ERROR("[AlertsModule] FATAL - No AI providers configured!");
            LOG_ERROR("[AlertsModule] At least one API key must be set in .env file:");
            LOG_ERROR("[AlertsModule] GEMINI_API_KEY (free tier: 1500 req/day, recommended)");
            LOG_ERROR("[AlertsModule] PERPLEXITY_API_KEY (Pro: $5/month credit)");
            LOG_ERROR("[AlertsModule] MISTRAL_API_KEY (free tier, good for Polish)");
            LOG_ERROR("[AlertsModule] GROQ_API_KEY (free tier: 14,400 req/day, fallback)");
            LOG_ERROR("[AlertsModule] See src/modules/Alerts/ALERTING_SETUP.md for setup instructions");
            LOG_ERROR("[AlertsModule] ==============================================================================");
        } else {
            LOG_INFO("[AlertsModule] %d AI provider(s) available via AIService", configuredCount);
        }
    } else {
        LOG_ERROR("[AlertsModule] AIService initialization failed");
    }

    alertsModule = this;

    LOG_DEBUG("[AlertsModule] Constructor completed - initialization deferred to runOnce()");
}

AlertsModule::~AlertsModule() {
    // Clean up sources
    for (int i = 0; i < numSources; i++) {
        delete sources[i];
        sources[i] = nullptr;
    }

    // Clean up dynamic sources
    for (int i = 0; i < numDynamicSources; i++) {
        delete dynamicSources[i];
        dynamicSources[i] = nullptr;
    }

    // Clear vectors and cache to free memory
    pendingAlerts.clear();
    alerts.clear();
    processedAlertIds.clear();
    processedAlertIdOrder.clear();

    // Note: aiService is a global managed elsewhere, not cleaned up here
}

bool AlertsModule::loadConfig()
{
    return true;
}

String AlertsModule::httpGet(const char *url, int &httpCode)
{
    String payload = "";
    httpCode = -1;

    if (!isWifiAvailable()) {
        LOG_DEBUG("WiFi not available for HTTP request");
        return payload;
    }

    LOG_DEBUG("Fetching URL: %s", url);

    // Use unique_ptr for automatic cleanup
    // IMPORTANT: client must be declared before http so that http is destroyed first
    // (C++ destroys locals in reverse declaration order). HTTPClient holds a reference
    // to the WiFiClient, so the client must outlive http to avoid use-after-free.
    std::unique_ptr<WiFiClientSecure> client(new WiFiClientSecure());
    std::unique_ptr<HTTPClient> http(new HTTPClient());

    if (!http || !client) {
        LOG_ERROR("Failed to allocate HTTP client resources");
        return payload;
    }

    client->setInsecure();
    http->begin(*client, url);
    http->setTimeout(HTTP_TIMEOUT_MS);

    // Add headers to reduce server load and identify ourselves
    http->addHeader("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");
    http->addHeader("Accept", "text/html,application/json,text/plain,*/*");

    httpCode = http->GET();

    // Reset watchdog after potentially long HTTP operation
    feedWatchdog();

    if (httpCode > 0) {
        if (httpCode == HTTP_CODE_OK) {
            // Monitor memory usage for large HTTP responses
            size_t heapBefore = memGet.getFreeHeap();

            // Use getString() but validate the response
            payload = http->getString();

            // Validate that we actually got data and it's not corrupted
            if (payload.length() == 0) {
                LOG_ERROR("HTTP response received but payload is empty");
                return "";
            }

            size_t heapAfter = memGet.getFreeHeap();
            size_t memoryUsed = (heapBefore > heapAfter) ? (heapBefore - heapAfter) / 1024 : 0;
            LOG_DEBUG("HTTP GET successful, received %d bytes (heap used: %d KB, free: %d KB)",
                     payload.length(), memoryUsed, heapAfter/1024);

            // Basic validation - check if response looks like valid data
            bool hasValidChars = false;
            const char* buffer = payload.c_str();
            for (int i = 0; i < min(100, (int)payload.length()); i++) {
                if (buffer[i] >= 32 || buffer[i] == '\n' || buffer[i] == '\r' || buffer[i] == '\t') {
                    hasValidChars = true;
                    break;
                }
            }

            if (!hasValidChars) {
                LOG_WARN("HTTP response appears to contain only control characters, possible corruption");
            }

            // Check for unexpected null characters (we shouldn't be getting any)
            int nullCharCount = 0;
            const char* payloadBuffer = payload.c_str();
            for (size_t i = 0; i < payload.length(); i++) {
                if (payloadBuffer[i] == '\x00') {
                    nullCharCount++;
                }
            }

            if (nullCharCount > 0) {
                LOG_ERROR("HTTP response contains %d unexpected null characters - possible data corruption", nullCharCount);
                // Don't filter, but log the issue for debugging
            }

            // Sanity check payload size (prevent memory exhaustion)
            if (payload.length() > 200000) { // 200KB limit
                LOG_WARN("Response too large (%d bytes), truncating", payload.length());
                payload = payload.substring(0, 200000);
            }
        } else {
            LOG_WARN("HTTP GET returned code %d", httpCode);
        }
    } else {
        LOG_ERROR("HTTP GET failed with code %d", httpCode);
    }

    // Explicit cleanup (unique_ptr will handle it, but be explicit)
    http->end();

    return payload;
}

/**
 * Stream HTTP response directly to JSON parser for memory-efficient parsing of large responses
 * @param url The URL to fetch
 * @param jsonProcessor Callback function that receives the stream and processes JSON
 * @return true if streaming completed successfully
 */
// Streaming method with shared JSON document for maximum memory efficiency
bool AlertsModule::httpGetStream(const char *url, std::function<bool(WiFiClient* stream, DynamicJsonDocument& doc)> jsonProcessor)
{
    if (!isWifiAvailable()) {
        LOG_DEBUG("[AlertsModule] WiFi not available for streaming HTTP request");
        return false;
    }

    // URL logging handled by caller for cleaner output

    // Retry logic for transient network failures
    const int MAX_RETRIES = 2;
    bool success = false;

    for (int attempt = 0; attempt <= MAX_RETRIES && !success; attempt++) {
        if (attempt > 0) {
            LOG_DEBUG("[AlertsModule] Retrying HTTP request (attempt %d/%d)", attempt + 1, MAX_RETRIES + 1);
            // Brief delay before retry
            delay(1000);
        }

        // Use unique_ptr for automatic cleanup
        // client declared before http to ensure correct destruction order
        std::unique_ptr<WiFiClientSecure> client(new WiFiClientSecure());
        std::unique_ptr<HTTPClient> http(new HTTPClient());

        if (!http || !client) {
            LOG_ERROR("[AlertsModule] Failed to allocate HTTP client resources for streaming");
            return false;
        }

        client->setInsecure();
        http->begin(*client, url);
        http->setTimeout(HTTP_TIMEOUT_MS);

        http->addHeader("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");
        http->addHeader("Accept", "application/json");
        http->addHeader("Accept-Encoding", "identity");  // Disable compression to reduce buffering

        LOG_DEBUG("[AlertsModule] Attempting HTTP GET to %s", url);
        int httpCode = http->GET();

        // Reset watchdog after potentially long HTTP operation
        feedWatchdog();

        if (httpCode > 0) {
            if (httpCode == HTTP_CODE_OK) {
                // Get direct stream access
                WiFiClient* stream = http->getStreamPtr();

                if (stream) {
                    // Call the processor callback with the stream and shared document
                    success = jsonProcessor(stream, sharedJsonDoc);
                    if (success) {
                        LOG_DEBUG("[AlertsModule] HTTP streaming completed successfully");
                    } else {
                        LOG_ERROR("[AlertsModule] HTTP streaming processor failed");
                    }
                } else {
                    LOG_ERROR("[AlertsModule] Failed to get HTTP stream pointer");
                }
            } else {
                LOG_WARN("[AlertsModule] HTTP GET returned code %d", httpCode);
                // Don't retry for HTTP error codes (4xx, 5xx) - these are not transient
                break;
            }
        } else {
            LOG_ERROR("[AlertsModule] HTTP GET failed with code %d (attempt %d/%d)",
                     httpCode, attempt + 1, MAX_RETRIES + 1);
            // Continue to retry for connection errors (-1, etc.)
        }

        // Explicit cleanup (unique_ptr will handle it, but be explicit)
        http->end();
    }

    return success;
}


bool AlertsModule::alertExists(uint32_t id)
{
    for (auto &a : alerts) {
        if (a.id == id)
            return true;
    }
    return false;
}

bool AlertsModule::isAlertProcessed(uint32_t id)
{
    // Use the in-memory cache only - it's populated at startup from loadAlertsFromDisk()
    bool found = processedAlertIds.find(id) != processedAlertIds.end();
    if (found) {
        LOG_DEBUG("Alert ID 0x%x found in processed cache", id);
    } else {
        LOG_DEBUG("Alert ID 0x%x NOT in processed cache (cache size: %d)", id, processedAlertIds.size());
    }
    return found;
}

void AlertsModule::cacheProcessedAlertId(uint32_t id)
{
    if (id == 0) {
        LOG_DEBUG("Skipping cache for ID 0x0 (invalid)");
        return;
    }

    if (processedAlertIds.find(id) != processedAlertIds.end()) {
        // Keep most-recent usage order and persist current order
        for (auto it = processedAlertIdOrder.begin(); it != processedAlertIdOrder.end(); ++it) {
            if (*it == id) {
                processedAlertIdOrder.erase(it);
                break;
            }
        }
        processedAlertIdOrder.push_back(id);
        saveProcessedIdsToSingleFile();
        LOG_DEBUG("ID 0x%x already in cache, skipping", id);
        return;
    }

    if (processedAlertIds.size() >= MAX_PROCESSED_IDS_CACHE) {
        if (!processedAlertIdOrder.empty()) {
            uint32_t oldest = processedAlertIdOrder.front();
            processedAlertIdOrder.pop_front();
            processedAlertIds.erase(oldest);
            LOG_DEBUG("Cache full (%d), removed oldest ID: 0x%x", MAX_PROCESSED_IDS_CACHE, oldest);
        }
    }

    processedAlertIds.insert(id);
    processedAlertIdOrder.push_back(id);
    saveProcessedIdsToSingleFile();
    LOG_DEBUG("Added ID 0x%x to processed cache (cache now has %d items)", id, processedAlertIds.size());
}

void AlertsModule::removeProcessedAlertId(uint32_t id)
{
    processedAlertIds.erase(id);
    if (processedAlertIdOrder.empty()) {
        return;
    }

    for (auto it = processedAlertIdOrder.begin(); it != processedAlertIdOrder.end(); ++it) {
        if (*it == id) {
            processedAlertIdOrder.erase(it);
            break;
        }
    }

    saveProcessedIdsToSingleFile();
}

size_t AlertsModule::clampMessageToPayload(String &message, size_t maxPayloadBytes)
{
    if (maxPayloadBytes == 0) {
        message = "";
        return 0;
    }

    size_t msgBytes = utf8ByteLength(message);
    if (msgBytes <= maxPayloadBytes) {
        return msgBytes;
    }

    size_t bytesUsed = 0;
    size_t byteIndex = 0;
    size_t totalBytes = message.length();

    while (byteIndex < totalBytes) {
        uint8_t leadByte = static_cast<uint8_t>(message.charAt(byteIndex));
        size_t charLen = 1;

        if ((leadByte & 0x80) == 0x00) {
            charLen = 1;
        } else if ((leadByte & 0xE0) == 0xC0) {
            charLen = 2;
        } else if ((leadByte & 0xF0) == 0xE0) {
            charLen = 3;
        } else if ((leadByte & 0xF8) == 0xF0) {
            charLen = 4;
        } else {
            // Invalid UTF-8 lead byte; treat as single byte to keep behavior safe
            charLen = 1;
        }

        if (bytesUsed + charLen > maxPayloadBytes) {
            break;
        }

        bytesUsed += charLen;
        byteIndex += charLen;
    }

    message = message.substring(0, byteIndex);
    return bytesUsed;
}

void AlertsModule::upsertAlertInMemory(const Alert &alert)
{
    bool found = false;
    for (auto &existing : alerts) {
        if (existing.id == alert.id) {
            existing = alert;
            found = true;
            break;
        }
    }
    if (!found) {
        alerts.push_back(alert);
    }
}

Alert AlertsModule::toAlert(const AlertBinary &binAlert)
{
    auto safeString = [](const char *field, size_t fieldLen) -> String {
        size_t len = 0;
        while (len < fieldLen && field[len] != '\0') {
            ++len;
        }
        return String(field, len);
    };

    Alert a;
    a.id = binAlert.id;
    a.title = safeString(binAlert.title, sizeof(binAlert.title));
    a.link = "";
    a.valid_from = safeString(binAlert.valid_from, sizeof(binAlert.valid_from));
    a.valid_to = safeString(binAlert.valid_to, sizeof(binAlert.valid_to));
    a.location = safeString(binAlert.location, sizeof(binAlert.location));
    a.message = safeString(binAlert.message, sizeof(binAlert.message));
    a.source = safeString(binAlert.source, sizeof(binAlert.source));
    a.severity = binAlert.severity;
    a.addedAt = binAlert.addedAt;
    a.alert_type = a.title;
    a.lastSent = 0;

    if (binAlert.nextSendAt >= MIN_VALID_EPOCH) {
        a.nextSendAt = binAlert.nextSendAt;
    } else {
        a.nextSendAt = 0;
    }

    return a;
}

bool AlertsModule::fillAlertBinary(const Alert &alert, AlertBinary &binAlert)
{
    memset(&binAlert, 0, sizeof(AlertBinary));
    if (alert.id == 0) {
        LOG_ERROR("Cannot save alert with invalid ID");
        return false;
    }

    strncpy(binAlert.title, alert.title.c_str(), sizeof(binAlert.title) - 1);
    binAlert.title[sizeof(binAlert.title) - 1] = '\0';

    strncpy(binAlert.message, alert.message.c_str(), sizeof(binAlert.message) - 1);
    binAlert.message[sizeof(binAlert.message) - 1] = '\0';

    strncpy(binAlert.location, alert.location.c_str(), sizeof(binAlert.location) - 1);
    binAlert.location[sizeof(binAlert.location) - 1] = '\0';

    strncpy(binAlert.source, alert.source.c_str(), sizeof(binAlert.source) - 1);
    binAlert.source[sizeof(binAlert.source) - 1] = '\0';

    strncpy(binAlert.valid_from, alert.valid_from.c_str(), sizeof(binAlert.valid_from) - 1);
    binAlert.valid_from[sizeof(binAlert.valid_from) - 1] = '\0';

    strncpy(binAlert.valid_to, alert.valid_to.c_str(), sizeof(binAlert.valid_to) - 1);
    binAlert.valid_to[sizeof(binAlert.valid_to) - 1] = '\0';

    binAlert.severity = alert.severity;
    binAlert.addedAt = alert.addedAt;
    binAlert.lastSent = alert.lastSent;
    binAlert.nextSendAt = alert.nextSendAt;
    binAlert.id = alert.id;

    return true;
}


bool AlertsModule::saveAlertToDisk(const Alert &alert)
{
    return saveAlertToFile(alert, alert.id);
}

bool AlertsModule::hasEnoughFreeSpace()
{
    // Check filesystem free space
    size_t totalBytes = 0;
    size_t usedBytes = 0;

#ifdef FSCom
    totalBytes = FSCom.totalBytes();
    usedBytes = FSCom.usedBytes();
#endif

    if (totalBytes == 0) {
        // Can't determine, assume OK
        return true;
    }

    size_t freeBytes = (totalBytes > usedBytes) ? (totalBytes - usedBytes) : 0;
    bool hasSpace = freeBytes >= MIN_FREE_SPACE_BYTES;

    if (!hasSpace) {
        LOG_WARN("Low filesystem space: %d/%d bytes used, need %d free",
                 usedBytes, totalBytes, MIN_FREE_SPACE_BYTES);
    }

    return hasSpace;
}

bool AlertsModule::cleanupTempFilesFromAlertsDir()
{
    uint32_t startMs = millis();
    bool removed = false;
    concurrency::LockGuard g(spiLock);

    FSCom.mkdir(ALERTS_DIR);
    File root = FSCom.open(ALERTS_DIR, FILE_O_READ);
    if (!root || !root.isDirectory()) {
        if (root) root.close();
        return false;
    }

    int filesScanned = 0;
    File file = root.openNextFile();
    while (file) {
        filesScanned++;
        if (!file.isDirectory()) {
            String filename = file.name();
            if (filename.endsWith(".tmp")) {
                String fullPath = String(ALERTS_DIR) + "/" + filename;
                file.close();
                if (FSCom.remove(fullPath.c_str())) {
                    removed = true;
                }
                file = root.openNextFile();
                continue;
            }
        }

        if (filesScanned % 20 == 0) {
            feedWatchdog();
        }
        file.close();
        file = root.openNextFile();
    }
    root.close();

    if (removed) {
        LOG_INFO("Removed stale temporary alert files in %s", ALERTS_DIR);
    }

    uint32_t elapsedMs = millis() - startMs;
    if (elapsedMs > 100) {
        LOG_WARN("cleanupTempFilesFromAlertsDir took %lu ms", elapsedMs);
    }
    return removed;
}

bool AlertsModule::saveAlertsToSingleFile()
{
    uint32_t startMs = millis();
    FSCom.mkdir(ALERTS_DIR);
    concurrency::LockGuard g(spiLock);
    feedWatchdog();

    File f = FSCom.open(ALERTS_DATA_FILE_TMP, FILE_O_WRITE);
    if (!f) {
        LOG_ERROR("Failed to open temp alert data file for writing: %s", ALERTS_DATA_FILE_TMP);
        return false;
    }

    AlertStorageHeader header = {};
    header.magic = ALERTS_STORAGE_MAGIC;
    header.version = ALERTS_STORAGE_VERSION;
    header.reserved = 0;
    header.alertCount = alerts.size();

    size_t writtenHeader = f.write((const uint8_t*)&header, sizeof(header));
    if (writtenHeader != sizeof(header)) {
        LOG_ERROR("Failed to write alert data header (%d bytes)", writtenHeader);
        f.close();
        FSCom.remove(ALERTS_DATA_FILE_TMP);
        return false;
    }

    AlertBinary *binAlert = new AlertBinary;
    if (!binAlert) {
        LOG_ERROR("Failed to allocate AlertBinary for save");
        f.close();
        FSCom.remove(ALERTS_DATA_FILE_TMP);
        return false;
    }

    for (size_t i = 0; i < alerts.size(); i++) {
        const auto &alert = alerts[i];
        if (!fillAlertBinary(alert, *binAlert)) {
            delete binAlert;
            f.close();
            FSCom.remove(ALERTS_DATA_FILE_TMP);
            return false;
        }

        size_t written = f.write((const uint8_t*)binAlert, sizeof(AlertBinary));
        if (written != sizeof(AlertBinary)) {
            LOG_ERROR("Failed to write alert record for id 0x%x", alert.id);
            delete binAlert;
            f.close();
            FSCom.remove(ALERTS_DATA_FILE_TMP);
            return false;
        }

        if ((i + 1) % 16 == 0) {
            feedWatchdog();
        }
    }
    delete binAlert;

    f.flush();
    f.close();

    if (!FSCom.rename(ALERTS_DATA_FILE_TMP, ALERTS_DATA_FILE)) {
        LOG_ERROR("Failed to commit alert data temp file");
        FSCom.remove(ALERTS_DATA_FILE_TMP);
        return false;
    }

    uint32_t elapsedMs = millis() - startMs;
    if (elapsedMs > 100) {
        LOG_WARN("saveAlertsToSingleFile took %lu ms for %d alerts", elapsedMs, alerts.size());
    }

    LOG_DEBUG("Saved %d alerts to %s", alerts.size(), ALERTS_DATA_FILE);
    return true;
}

bool AlertsModule::saveProcessedIdsToSingleFile()
{
    uint32_t startMs = millis();
    FSCom.mkdir(ALERTS_DIR);
    concurrency::LockGuard g(spiLock);
    feedWatchdog();

    File f = FSCom.open(PROCESSED_IDS_FILE_TMP, FILE_O_WRITE);
    if (!f) {
        LOG_ERROR("Failed to open temp processed IDs file for writing: %s", PROCESSED_IDS_FILE_TMP);
        return false;
    }

    ProcessedRefsHeader header = {};
    header.magic = PROCESSED_IDS_MAGIC;
    header.version = PROCESSED_IDS_VERSION;
    header.reserved = 0;
    header.refCount = processedAlertIdOrder.size();

    size_t writtenHeader = f.write((const uint8_t*)&header, sizeof(header));
    if (writtenHeader != sizeof(header)) {
        LOG_ERROR("Failed to write processed IDs header (%d bytes)", writtenHeader);
        f.close();
        FSCom.remove(PROCESSED_IDS_FILE_TMP);
        return false;
    }

    size_t index = 0;
    for (const auto id : processedAlertIdOrder) {
        ProcessedRefRecord rec;
        rec.id = id;
        rec.seenAt = getTime(false);
        size_t written = f.write((const uint8_t*)&rec, sizeof(ProcessedRefRecord));
        if (written != sizeof(ProcessedRefRecord)) {
            LOG_ERROR("Failed to write processed ID record 0x%x", id);
            f.close();
            FSCom.remove(PROCESSED_IDS_FILE_TMP);
            return false;
        }

        if (++index % 16 == 0) {
            feedWatchdog();
        }
    }

    f.flush();
    f.close();

    if (!FSCom.rename(PROCESSED_IDS_FILE_TMP, PROCESSED_IDS_FILE)) {
        LOG_ERROR("Failed to commit processed IDs temp file");
        FSCom.remove(PROCESSED_IDS_FILE_TMP);
        return false;
    }

    uint32_t elapsedMs = millis() - startMs;
    if (elapsedMs > 100) {
        LOG_WARN("saveProcessedIdsToSingleFile took %lu ms for %d ids", elapsedMs, processedAlertIdOrder.size());
    }

    LOG_DEBUG("Saved %d processed IDs to %s", processedAlertIdOrder.size(), PROCESSED_IDS_FILE);
    return true;
}

bool AlertsModule::loadAlertsFromSingleFile()
{
    concurrency::LockGuard g(spiLock);

    File f = FSCom.open(ALERTS_DATA_FILE, FILE_O_READ);
    if (!f) {
        return false;
    }
    if (!f.isDirectory()) {
        AlertStorageHeader header;
        if (f.size() < (long)sizeof(AlertStorageHeader)) {
            LOG_WARN("Alert storage file too small: %s", ALERTS_DATA_FILE);
            f.close();
            FSCom.remove(ALERTS_DATA_FILE);
            return false;
        }

        size_t headerRead = f.read((uint8_t*)&header, sizeof(AlertStorageHeader));
        if (headerRead != sizeof(AlertStorageHeader) ||
            header.magic != ALERTS_STORAGE_MAGIC ||
            header.version != ALERTS_STORAGE_VERSION) {
            LOG_WARN("Invalid alert storage header in %s", ALERTS_DATA_FILE);
            f.close();
            FSCom.remove(ALERTS_DATA_FILE);
            return false;
        }

        if (f.size() != (long)(sizeof(AlertStorageHeader) + header.alertCount * sizeof(AlertBinary))) {
            LOG_WARN("Alert storage size mismatch in %s", ALERTS_DATA_FILE);
            f.close();
            FSCom.remove(ALERTS_DATA_FILE);
            return false;
        }

        uint32_t count = header.alertCount;
        AlertBinary *binAlert = new AlertBinary;
        if (!binAlert) {
            LOG_ERROR("Failed to allocate AlertBinary for load");
            f.close();
            return false;
        }
        for (uint32_t i = 0; i < count; i++) {
            size_t bytesRead = f.read((uint8_t*)binAlert, sizeof(AlertBinary));
            if (bytesRead != sizeof(AlertBinary) || binAlert->id == 0) {
                LOG_WARN("Invalid alert record in %s", ALERTS_DATA_FILE);
                delete binAlert;
                f.close();
                FSCom.remove(ALERTS_DATA_FILE);
                return false;
            }

            Alert a = toAlert(*binAlert);
            if (isAlertValid(a)) {
                upsertAlertInMemory(a);
            }
            // Even if alert is expired, keep ID for duplicate suppression
            if (processedAlertIds.find(binAlert->id) == processedAlertIds.end()) {
                processedAlertIds.insert(binAlert->id);
                processedAlertIdOrder.push_back(binAlert->id);
            }
        }
        delete binAlert;
        while (processedAlertIdOrder.size() > MAX_PROCESSED_IDS_CACHE) {
            uint32_t oldestId = processedAlertIdOrder.front();
            processedAlertIdOrder.pop_front();
            processedAlertIds.erase(oldestId);
        }
    } else {
        f.close();
        return false;
    }

    f.close();
    return true;
}

bool AlertsModule::loadProcessedIdsFromSingleFile()
{
    concurrency::LockGuard g(spiLock);

    File f = FSCom.open(PROCESSED_IDS_FILE, FILE_O_READ);
    if (!f) {
        return false;
    }

    ProcessedRefsHeader header;
    if (f.size() < (long)sizeof(ProcessedRefsHeader)) {
        LOG_WARN("Processed IDs file too small: %s", PROCESSED_IDS_FILE);
        f.close();
        FSCom.remove(PROCESSED_IDS_FILE);
        return false;
    }

    size_t headerRead = f.read((uint8_t*)&header, sizeof(ProcessedRefsHeader));
    if (headerRead != sizeof(ProcessedRefsHeader) ||
        header.magic != PROCESSED_IDS_MAGIC ||
        header.version != PROCESSED_IDS_VERSION) {
        LOG_WARN("Invalid processed IDs header in %s", PROCESSED_IDS_FILE);
        f.close();
        FSCom.remove(PROCESSED_IDS_FILE);
        return false;
    }

    if (f.size() != (long)(sizeof(ProcessedRefsHeader) + header.refCount * sizeof(ProcessedRefRecord))) {
        LOG_WARN("Processed IDs size mismatch in %s", PROCESSED_IDS_FILE);
        f.close();
        FSCom.remove(PROCESSED_IDS_FILE);
        return false;
    }

    for (uint32_t i = 0; i < header.refCount; i++) {
        ProcessedRefRecord rec;
        size_t bytesRead = f.read((uint8_t*)&rec, sizeof(ProcessedRefRecord));
        if (bytesRead != sizeof(ProcessedRefRecord) || rec.id == 0) {
            LOG_WARN("Invalid processed ID record in %s", PROCESSED_IDS_FILE);
            f.close();
            FSCom.remove(PROCESSED_IDS_FILE);
            return false;
        }

        if (processedAlertIds.find(rec.id) == processedAlertIds.end()) {
            processedAlertIds.insert(rec.id);
            processedAlertIdOrder.push_back(rec.id);
        }
    }

    f.close();

    while (processedAlertIdOrder.size() > MAX_PROCESSED_IDS_CACHE) {
        processedAlertIdOrder.pop_front();
    }
    // Rebuild set to match order after trimming
    std::unordered_set<uint32_t> trimmedSet;
    for (const auto id : processedAlertIdOrder) {
        trimmedSet.insert(id);
    }
    processedAlertIds.swap(trimmedSet);

    return true;
}

bool AlertsModule::loadLegacyAlertFiles()
{
    bool migrated = false;
    concurrency::LockGuard g(spiLock);

    FSCom.mkdir(ALERTS_DIR);
    File root = FSCom.open(ALERTS_DIR, FILE_O_READ);
    if (!root || !root.isDirectory()) {
        if (root) root.close();
        return false;
    }

    std::vector<String> legacyFiles;
    int filesProcessed = 0;
    AlertBinary *binAlert = new AlertBinary;
    if (!binAlert) {
        root.close();
        return false;
    }
    File file = root.openNextFile();
    while (file) {
        filesProcessed++;
        if (filesProcessed % 10 == 0) {
            feedWatchdog();
        }

        if (!file.isDirectory()) {
            String filename = file.name();
            bool isKnownContainer = filename == "alerts.bin" || filename == "alerts.bin.tmp" ||
                                   filename == "processed_ids.bin" || filename == "processed_ids.bin.tmp";
            if (!isKnownContainer && filename.endsWith(".bin") && !filename.endsWith(".tmp")) {
                size_t bytesRead = file.read((uint8_t*)binAlert, sizeof(AlertBinary));
                file.close();

                if (bytesRead == sizeof(AlertBinary) && binAlert->id > 0) {
                    Alert a = toAlert(*binAlert);
                    upsertAlertInMemory(a);

                    if (processedAlertIds.find(a.id) == processedAlertIds.end()) {
                        processedAlertIds.insert(a.id);
                        processedAlertIdOrder.push_back(a.id);
                    }
                    legacyFiles.push_back(filename);
                    migrated = true;
                    LOG_DEBUG("Migrating legacy alert file: %s", filename.c_str());
                } else {
                    if (FSCom.remove((String(ALERTS_DIR) + "/" + filename).c_str())) {
                        LOG_WARN("Deleted invalid legacy file: %s", filename.c_str());
                    }
                }
                file = root.openNextFile();
                continue;
            }
        }
        file.close();
        file = root.openNextFile();
    }
    root.close();
    delete binAlert;

    if (!migrated) {
        return false;
    }

    for (const auto &filename : legacyFiles) {
        String fullPath = String(ALERTS_DIR) + "/" + filename;
        if (FSCom.remove(fullPath.c_str())) {
            LOG_DEBUG("Deleted legacy file after migration: %s", fullPath.c_str());
        } else {
            LOG_WARN("Failed to remove legacy file: %s", fullPath.c_str());
        }
    }

    while (processedAlertIdOrder.size() > MAX_PROCESSED_IDS_CACHE) {
        uint32_t oldestId = processedAlertIdOrder.front();
        processedAlertIdOrder.pop_front();
        processedAlertIds.erase(oldestId);
    }

    return migrated;
}

void AlertsModule::enforceFileLimits()
{
    if ((int)alerts.size() <= MAX_FILES_ON_DISK) {
        return;  // Under limit, nothing to do
    }

    size_t targetCount = MAX_FILES_ON_DISK;
    std::sort(alerts.begin(), alerts.end(), [](const Alert &a, const Alert &b) {
        return a.addedAt < b.addedAt;
    });
    alerts.erase(alerts.begin(), alerts.begin() + (alerts.size() - targetCount));
    saveAlertsToSingleFile();
}

bool AlertsModule::saveAlertToFile(const Alert &alert, uint32_t id, const String &dateStr)
{
    (void)dateStr;
    // Check filesystem space before attempting to save
    if (!hasEnoughFreeSpace()) {
        LOG_WARN("Insufficient filesystem space, running cleanup before save");
        enforceFileLimits();

        // Check again after cleanup
        if (!hasEnoughFreeSpace()) {
            LOG_ERROR("Cannot save alert: filesystem still full after cleanup");
            return false;
        }
    }

    if (id == 0) {
        LOG_ERROR("Cannot save alert with invalid ID");
        return false;
    }

    upsertAlertInMemory(alert);

    if ((int)alerts.size() > MAX_FILES_ON_DISK) {
        enforceFileLimits();
    }

    if (!saveAlertsToSingleFile()) {
        LOG_ERROR("Failed to persist alerts after processing id 0x%x", id);
        return false;
    }

    cacheProcessedAlertId(id);
    LOG_DEBUG("Persisted alert id 0x%x into single-file storage", id);

    return true;
}

bool AlertsModule::loadAlertsFromDisk()
{
    LOG_DEBUG("Loading alerts from disk (memory-optimized)");
    alerts.clear();
    processedAlertIds.clear();
    processedAlertIdOrder.clear();
    bool tmpCleaned = cleanupTempFilesFromAlertsDir();
    if (tmpCleaned) {
        LOG_INFO("Cleaned up stale temporary alert files during boot");
    }

    bool loadedProcessedIds = loadProcessedIdsFromSingleFile();
    bool loadedAlerts = loadAlertsFromSingleFile();

    if (!loadedAlerts) {
        // No consolidated alerts file loaded. Try legacy migration.
        alerts.clear();
        if (!loadedProcessedIds) {
            processedAlertIds.clear();
            processedAlertIdOrder.clear();
        }
        if (loadLegacyAlertFiles()) {
            if (!saveAlertsToSingleFile()) {
                LOG_ERROR("Failed to persist migrated alerts");
            }
            if (!saveProcessedIdsToSingleFile()) {
                LOG_WARN("Failed to persist migrated processed IDs");
            }
            loadedAlerts = true;
        }
    }

    if ((int)alerts.size() > MAX_FILES_ON_DISK) {
        enforceFileLimits();
    }

    if ((int)alerts.size() > MAX_ALERTS_IN_MEMORY) {
        alerts.resize(MAX_ALERTS_IN_MEMORY);
    }

    LOG_INFO("Loaded %d valid alerts from disk (processed IDs: %d)",
             alerts.size(), processedAlertIds.size());
    return loadedAlerts || loadedProcessedIds;
}



// ========== Alert Processing Functions ==========

const char* AlertsModule::stateName(ModuleState state)
{
    switch (state) {
        case ModuleState::INIT: return "INIT";
        case ModuleState::IDLE: return "IDLE";
        case ModuleState::FETCHING_PAGE: return "FETCHING_PAGE";
        case ModuleState::SAVING_ALERT: return "SAVING_ALERT";
        case ModuleState::SENDING_ALERT: return "SENDING_ALERT";
        case ModuleState::FETCHING_DYNAMIC: return "FETCHING_DYNAMIC";
        case ModuleState::CALLING_AI: return "CALLING_AI";
        default: return "UNKNOWN";
    }
}

void AlertsModule::transitionToState(ModuleState nextState, const char *reason)
{
    if (currentState != nextState) {
        LOG_INFO("State transition: %s -> %s (%s)", stateName(currentState), stateName(nextState), reason);
    }
    currentState = nextState;
}

int32_t AlertsModule::runOnce()
{
    unsigned long currentMillis = millis();
    uint32_t currentTime = getTime(false);

    // Periodic memory usage monitoring
    if (currentMillis - lastMemoryCheckTime > MEMORY_CHECK_INTERVAL_MS) {
        size_t currentMemoryUsage = alerts.size() * sizeof(Alert);
        if (currentMemoryUsage != lastReportedMemoryUsage ||
            alerts.size() >= MAX_ALERTS_IN_MEMORY * 0.8) { // Report when >80% of limit
            LOG_INFO("Memory usage - %d alerts (%d bytes), limit: %d alerts",
                     alerts.size(), currentMemoryUsage, MAX_ALERTS_IN_MEMORY);
            lastReportedMemoryUsage = currentMemoryUsage;
        }

        if (alerts.size() >= MAX_ALERTS_IN_MEMORY) {
            LOG_WARN("Alert count at memory limit (%d), consider cleanup", MAX_ALERTS_IN_MEMORY);
        }

        lastMemoryCheckTime = currentMillis;

        return ALERT_PROCESSING_YIELD_MS;
    }

    // State machine for non-blocking operation
    switch (currentState) {
        case ModuleState::INIT: {
            // Initialization state - run once
            LOG_INFO("Quick initialization - minimal operations");
            
            // Purge all alerts on boot if configured
            if (PURGE_ALERTS_ON_BOOT) {
                LOG_INFO("Purging all alerts on boot (PURGE_ALERTS_ON_BOOT=true)");
                purgeAllAlerts();
                alerts.clear(); // Clear in-memory cache too
            } else {
                // Load existing alerts from disk
                LOG_INFO("Loading existing alerts from storage");
                loadAlertsFromDisk();
            }
            
            initializationDone = true;
            transitionToState(ModuleState::IDLE, "initialization complete");
            LOG_INFO("Initialization complete - system responsive");
            return ALERT_PROCESSING_YIELD_MS; // Quick return to continue
        }
        
        case ModuleState::IDLE: {
            // Normal operation - check what needs to be done
            
            // Priority 1: Check for alerts that need re-sending (works without WiFi)
            // Only send if time is synced (to check dates) and radio is available
            if (currentTime > 0 && currentTime >= MIN_VALID_EPOCH) {
                // Time is synced, we can check and resend existing alerts
                // Limit processing to avoid blocking - only check a few alerts per cycle
                static size_t lastCheckedIndex = 0;
                size_t alertsCheckedThisCycle = 0;
                const size_t MAX_CHECKS_PER_CYCLE = 1;

                // Check if it's time to log pending alerts (every 3 minutes)
                bool shouldLogPending = (currentMillis - lastPendingAlertLogTime >= PENDING_ALERT_LOG_INTERVAL_MS);

                for (size_t i = 0; i < alerts.size() && alertsCheckedThisCycle < MAX_CHECKS_PER_CYCLE; i++) {
                    size_t checkIndex = (lastCheckedIndex + i) % alerts.size();
                    const Alert alert = alerts[checkIndex];

                    if (!isAlertValid(alert)) {
                        continue;
                    }

                    alertsCheckedThisCycle++;

                    // Check if it's time to send based on pre-calculated nextSendAt
                    // nextSendAt is stored as absolute Unix timestamp
                    if (currentTime >= alert.nextSendAt) {
                        // Time to re-send this alert (mesh only, no WiFi needed)
                        if (sendAlertToMesh(alert)) {
                            alerts[checkIndex].lastSent = currentMillis;
                            // Calculate next send time based on severity
                            unsigned long interval = getSendInterval(alert.severity);
                            // Store absolute time instead of relative time from boot
                            if (currentTime > 0) {
                                alerts[checkIndex].nextSendAt = currentTime + interval;
                            } else {
                                // Fallback if time not synced (shouldn't happen in resend logic)
                                alerts[checkIndex].nextSendAt = currentTime + interval;
                            }
                            saveAlertToDisk(alerts[checkIndex]);
                            LOG_INFO("Re-sent alert [%s, sev:%d]: %s (next in %lu min)",
                                     alert.source.c_str(), alert.severity, alert.title.c_str(), interval / 60);
                        } else {
                            // Failed to send - set retry delay to avoid tight loop
                            alerts[checkIndex].nextSendAt = currentTime + 60;
                        }
                        // Only resend one alert per cycle to avoid blocking
                        lastCheckedIndex = (checkIndex + 1) % alerts.size(); // Resume after this alert
                        return RESEND_CHECK_YIELD_MS;
                    } else {
                        // Alert is pending but not yet due
                        // Log all pending alerts if it's time to log (every 3 minutes)
                        if (shouldLogPending) {
                            unsigned long remainingSec = alert.nextSendAt - currentTime;
                            if (remainingSec > 120) {
                                unsigned long remainingMin = remainingSec / 60;
                                LOG_DEBUG("Alert pending (will send in %lu min, source: %s, severity: %d): %s",
                                          remainingMin, alert.source.c_str(), alert.severity, alert.title.c_str());
                            } else {
                                LOG_DEBUG("Alert pending (will send in %lu sec, source: %s, severity: %d): %s",
                                          remainingSec, alert.source.c_str(), alert.severity, alert.title.c_str());
                            }
                        }
                    }
                }

                // Update the logging timestamp after processing all alerts in this cycle
                if (shouldLogPending) {
                    lastPendingAlertLogTime = currentMillis;
                }

                // Update starting index for next cycle (guard against division by zero)
                if (!alerts.empty()) {
                    lastCheckedIndex = (lastCheckedIndex + alertsCheckedThisCycle) % alerts.size();
                }
            }
            
            // Priority 2: Process ONE alert at a time (fetch content + AI processing) to prevent watchdog timeout
            if (!pendingAlerts.empty() && !processingCtx.active) {
                // Only process one alert per runOnce cycle to stay within watchdog timeout
                static uint32_t lastAlertProcessingTime = 0;

                // Check if we processed an alert recently (throttle to prevent overwhelming the system)
                if (currentMillis - lastAlertProcessingTime < ALERT_PROCESSING_THROTTLE_MS) {
                    // Too soon since last processing, skip for now
                    return ALERT_PROCESSING_YIELD_MS;
                }

                // Find first alert that needs full content fetch
                for (auto& pending : pendingAlerts) {
                    if (pending.needsFullFetch) {
                        LOG_INFO("Fetching full content for alert: %s (queue: %d)",
                                 pending.rawAlert.title.c_str(), pendingAlerts.size());

                        // Reset watchdog before starting HTTP operation
                        feedWatchdog();

                        // Create HTTP GET callback
                        auto httpGetCallback = [this](const char* url, int& httpCode) -> String {
                            return httpGet(url, httpCode);
                        };

                        // Fetch full content for this one alert
                        AlertSource::RawAlert fullAlert = pending.source->fetchFullAlertContent(
                            pending.rawAlert, httpGetCallback);
                        pending.rawAlert = fullAlert;
                        pending.needsFullFetch = false;

                        LOG_DEBUG("Full content fetched for: %s", fullAlert.title.c_str());

                        // Reset watchdog after HTTP operation
                        feedWatchdog();

                        // Update processing timestamp
                        lastAlertProcessingTime = currentMillis;

                        // Return to main loop immediately after content fetch
                        // This ensures we don't start AI processing in the same cycle
                        return ALERT_PROCESSING_YIELD_MS;
                    }
                }

                // All content fetched for first pending alert, start AI processing for it
                PendingAlert pending = pendingAlerts.front();
                pendingAlerts.erase(pendingAlerts.begin());

                processingCtx.active = true;
                processingCtx.source = pending.source;
                processingCtx.rawAlert = pending.rawAlert;
                processingCtx.stateStartTime = currentMillis;

                LOG_INFO("Starting AI processing for alert from [%s]: %s (queue: %d remaining)",
                         pending.source->getSourceId().c_str(), pending.rawAlert.title.c_str(), pendingAlerts.size());

                // Update processing timestamp
                lastAlertProcessingTime = currentMillis;

                // Proceed to AI extraction
                transitionToState(ModuleState::CALLING_AI, "pending alert ready for AI processing");
                return ALERT_PROCESSING_YIELD_MS; // Yield before starting AI request
            }
            
            // From here on, both WiFi connection AND time sync are required for fetching
            // Check WiFi is not just enabled, but actually connected with an IP address
            bool wifiConnected = false;
#if HAS_WIFI && !defined(ARCH_PORTDUINO)
            wifiConnected = (WiFi.status() == WL_CONNECTED) && (WiFi.localIP() != IPAddress(0, 0, 0, 0));
#endif
            
            if (!wifiConnected) {
                LOG_DEBUG("WiFi not connected (status=%d), waiting 60s (mesh resends still active)",
#if HAS_WIFI && !defined(ARCH_PORTDUINO)
                         WiFi.status()
#else
                         0
#endif
                         );
                return MAX_RUNONCE_INTERVAL_MS;
            }
            
            // Also check if time is synced before fetching (need valid dates)
            if (currentTime == 0 || currentTime < MIN_VALID_EPOCH) {
                LOG_DEBUG("Time not synced yet (now=%lu), waiting 60s", currentTime);
                return MAX_RUNONCE_INTERVAL_MS;
            }
            
            // Priority 3: Check all sources for fetching (round-robin, each has its own interval)
            // NOTE: This is AFTER pending alerts are processed, so we send before fetching new ones
            for (int i = 0; i < numSources; i++) {
                // Check if it's time to fetch from this source
                unsigned long lastFetch = sourceLastFetchTime[i];
                unsigned long fetchInterval = sources[i]->getFetchIntervalMs();
                
                // On first run (lastFetch == 0), fetch immediately when WiFi+time are ready
                if (lastFetch == 0 || (currentMillis - lastFetch) >= fetchInterval) {
                    currentSourceIndex = i;
                    if (lastFetch == 0) {
                        LOG_INFO("Initial fetch for source %s - WiFi connected and time synced", 
                                 sources[i]->getSourceId().c_str());
                    } else {
                        LOG_INFO("Fetch interval elapsed for source %s (%lu min), fetching new alerts", 
                                 sources[i]->getSourceId().c_str(), fetchInterval / 60000);
                    }
                    transitionToState(ModuleState::FETCHING_PAGE, "source fetch interval elapsed");
                    return ALERT_PROCESSING_YIELD_MS;
                }
            }
            
            // Priority 4: Check dynamic sources for fetching (periodic data like weather)
            for (int i = 0; i < numDynamicSources; i++) {
                unsigned long lastFetch = dynamicSourceLastFetchTime[i];
                unsigned long fetchInterval = dynamicSources[i]->getFetchIntervalMs();

                // On first run (lastFetch == 0), fetch immediately when WiFi is ready
                if (lastFetch == 0 || (currentMillis - lastFetch) >= fetchInterval) {
                    currentDynamicSourceIndex = i;
                    if (lastFetch == 0) {
                        LOG_INFO("Initial fetch for dynamic source %s",
                                 dynamicSources[i]->getSourceId().c_str());
                    } else {
                        LOG_INFO("Fetch interval elapsed for dynamic source %s (%lu min)",
                                 dynamicSources[i]->getSourceId().c_str(), fetchInterval / 60000);
                    }
                    transitionToState(ModuleState::FETCHING_DYNAMIC, "dynamic source fetch interval elapsed");
                    return ALERT_PROCESSING_YIELD_MS;
                }
            }

            // Priority 5: Run cleanup if needed
            if (lastCleanupTime == 0 || (currentMillis - lastCleanupTime) > CLEANUP_INTERVAL_MS) {
                LOG_DEBUG("Running cleanup");
                cleanupOldAlerts();
                lastCleanupTime = currentMillis;
                return ALERT_PROCESSING_YIELD_MS;
            }
            
            // Nothing to do, calculate next wake time but cap at 1 minute for responsive checks
            unsigned long minInterval = intervalMs;
            
            // Check if any alerts need resending soon (sample a few alerts to avoid blocking)
            // This is an optimization - we don't need to check ALL alerts for wake time calculation
            const size_t SAMPLE_SIZE = 10; // Check up to 10 alerts for wake time calculation
            size_t checked = 0;
            for (const auto &alert : alerts) {
                if (checked >= SAMPLE_SIZE) break;
                if (isAlertValid(alert) && alert.lastSent > 0) {
                    unsigned long alertInterval = getSendInterval(alert.severity);
                    unsigned long elapsed = currentMillis - alert.lastSent;
                    if (elapsed < alertInterval * 1000) {  // Convert alertInterval from seconds to milliseconds for comparison
                        unsigned long timeUntilNext = (alertInterval * 1000) - elapsed;
                        if (timeUntilNext < minInterval) {
                            minInterval = timeUntilNext;
                        }
                    }
                }
                checked++;
            }

            // Cap at 1 minute for responsive operation
            if (minInterval > MAX_RUNONCE_INTERVAL_MS) {
                minInterval = MAX_RUNONCE_INTERVAL_MS;
            }

            LOG_DEBUG("Idle, next check in %lu seconds", minInterval / 1000);

            // Power optimization: allow CPU to idle for longer waits
            unsigned long returnInterval = minInterval > 0 ? minInterval : MAX_RUNONCE_INTERVAL_MS;

#ifdef ARCH_ESP32
            // For very long waits (>30 seconds), allow deeper CPU idle to reduce power
            if (returnInterval > 30000) {
                // ESP32 can enter light sleep or just yield more aggressively
                // The OSThread framework will handle the timing
                LOG_DEBUG("Long idle period (%lu sec), optimizing for power", returnInterval / 1000);
            }
#endif

            // Check if we should broadcast channel information
            if (shouldBroadcastInfo() && currentMillis >= nextBroadcastTimeMs) {
                LOG_DEBUG("[AlertsModule] Time to broadcast channel information");

                if (broadcastInfoMessage()) {
                    // Schedule next broadcast
                    nextBroadcastTimeMs = currentMillis + BROADCAST_INTERVAL_MS;
                    LOG_INFO("[AlertsModule] Channel info broadcast successful, next broadcast in %lu minutes",
                             BROADCAST_INTERVAL_MS / 60000);
                } else {
                    // Failed to broadcast, retry in 1 minute
                    nextBroadcastTimeMs = currentMillis + 1 * 60 * 1000;
                    LOG_WARN("[AlertsModule] Channel info broadcast failed, retrying in 1 minute");
                }

                // Don't return immediately - allow other processing to continue
            }

            return returnInterval;
        }
        
        case ModuleState::FETCHING_PAGE: {
            LOG_INFO("Fetching alerts from source [%s]...", sources[currentSourceIndex]->getSourceId().c_str());

            // Create HTTP GET callback for the source to use
            auto httpGetCallback = [this](const char* url, int& httpCode) -> String {
                return httpGet(url, httpCode);
            };

            // Clear shared JSON document for this source
            sharedJsonDoc.clear();

            // Call source plugin to fetch and parse alerts (first pass - minimal data)
            std::vector<AlertSource::RawAlert> rawAlerts = sources[currentSourceIndex]->fetchAndParseAlerts(httpGetCallback);

            // Update last fetch time for this source
            sourceLastFetchTime[currentSourceIndex] = currentMillis;

            if (rawAlerts.empty()) {
                LOG_INFO("No new alerts from source %s", sources[currentSourceIndex]->getSourceId().c_str());
                transitionToState(ModuleState::IDLE, "no new alerts from source");
                return ALERT_PROCESSING_YIELD_MS;
            }

            LOG_INFO("Found %d new alerts from source %s",
                     rawAlerts.size(), sources[currentSourceIndex]->getSourceId().c_str());

            uint32_t nextSendAtTime = getTime(false);

            // Queue raw alerts for full content fetching (without fetching content yet)
            // Content will be fetched one-at-a-time in FETCHING_ARTICLE state
            int queuedCount = 0;
            for (const auto& rawAlert : rawAlerts) {
                // Check if this alert has already been processed
                if (isAlertProcessed(rawAlert.id)) {
                    // Look up the existing alert to check if it's expired
                    bool found = false;
                    for (const auto& existingAlert : alerts) {
                        if (existingAlert.id == rawAlert.id) {
                            if (isAlertValid(existingAlert)) {
                                LOG_DEBUG("Alert already processed and still valid (ID: 0x%x), skipping: %s", 
                                         rawAlert.id, rawAlert.title.c_str());
                            } else {
                                LOG_DEBUG("Alert already processed but expired (ID: 0x%x), skipping: %s (expired %s)", 
                                         rawAlert.id, rawAlert.title.c_str(), existingAlert.valid_to.c_str());
                            }
                            found = true;
                            break;
                        }
                    }
                    
                    // If not found in alerts vector, it's in the processed cache but not in memory
                    // (probably expired and pruned). Log this too.
                    if (!found) {
                        LOG_DEBUG("Alert already processed and likely expired (ID: 0x%x), skipping: %s", 
                                 rawAlert.id, rawAlert.title.c_str());
                    }
                    continue;
                }

                // Check if already in pending queue
                bool alreadyPending = false;
                for (const auto &pending : pendingAlerts) {
                    if (pending.rawAlert.id == rawAlert.id) {
                        alreadyPending = true;
                        break;
                    }
                }

                if (!alreadyPending) {
                    // For sources with structured dates, check if alert is already expired
                    // This saves AI tokens by not processing already-expired alerts
                    if (rawAlert.structuredEndDate.length() > 0) {
                        time_t endTime = parseDateString(rawAlert.structuredEndDate);
                        time_t now = time(nullptr);
                        if (endTime > 0 && now >= MIN_VALID_EPOCH && now > endTime) {
                            LOG_DEBUG("Alert already expired (end: %s), marking as processed: %s",
                                     rawAlert.structuredEndDate.c_str(), rawAlert.title.c_str());
                            // Save empty marker to prevent re-processing
                            Alert expiredMarker;
                            expiredMarker.id = rawAlert.id;
                            expiredMarker.title = rawAlert.title;
                            expiredMarker.valid_from = rawAlert.structuredStartDate;
                            expiredMarker.valid_to = rawAlert.structuredEndDate;
                            expiredMarker.source = sources[currentSourceIndex]->getSourceId();
                            expiredMarker.message = "";
                            expiredMarker.location = "";
                            expiredMarker.severity = 10;
                            expiredMarker.addedAt = now;
                            expiredMarker.lastSent = 0;
                            expiredMarker.nextSendAt = nextSendAtTime;
                            saveAlertToDisk(expiredMarker);
                            nextSendAtTime += 10;
                            continue;
                        }
                    }

                    // Queue alert for full content fetching - don't fetch here to avoid watchdog
                    // Check bounds to prevent memory exhaustion
                    if (pendingAlerts.size() >= MAX_PENDING_ALERTS) {
                        LOG_WARN("Pending alerts queue at limit (%d), skipping alert: %s",
                                MAX_PENDING_ALERTS, rawAlert.title.c_str());
                        continue;
                    }

                    PendingAlert pending;
                    pending.source = sources[currentSourceIndex];
                    pending.rawAlert = rawAlert;  // Store raw alert, will fetch full content later
                    pending.needsFullFetch = true; // Flag to indicate content needs fetching
                    pendingAlerts.push_back(pending);
                    queuedCount++;
                    LOG_DEBUG("Queued alert for fetching: %s (ID: 0x%x)",
                             rawAlert.title.c_str(), rawAlert.id);
                }
            }

            if (queuedCount > 0) {
                LOG_INFO("Queued %d new alerts from [%s] for content fetching (skipped %d duplicates)",
                         queuedCount, sources[currentSourceIndex]->getSourceId().c_str(),
                         rawAlerts.size() - queuedCount);
            } else {
                LOG_DEBUG("No new alerts from [%s] (%d duplicates skipped)",
                          sources[currentSourceIndex]->getSourceId().c_str(), rawAlerts.size());
            }

            transitionToState(ModuleState::IDLE, "fetch processed, returning to idle");
            return ALERT_PROCESSING_YIELD_MS; // Quick return to start processing
        }
        
        
        case ModuleState::CALLING_AI: {
            // Check if we have enough memory for AI processing
            // AI calls require ~16KB for JSON buffer + HTTP client overhead
            const size_t MIN_HEAP_FOR_AI = 40000; // 40KB minimum
            size_t freeHeap = memGet.getFreeHeap();
            if (freeHeap < MIN_HEAP_FOR_AI) {
                LOG_WARN("Insufficient heap for AI processing (%d bytes free, need %d). Deferring...",
                         freeHeap, MIN_HEAP_FOR_AI);
                // Don't process now, wait for memory to free up
                // Put alert back in pending queue
                PendingAlert deferred;
                deferred.source = processingCtx.source;
                deferred.rawAlert = processingCtx.rawAlert;
                deferred.needsFullFetch = false;
                pendingAlerts.insert(pendingAlerts.begin(), deferred);
                processingCtx.active = false;
                transitionToState(ModuleState::IDLE, "insufficient heap to continue AI");
                return 5000; // Wait 5 seconds before retrying
            }

            // Create base alert object from raw alert
            processingCtx.alert.link = processingCtx.rawAlert.link;
            processingCtx.alert.title = processingCtx.rawAlert.title;
            processingCtx.alert.id = processingCtx.rawAlert.id; // Store the unique identifier hash
            processingCtx.alert.source = processingCtx.source->getSourceId();
            processingCtx.alert.severity = processingCtx.source->getDefaultSeverity();
            processingCtx.alert.lastSent = 0;

            if (currentTime > 0) {
                processingCtx.alert.addedAt = currentTime;
            } else {
                processingCtx.alert.addedAt = currentMillis / 1000;
            }
            
            // Check if source provided structured dates in RawAlert
            bool hasStructuredDates = (processingCtx.rawAlert.structuredStartDate.length() > 0 &&
                                       processingCtx.rawAlert.structuredEndDate.length() > 0);
            
            if (hasStructuredDates) {
                LOG_DEBUG("Source %s provided structured dates: %s to %s", 
                         processingCtx.source->getSourceId().c_str(),
                         processingCtx.rawAlert.structuredStartDate.c_str(), 
                         processingCtx.rawAlert.structuredEndDate.c_str());
            }
            
            // Calculate message size limits for this alert
            String sourcePrefixStr = "[" + processingCtx.source->getSourceId() + "] ";
            size_t sourcePrefixBytes = utf8ByteLength(sourcePrefixStr);
            const int maxPayload = meshtastic_Constants_DATA_PAYLOAD_LEN;
            const int maxLocationBytes = 35; // Approximate max for " [location]"
            const int safetyMargin = 10; // Safety buffer
            int maxMessageBytes = maxPayload - sourcePrefixBytes - maxLocationBytes - safetyMargin;

            // Check if the source provides a pre-processed message
            String preprocessedMessage = processingCtx.source->getPreprocessedMessage(processingCtx.rawAlert, maxMessageBytes);

            String message, aiStart, aiEnd, where;
            uint8_t aiSeverity = processingCtx.source->getDefaultSeverity();

            if (preprocessedMessage.length() > 0) {
                // Use pre-processed message directly (no AI needed)
                message = preprocessedMessage;

                // For pre-processed alerts, structured dates are already set in rawAlert
                // The date assignment logic below will use them automatically

                LOG_DEBUG("Using pre-processed message from %s: %s (severity: %d)",
                         processingCtx.source->getSourceId().c_str(), message.c_str(), aiSeverity);
            } else {
                // Use AI processing flow
                // Reset watchdog before starting AI processing (which can take up to 60 seconds with fallbacks)
                feedWatchdog();

                LOG_DEBUG("Calling AI for extraction (source: %s)",
                         processingCtx.source->getSourceId().c_str());

                if (!callAIForExtraction(processingCtx.source, processingCtx.rawAlert,
                                        message, aiStart, aiEnd, where, aiSeverity)) {
                    LOG_ERROR("AI extraction failed for alert: %s",
                             processingCtx.alert.title.c_str());
                    processingCtx.active = false;
                    transitionToState(ModuleState::IDLE, "AI extraction failed");
                    return ALERT_PROCESSING_YIELD_MS;
                }
            }

            // Reset watchdog after AI processing completes
            feedWatchdog();
            
            // Use structured dates if available, otherwise use AI-extracted or fallback
            if (hasStructuredDates) {
                // Source provided structured dates - use them (ignore AI dates)
                processingCtx.alert.valid_from = processingCtx.rawAlert.structuredStartDate;
                processingCtx.alert.valid_to = processingCtx.rawAlert.structuredEndDate;
            } else {
                // No structured dates - use AI extraction with fallbacks
                if (aiStart.length() > 0) {
                    processingCtx.alert.valid_from = aiStart;
                } else if (processingCtx.rawAlert.dateStr.length() > 0) {
                    // Fallback to source's publish date
                    processingCtx.alert.valid_from = processingCtx.rawAlert.dateStr;
                    LOG_DEBUG("No start date from AI, using publish date: %s",
                             processingCtx.rawAlert.dateStr.c_str());
                } else {
                    // No date from AI or source - fallback to current time
                    time_t now = time(nullptr);
                    if (now >= MIN_VALID_EPOCH) {
                        struct tm *nowTm = gmtime(&now);
                        if (nowTm) {
                            char currentTimeStr[20];
                            snprintf(currentTimeStr, sizeof(currentTimeStr), "%04d-%02d-%02d %02d:%02d:%02d",
                                    nowTm->tm_year + 1900, nowTm->tm_mon + 1, nowTm->tm_mday,
                                    nowTm->tm_hour, nowTm->tm_min, nowTm->tm_sec);
                            processingCtx.alert.valid_from = String(currentTimeStr);
                            LOG_DEBUG("No date from source or AI, using current time: %s", currentTimeStr);
                        } else {
                            // gmtime failed - set to empty string, will be handled later
                            processingCtx.alert.valid_from = "";
                            LOG_WARN("Could not get current time for alert start date");
                        }
                    } else {
                        // Time not synced - set to empty string, will be handled later
                        processingCtx.alert.valid_from = "";
                        LOG_WARN("Time not synced, cannot set alert start date");
                    }
                }
                
                // For end date, use AI result, or calculate reasonable expiration
                if (aiEnd.length() > 0) {
                    processingCtx.alert.valid_to = aiEnd;
                } else {
                    // No explicit end date - set reasonable expiration based on source type
                    time_t startTime = 0;
                    if (aiStart.length() > 0) {
                        startTime = parseDateString(aiStart);
                    } else {
                        startTime = parseDateString(processingCtx.rawAlert.dateStr);
                    }

                    if (startTime > 0) {
                        // Add default expiration period based on source
                        if (processingCtx.source->getSourceId() == "RCB") {
                            // RCB alerts are typically short-term emergency notifications
                            // Expire after 24 hours
                            startTime += (24 * 60 * 60);
                        } else {
                            startTime += (2 * 24 * 60 * 60);
                        }

                        // Convert back to string format
                        struct tm *expireTm = gmtime(&startTime);
                        if (expireTm) {
                            char expireStr[20];
                            snprintf(expireStr, sizeof(expireStr), "%04d-%02d-%02d %02d:%02d:%02d",
                                    expireTm->tm_year + 1900, expireTm->tm_mon + 1, expireTm->tm_mday,
                                    expireTm->tm_hour, expireTm->tm_min, expireTm->tm_sec);
                            processingCtx.alert.valid_to = String(expireStr);

                            LOG_DEBUG("Set default expiration for %s alert: %s",
                                     processingCtx.source->getSourceId().c_str(), expireStr);
                        } else {
                            // Fallback if gmtime fails
                            processingCtx.alert.valid_to = String((unsigned long)startTime);
                            LOG_DEBUG("Set default expiration for %s alert (fallback): %lu",
                                     processingCtx.source->getSourceId().c_str(), (unsigned long)startTime);
                        }
                    } else {
                        // No valid start time from AI or source - fallback to end of current day
                        time_t now = time(nullptr);
                        if (now >= MIN_VALID_EPOCH) {
                            // Set end date to end of current day (23:59:59)
                            struct tm *nowTm = gmtime(&now);
                            if (nowTm) {
                                char endOfDayStr[20];
                                snprintf(endOfDayStr, sizeof(endOfDayStr), "%04d-%02d-%02d 23:59:59",
                                        nowTm->tm_year + 1900, nowTm->tm_mon + 1, nowTm->tm_mday);
                                processingCtx.alert.valid_to = String(endOfDayStr);
                                LOG_DEBUG("No valid date from source or AI, using end of current day: %s", endOfDayStr);
                            } else {
                                // gmtime failed - use publish date as last resort
                                processingCtx.alert.valid_to = processingCtx.rawAlert.dateStr;
                                LOG_WARN("Could not calculate end of day, using publish date");
                            }
                        } else {
                            // Time not synced - use publish date as last resort
                            processingCtx.alert.valid_to = processingCtx.rawAlert.dateStr;
                            LOG_WARN("Time not synced, using publish date for expiration");
                        }
                    }
                }
            }
            
            // Populate alert with AI-extracted data
            processingCtx.alert.message = message.length() > 0 ? message : processingCtx.alert.title;
            processingCtx.alert.location = where;
            processingCtx.alert.alert_type = processingCtx.alert.title;
            processingCtx.alert.severity = aiSeverity;
            
            // Let source plugin validate and cleanup if needed
            if (!processingCtx.source->validateAndCleanup(processingCtx.alert)) {
                LOG_WARN("Alert validation failed, skipping");
                processingCtx.active = false;
                transitionToState(ModuleState::IDLE, "alert validation failed");
                return ALERT_PROCESSING_YIELD_MS;
            }

            LOG_DEBUG("AI extraction successful - severity: %d, location: %s",
                     processingCtx.alert.severity, processingCtx.alert.location.c_str());

            transitionToState(ModuleState::SAVING_ALERT, "AI extraction succeeded");
            return ALERT_PROCESSING_YIELD_MS; // Yield before disk I/O
        }
        
        case ModuleState::SAVING_ALERT: {
            // Use alert ID from raw alert (hash of URL, UUID, RSS GUID, etc.)
            uint32_t id = processingCtx.rawAlert.id;

            // Save to disk
            LOG_DEBUG("Saving alert to disk");
            if (!saveAlertToFile(processingCtx.alert, id, processingCtx.alert.valid_from)) {
                LOG_ERROR("Failed to save alert to disk");
                processingCtx.active = false;
                transitionToState(ModuleState::IDLE, "failed to save alert");
                return ALERT_PROCESSING_YIELD_MS;
            }

            LOG_DEBUG("Alert saved to disk");

            transitionToState(ModuleState::SENDING_ALERT, "alert saved to disk");
            return ALERT_PROCESSING_YIELD_MS; // Yield before sending
        }
        
        case ModuleState::SENDING_ALERT: {
            // Use alert ID from raw alert (hash of URL, UUID, RSS GUID, etc.)
            uint32_t id = processingCtx.rawAlert.id;
            
            // Check if alert is still valid before sending
            if (!isAlertValid(processingCtx.alert)) {
                LOG_WARN("Alert expired before sending, skipping: %s", 
                        processingCtx.alert.title.c_str());
                processingCtx.active = false;
                transitionToState(ModuleState::IDLE, "alert expired before sending");
                return ALERT_PROCESSING_YIELD_MS;
            }
            
            // Add to alerts vector if not duplicate
            if (!alertExists(processingCtx.alert.id)) {
                // Send to mesh
                if (sendAlertToMesh(processingCtx.alert)) {
                    processingCtx.alert.lastSent = currentMillis;
                    // Calculate next send time based on severity
                    unsigned long interval = getSendInterval(processingCtx.alert.severity);
                    processingCtx.alert.nextSendAt = currentTime + interval;
                    saveAlertToFile(processingCtx.alert, id, processingCtx.alert.valid_from);
                    LOG_INFO("Sent NEW alert [%s, sev:%d]: %s (next in %lu min)",
                             processingCtx.alert.source.c_str(), processingCtx.alert.severity,
                             processingCtx.alert.title.c_str(), interval / 60);
                } else {
                    // Failed to send - set retry delay to avoid tight loop
                    processingCtx.alert.nextSendAt = currentTime + 60; // Retry in 1 minute
                    saveAlertToFile(processingCtx.alert, id, processingCtx.alert.valid_from);
                    LOG_WARN("Failed to send new alert, will retry in 1 min [%s, sev:%d]: %s",
                             processingCtx.alert.source.c_str(), processingCtx.alert.severity,
                             processingCtx.alert.title.c_str());
                }

                alerts.push_back(processingCtx.alert);
                LOG_INFO("Alert processed successfully (total in memory: %d)", alerts.size());
            } else {
                LOG_DEBUG("Alert already exists in memory");
            }
            
            // Done processing this alert
            processingCtx.active = false;
            transitionToState(ModuleState::IDLE, "send flow complete");
            return ALERT_PROCESSING_YIELD_MS; // Quick return to process next alert
        }

        case ModuleState::FETCHING_DYNAMIC: {
            DynamicSource* source = dynamicSources[currentDynamicSourceIndex];
            LOG_INFO("Fetching data from dynamic source [%s]...", source->getSourceId().c_str());

            // Create HTTP GET callback for the source to use
            auto httpGetCallback = [this](const char* url, int& httpCode) -> String {
                return httpGet(url, httpCode);
            };

            // Fetch and format data - returns ready-to-send message
            String message = source->fetchAndFormat(httpGetCallback);
            feedWatchdog();

            // Update last fetch time for this source
            dynamicSourceLastFetchTime[currentDynamicSourceIndex] = currentMillis;

            if (message.length() == 0) {
                LOG_WARN("No data from dynamic source %s", source->getSourceId().c_str());
                transitionToState(ModuleState::IDLE, "dynamic source empty response");
                return ALERT_PROCESSING_YIELD_MS;
            }

            // Send immediately to mesh (no storage, no resending)
            if (sendMessageToMesh(message)) {
                LOG_INFO("Sent dynamic data from [%s]: %s",
                         source->getSourceId().c_str(), message.c_str());
            } else {
                LOG_WARN("Failed to send dynamic data from [%s]",
                         source->getSourceId().c_str());
            }

            transitionToState(ModuleState::IDLE, "dynamic source processing done");
            return ALERT_PROCESSING_YIELD_MS;
        }

        default:
            LOG_ERROR("Invalid state %d", (int)currentState);
            transitionToState(ModuleState::IDLE, "recovering from invalid state");
            return MAX_RUNONCE_INTERVAL_MS; // Return 1 minute on error
    }
}

bool AlertsModule::callAIForExtraction(AlertSource* source, const AlertSource::RawAlert &rawAlert,
                                       String &outMessage, String &outStart, String &outEnd, String &outWhere, uint8_t &outSeverity)
{
    LOG_DEBUG("Calling AI for extraction (source: %s)", source->getSourceId().c_str());

    // Check if AIService is available
    if (aiService == nullptr || !aiService->hasConfiguredProviders()) {
        LOG_ERROR("AIService not available or no providers configured");
        return false;
    }

    // Calculate source prefix dynamically
    String sourcePrefixStr = "[" + source->getSourceId() + "] ";
    size_t sourcePrefixBytes = utf8ByteLength(sourcePrefixStr);

    // Calculate available bytes for message
    const int maxPayload = meshtastic_Constants_DATA_PAYLOAD_LEN;
    const int maxLocationBytes = 35; // Approximate max for " [location]"
    const int safetyMargin = 10; // Safety buffer
    int maxMessageBytes = maxPayload - sourcePrefixBytes - maxLocationBytes - safetyMargin;

    // Max location chars (in chars, not bytes - AI should respect this)
    int maxLocationChars = 30;

    // Get source-specific prompt
    String prompt = source->buildAIPrompt(rawAlert, maxMessageBytes, maxLocationChars);

    LOG_DEBUG("AI prompt built (%d bytes)", prompt.length());

    // Try each AI provider until one succeeds (both HTTP call AND parsing)
    for (int providerIdx = 0; providerIdx < aiService->getMaxProviders(); providerIdx++) {
        AIService::AIProvider& provider = aiService->getProviders()[providerIdx];

        // Reset watchdog before trying each provider (AI calls can take 15+ seconds each)
        feedWatchdog();

        // Skip if provider not configured
        if (provider.endpoint.length() == 0 || provider.apiKey.length() == 0) {
            LOG_DEBUG("Skipping provider %s (not configured)", provider.name.c_str());
            continue;
        }

        LOG_INFO("Attempting AI extraction with [%s]...", provider.name.c_str());

        String aiResponse;
        bool httpSuccess = false;

        // Call the specific provider directly
        httpSuccess = aiService->callProvider(providerIdx, prompt, aiResponse);

        if (!httpSuccess) {
            LOG_WARN("HTTP call failed for [%s], trying next provider...", provider.name.c_str());
            continue;
        }

        LOG_DEBUG("AI response received from [%s] (%d bytes)", provider.name.c_str(), aiResponse.length());

        // Try to parse the response
        bool parseSuccess = parseAIResponse(aiResponse, outMessage, outStart, outEnd, outWhere, outSeverity);

        if (parseSuccess) {
            LOG_INFO("AI extraction successful with [%s] - severity: %d, location: %s",
                    provider.name.c_str(), outSeverity, outWhere.c_str());
            // Remember successful provider for future calls
            aiService->setCurrentProviderIndex(providerIdx);
            return true;
        } else {
            LOG_WARN("Failed to parse AI response from [%s], trying next provider...", provider.name.c_str());
        }
    }

    LOG_ERROR("All AI providers failed (either HTTP error or parsing error)");
    return false;
}


// Helper function to decode Unicode escape sequences (\uXXXX) to UTF-8
String decodeUnicodeEscapes(const String &input) {
    String output;
    output.reserve(input.length());
    
    for (int i = 0; i < input.length(); i++) {
        if (i + 5 < input.length() && input[i] == '\\' && input[i+1] == 'u') {
            // Found \uXXXX sequence
            String hexCode = input.substring(i+2, i+6);
            uint16_t unicodeChar = (uint16_t)strtol(hexCode.c_str(), NULL, 16);
            
            // Convert Unicode code point to UTF-8
            if (unicodeChar < 0x80) {
                // 1-byte UTF-8
                output += (char)unicodeChar;
            } else if (unicodeChar < 0x800) {
                // 2-byte UTF-8
                output += (char)(0xC0 | (unicodeChar >> 6));
                output += (char)(0x80 | (unicodeChar & 0x3F));
            } else {
                // 3-byte UTF-8 (handles most Polish and European characters)
                output += (char)(0xE0 | (unicodeChar >> 12));
                output += (char)(0x80 | ((unicodeChar >> 6) & 0x3F));
                output += (char)(0x80 | (unicodeChar & 0x3F));
            }
            
            i += 5; // Skip the \uXXXX sequence
        } else {
            output += input[i];
        }
    }
    
    return output;
}


bool AlertsModule::parseAIResponse(const String &response, String &outMessage, String &outStart, String &outEnd, String &outWhere, uint8_t &outSeverity)
{
    LOG_DEBUG("[parseAIResponse] Starting parsing (response length: %d)", response.length());

    // Use AIService to extract text content from JSON response
    String extractedText;
    if (!aiService->extractTextFromAIResponse(response, extractedText)) {
        LOG_ERROR("[parseAIResponse] Failed to extract text from AI response");
        return false;
    }

    LOG_DEBUG("[parseAIResponse] Extracted text content (length: %d)", extractedText.length());

    // Parse named field format: field:value|||___|||field:value|||___|||...
    // Fields can be in any order and some may be missing

    String delimiter = "|||___|||";

    // Initialize outputs with defaults
    outMessage = "";
    outStart = "";
    outEnd = "";
    outWhere = "";
    outSeverity = DEFAULT_SOURCE_SEVERITY;

    // Split response by delimiter
    int startPos = 0;
    while (startPos < extractedText.length()) {
        int delimPos = extractedText.indexOf(delimiter, startPos);
        String fieldStr;

        if (delimPos >= 0) {
            fieldStr = extractedText.substring(startPos, delimPos);
            startPos = delimPos + delimiter.length();
        } else {
            // Last field
            fieldStr = extractedText.substring(startPos);
            startPos = extractedText.length();
        }

        fieldStr.trim();

        // Parse field name and value (format: "fieldname:value")
        // The AI replaces {fieldname} with actual content, so we get "fieldname:actual_value"
        int colonPos = fieldStr.indexOf(':');
        if (colonPos <= 0) {
            LOG_WARN("[parseAIResponse] Invalid field format: '%s'", fieldStr.c_str());
            continue;
        }

        String fieldName = fieldStr.substring(0, colonPos);
        String fieldValue = fieldStr.substring(colonPos + 1);

        fieldName.trim();
        fieldValue.trim();

        // Map field to output variable
        if (fieldName == "message") {
            outMessage = fieldValue;
        } else if (fieldName == "where") {
            outWhere = fieldValue;
        } else if (fieldName == "severity") {
            outSeverity = fieldValue.toInt();
            if (outSeverity > 10) {
                LOG_WARN("[parseAIResponse] Invalid severity value: %d (expected 0-10)", outSeverity);
                outSeverity = DEFAULT_SOURCE_SEVERITY;
            }
        } else if (fieldName == "start") {
            outStart = fieldValue;
        } else if (fieldName == "end") {
            outEnd = fieldValue;
        } else {
            LOG_WARN("[parseAIResponse] Unknown field: '%s'", fieldName.c_str());
        }
    }

    // Validate required fields
    if (outMessage.length() == 0) {
        LOG_WARN("[parseAIResponse] Message field not found or empty");
        return false;
    }

    if (outWhere.length() == 0) {
        LOG_WARN("[parseAIResponse] Where field not found or empty");
        return false;
    }

    LOG_INFO("[parseAIResponse] Successfully parsed AI response: msg='%s', start='%s', end='%s', where='%s', severity=%d",
              outMessage.c_str(), outStart.c_str(), outEnd.c_str(), outWhere.c_str(), outSeverity);

    return true;
}


// ========== Utility Functions ==========

bool AlertsModule::isWifiAvailable()
{
#if HAS_WIFI && !defined(ARCH_PORTDUINO)
    // On ESP32, check if WiFi is enabled AND actually connected with a valid IP
    return (WiFi.status() == WL_CONNECTED) && (WiFi.localIP() != IPAddress(0, 0, 0, 0));
#else
    return false; // No WiFi on this platform
#endif
}

size_t AlertsModule::utf8ByteLength(const String &str)
{
    // String.length() in Arduino returns byte length, which is what we need for UTF-8
    return str.length();
}

bool AlertsModule::isAlertValid(const Alert &alert)
{
    time_t now = time(nullptr);

    // If current time is not valid, assume alert is valid
    // We'll check again when time is synced
    if (now < MIN_VALID_EPOCH) {
        return true; // Assume valid until we can check properly
    }

    time_t validTo = parseDateString(alert.valid_to);

    // Only check expiry - we WANT to broadcast alerts about future events
    // An alert is invalid only if it has expired (past validTo date)
    if (validTo > 0 && now > validTo) {
        return false; // Expired
    }

    return true; // Alert is valid (current or future)
}

bool AlertsModule::sendAlertToMesh(const Alert &alert)
{
    LOG_DEBUG("Sending alert to mesh - source: %s, severity: %d", alert.source.c_str(), alert.severity);

    // Prepare message with source prefix and geolocation suffix
    String sourcePrefix = "[" + alert.source + "] ";
    size_t sourcePrefixBytes = utf8ByteLength(sourcePrefix);
    
    String msg = alert.message;
    const int maxPayload = meshtastic_Constants_DATA_PAYLOAD_LEN;

    // Add location if available
    String locationSuffix = "";
    if (alert.location.length() > 0) {
        locationSuffix = " [" + alert.location + "]";
    }
    
    size_t suffixBytes = utf8ByteLength(locationSuffix);
    size_t msgBytes = utf8ByteLength(msg);
    
    // Check if prefix + message + suffix fits
    if (sourcePrefixBytes + msgBytes + suffixBytes > maxPayload) {
        // Try removing location suffix if location is already in the message
        if (alert.location.length() > 0 && msg.indexOf(alert.location) >= 0) {
            locationSuffix = "";
        } else {
            // Use title as fallback
            msg = alert.title;
            msgBytes = utf8ByteLength(msg);
            if (sourcePrefixBytes + msgBytes + suffixBytes > maxPayload) {
                locationSuffix = "";
            }
        }
    }

    msg = sourcePrefix + msg + locationSuffix;

    // Publish message to mesh
    meshtastic_MeshPacket *p = router->allocForSending();
    if (!p) {
        LOG_ERROR("Failed to allocate mesh packet for alert");
        return false;
    }
    p->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
    p->to = 0xffffffff; // Broadcast

    int8_t alertChannelIndex = findAlertChannel();
    if (alertChannelIndex < 0) {
        packetPool.release(p);
        return false;
    }
    p->channel = alertChannelIndex;
    p->want_ack = true;
    
    // Set priority based on severity
    if (alert.severity <= 2) {
        p->priority = meshtastic_MeshPacket_Priority_HIGH;
    } else {
        p->priority = meshtastic_MeshPacket_Priority_RELIABLE;
    }
    
    const size_t meshPayloadCapacity = sizeof(p->decoded.payload.bytes);
    const size_t effectiveMaxPayload = (meshPayloadCapacity < (size_t)maxPayload) ? meshPayloadCapacity : (size_t)maxPayload;
    size_t payloadBytes = clampMessageToPayload(msg, effectiveMaxPayload);
    if (payloadBytes > meshPayloadCapacity) {
        LOG_ERROR("Payload clamp guard triggered in sendAlertToMesh (want %u, cap %u), forcing clamp",
                  payloadBytes, meshPayloadCapacity);
        payloadBytes = meshPayloadCapacity;
    }
    p->decoded.payload.size = payloadBytes;
    if (payloadBytes > 0) {
        memcpy(p->decoded.payload.bytes, msg.c_str(), payloadBytes);
    }
    if (payloadBytes < meshPayloadCapacity) {
        p->decoded.payload.bytes[payloadBytes] = '\0';
    }
    
    LOG_INFO("Sending alert to mesh - channel: %d, size: %d, priority: %d", 
             alertChannelIndex, p->decoded.payload.size, p->priority);
    LOG_INFO("Message: %s", msg.c_str());
    
    if (service) {
        p->from = nodeDB->getNodeNum();
        service->sendToMesh(p, RX_SRC_USER, true);
        LOG_DEBUG("Alert sent to mesh network");
    } else {
        LOG_ERROR("MeshService not available");
        packetPool.release(p);
        return false;
    }

    return true;
}

bool AlertsModule::sendMessageToMesh(const String &message)
{
    LOG_DEBUG("Sending message to mesh: %s", message.c_str());

    const int maxPayload = meshtastic_Constants_DATA_PAYLOAD_LEN;
    size_t msgBytes = utf8ByteLength(message);

    if (msgBytes > maxPayload) {
        LOG_WARN("Message too long (%d bytes, max %d), truncating", msgBytes, maxPayload);
    }

    // Allocate and prepare mesh packet
    meshtastic_MeshPacket *p = router->allocForSending();
    if (!p) {
        LOG_ERROR("Failed to allocate mesh packet for message");
        return false;
    }
    p->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
    p->to = 0xffffffff; // Broadcast

    int8_t alertChannelIndex = findAlertChannel();
    if (alertChannelIndex < 0) {
        packetPool.release(p);
        return false;
    }
    p->channel = alertChannelIndex;
    p->want_ack = true;
    p->priority = meshtastic_MeshPacket_Priority_RELIABLE;

    String payloadMessage = message;
    const size_t meshPayloadCapacity = sizeof(p->decoded.payload.bytes);
    const size_t effectiveMaxPayload = (meshPayloadCapacity < (size_t)maxPayload) ? meshPayloadCapacity : (size_t)maxPayload;
    msgBytes = clampMessageToPayload(payloadMessage, effectiveMaxPayload);
    if (msgBytes > meshPayloadCapacity) {
        LOG_ERROR("Payload clamp guard triggered in sendMessageToMesh (want %u, cap %u), forcing clamp", msgBytes, meshPayloadCapacity);
        msgBytes = meshPayloadCapacity;
    }
    p->decoded.payload.size = msgBytes;
    if (msgBytes > 0) {
        memcpy(p->decoded.payload.bytes, payloadMessage.c_str(), msgBytes);
    }
    if (msgBytes < meshPayloadCapacity) {
        p->decoded.payload.bytes[msgBytes] = '\0';
    }

    LOG_INFO("Sending message to mesh - channel: %d, size: %d", alertChannelIndex, p->decoded.payload.size);

    if (service) {
        p->from = nodeDB->getNodeNum();
        service->sendToMesh(p, RX_SRC_USER, true);
        LOG_DEBUG("Message sent to mesh network");
        return true;
    }

    LOG_ERROR("MeshService not available");
    packetPool.release(p);
    return false;
}

int8_t AlertsModule::findAlertChannel()
{
    // If channel name is empty, use primary channel
    if (alertChannelName.length() == 0) {
        return channels.getPrimaryIndex();
    }

    // Look for existing channel with matching name
    int8_t foundIndex = -1;
    for (ChannelIndex i = 0; i < channels.getNumChannels(); i++) {
        meshtastic_Channel &ch = channels.getByIndex(i);
        if (ch.role == meshtastic_Channel_Role_DISABLED) {
            continue;
        }

        const char *channelName = channels.getName(i);
        if (strcasecmp(channelName, alertChannelName.c_str()) == 0) {
            foundIndex = i;
            break;
        }
    }

    // Log state changes only
    if (foundIndex >= 0 && lastKnownChannelIndex < 0) {
        LOG_INFO("Channel '%s' found at index %d - alerts will be sent", alertChannelName.c_str(), foundIndex);
    } else if (foundIndex < 0 && lastKnownChannelIndex >= 0) {
        LOG_WARN("Channel '%s' no longer available - alerts will not be sent", alertChannelName.c_str());
    } else if (foundIndex < 0 && lastKnownChannelIndex == -2) {
        LOG_ERROR("Channel '%s' not found - please create it manually", alertChannelName.c_str());
    } else if (foundIndex >= 0 && lastKnownChannelIndex >= 0 && foundIndex != lastKnownChannelIndex) {
        LOG_INFO("Channel '%s' moved from index %d to %d", alertChannelName.c_str(), lastKnownChannelIndex, foundIndex);
    }

    lastKnownChannelIndex = foundIndex;
    return foundIndex;
}

unsigned long AlertsModule::getSendInterval(uint8_t severity)
{
    unsigned long baseIntervalSec = SEVERITY_MIN_INTERVAL_SEC;
    unsigned long maxIntervalSec = SEVERITY_MAX_INTERVAL_SEC;
    unsigned long rangeSec = maxIntervalSec - baseIntervalSec;

    // Calculate proportional interval in seconds
    unsigned long intervalSec = baseIntervalSec + (severity * rangeSec / 10);

    return intervalSec;
}

time_t AlertsModule::parseDateString(const String &dateStr)
{
    if (dateStr.length() == 0) {
        return 0;
    }

    struct tm tm = {0};
    int day, month, year, hour = 0, minute = 0, second = 0;

    // Try YYYY-MM-DD hh:mm:ss format first (from AI)
    if (sscanf(dateStr.c_str(), "%d-%d-%d %d:%d:%d", &year, &month, &day, &hour, &minute, &second) >= 3) {
        // Validate ranges
        if (year < 2020 || year > 2030 || month < 1 || month > 12 || day < 1 || day > 31 ||
            hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) {
            return 0; // Invalid date
        }
        tm.tm_mday = day;
        tm.tm_mon = month - 1; // tm_mon is 0-11
        tm.tm_year = year - 1900; // tm_year is years since 1900
        tm.tm_hour = hour;
        tm.tm_min = minute;
        tm.tm_sec = second;
        tm.tm_isdst = -1; // Let system determine DST

        return mktime(&tm);
    }

    // Try DD.MM.YYYY or DD.MM.YYYY HH:MM format (from HTML parsing)
    if (sscanf(dateStr.c_str(), "%d.%d.%d %d:%d", &day, &month, &year, &hour, &minute) >= 3 ||
        sscanf(dateStr.c_str(), "%d.%d.%d", &day, &month, &year) >= 3) {
        // Validate ranges
        if (year < 2020 || month < 1 || month > 12 || day < 1 || day > 31 ||
            hour < 0 || hour > 23 || minute < 0 || minute > 59) {
            return 0; // Invalid date
        }
        tm.tm_mday = day;
        tm.tm_mon = month - 1; // tm_mon is 0-11
        tm.tm_year = year - 1900; // tm_year is years since 1900
        tm.tm_hour = hour;
        tm.tm_min = minute;
        tm.tm_sec = 0;
        tm.tm_isdst = -1; // Let system determine DST

        return mktime(&tm);
    }

    return 0; // Parsing failed
}

uint32_t AlertsModule::hashLink(const String &link)
{
    // Simple hash function: XOR all bytes
    uint32_t hash = 0;
    for (int i = 0; i < link.length(); i++) {
        hash ^= (uint32_t)link.charAt(i) << ((i % 4) * 8);
    }
    return hash;
}

void AlertsModule::cleanupOldAlerts()
{
    uint32_t startMs = millis();
    LOG_DEBUG("Starting cleanup of old alerts");
    feedWatchdog();

    cleanupTempFilesFromAlertsDir();

    uint32_t now = getTime(false); // Get current Unix timestamp (UTC)

    // If time is not available, skip cleanup (time might not be synced yet)
    if (now == 0 || now < MIN_VALID_EPOCH) {
        LOG_DEBUG("Time not synced yet (now=%lu), skipping cleanup", now);
        LOG_WARN("cleanupOldAlerts aborted after %lu ms because time is not synced", millis() - startMs);
        return;
    }

    int expiredInMemory = 0;
    int watchdogCounter = 0;
    for (auto it = alerts.begin(); it != alerts.end();) {
        if (!isAlertValid(*it)) {
            LOG_DEBUG("Found and removing expired alert from memory: %s", it->title.c_str());
            it = alerts.erase(it);
            expiredInMemory++;
        } else {
            ++it;
        }

        if (++watchdogCounter % 20 == 0) {
            feedWatchdog();
        }
    }

    if (expiredInMemory > 0) {
        uint32_t saveStartMs = millis();
        feedWatchdog();
        if (!saveAlertsToSingleFile()) {
            LOG_WARN("Failed to persist alerts after cleanup");
        } else {
            LOG_INFO("Cleanup removed %d expired alerts and updated single-file storage", expiredInMemory);
            LOG_WARN("cleanupOldAlerts persisted %d removals in %lu ms", expiredInMemory, millis() - saveStartMs);
        }
    } else {
        LOG_DEBUG("Cleanup completed - no expired alerts to remove");
    }

    uint32_t totalMs = millis() - startMs;
    if (totalMs > 200) {
        LOG_WARN("cleanupOldAlerts total duration %lu ms (expired=%d)", totalMs, expiredInMemory);
    }
}


void AlertsModule::purgeAllAlerts()
{
    LOG_INFO("Purging all alerts");
    concurrency::LockGuard g(spiLock);

    int deletedCount = 0;
    FSCom.mkdir(ALERTS_DIR);
    if (FSCom.remove(ALERTS_DATA_FILE)) {
        deletedCount++;
    }
    if (FSCom.remove(PROCESSED_IDS_FILE)) {
        deletedCount++;
    }

    File root = FSCom.open(ALERTS_DIR, FILE_O_READ);
    if (!root || !root.isDirectory()) {
        if (root) root.close();
        LOG_DEBUG("%s directory not found or is not a directory", ALERTS_DIR);
    } else {
        File file = root.openNextFile();
        while (file) {
            String filename = file.name();
            file.close();
            if (!filename.startsWith("/")) {
                filename = String(ALERTS_DIR) + "/" + filename;
            }
            if (filename.length() > 0 && FSCom.remove(filename.c_str())) {
                deletedCount++;
            }
            file = root.openNextFile();
        }
        root.close();
    }

    if (FSCom.remove(ALERTS_DATA_FILE_TMP)) {
        deletedCount++;
    }
    if (FSCom.remove(PROCESSED_IDS_FILE_TMP)) {
        deletedCount++;
    }

    alerts.clear();
    processedAlertIds.clear();
    processedAlertIdOrder.clear();
    LOG_INFO("Purged %d files and cleared in-memory cache", deletedCount);
}

// ===== Broadcasting Functions =====

bool AlertsModule::shouldBroadcastInfo()
{
    // Broadcasting is enabled when we have an alert channel configured
    return alertChannelName.length() > 0;
}

String AlertsModule::getChannelEncryptionKey()
{
    int8_t channelIndex = findAlertChannel();
    if (channelIndex < 0) {
        LOG_WARN("[AlertsModule] Cannot get encryption key - alert channel not found");
        return "";
    }

    const meshtastic_Channel &ch = channels.getByIndex(channelIndex);
    if (!ch.has_settings) {
        LOG_WARN("[AlertsModule] Cannot get encryption key - channel %d has no settings", channelIndex);
        return "";
    }

    const meshtastic_ChannelSettings &channelSettings = ch.settings;
    if (channelSettings.psk.size == 0) {
        // Channel has no encryption enabled
        LOG_DEBUG("[AlertsModule] Channel %d has no encryption set", channelIndex);
        return "";
    }

    return base64Encode(channelSettings.psk.bytes, channelSettings.psk.size);
}

String AlertsModule::base64Encode(const uint8_t* data, size_t length)
{
    static const char* base64Chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    String result = "";
    size_t i = 0;

    // Process 3 bytes at a time
    while (i < length) {
        uint32_t octet_a = i < length ? data[i++] : 0;
        uint32_t octet_b = i < length ? data[i++] : 0;
        uint32_t octet_c = i < length ? data[i++] : 0;

        uint32_t triple = (octet_a << 16) + (octet_b << 8) + octet_c;

        result += base64Chars[(triple >> 18) & 0x3F];
        result += base64Chars[(triple >> 12) & 0x3F];
        result += base64Chars[(triple >> 6) & 0x3F];
        result += base64Chars[triple & 0x3F];
    }

    // Add padding
    size_t padding = (3 - (length % 3)) % 3;
    for (size_t p = 0; p < padding; p++) {
        result[result.length() - 1 - p] = '=';
    }

    return result;
}

bool AlertsModule::broadcastInfoMessage()
{
    if (!shouldBroadcastInfo()) {
        return false;
    }

    String channelName = alertChannelName;
    String encryptionKey = getChannelEncryptionKey();

    // Format the message according to the template
    String message = "Otrzymuj alerty, powiadomienia, informacje lokalne: dodaj kanał ";
    message += channelName;
    if (encryptionKey.length() > 0) {
        message += ", klucz: ";
        message += encryptionKey;
    } else {
        message += " (bez szyfrowania)";
    }

    LOG_INFO("[AlertsModule] Broadcasting channel info: %s", message.c_str());

    // Send to primary channel (not alert channel)
    const int maxPayload = meshtastic_Constants_DATA_PAYLOAD_LEN;
    size_t msgBytes = utf8ByteLength(message);

    if (msgBytes > maxPayload) {
        LOG_WARN("Broadcast message too long (%d bytes, max %d), trimming from beginning", msgBytes, maxPayload);
        // Trim from the beginning to preserve channel name and key at the end
        size_t bytesToTrim = msgBytes - maxPayload;
        size_t trimPos = bytesToTrim;

        // Find a safe place to trim (after a space if possible)
        while (trimPos < message.length() && message[trimPos] != ' ') {
            trimPos++;
        }

        // If no space found, just trim at the calculated position
        if (trimPos >= message.length()) {
            trimPos = bytesToTrim;
        }

        message = message.substring(trimPos);
        msgBytes = message.length();
        LOG_DEBUG("Message trimmed to %d bytes: %s", msgBytes, message.c_str());
    }

    // Allocate and prepare mesh packet
    meshtastic_MeshPacket *p = router->allocForSending();
    if (!p) {
        LOG_ERROR("Failed to allocate mesh packet for channel info broadcast");
        return false;
    }
    p->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
    p->to = 0xffffffff; // Broadcast
    p->channel = channels.getPrimaryIndex(); // Always send to primary channel
    p->want_ack = true;
    p->priority = meshtastic_MeshPacket_Priority_RELIABLE;

    const size_t meshPayloadCapacity = sizeof(p->decoded.payload.bytes);
    const size_t effectiveMaxPayload = (meshPayloadCapacity < (size_t)maxPayload) ? meshPayloadCapacity : (size_t)maxPayload;
    msgBytes = clampMessageToPayload(message, effectiveMaxPayload);
    if (msgBytes > meshPayloadCapacity) {
        LOG_ERROR("Payload clamp guard triggered in broadcastInfoMessage (want %u, cap %u), forcing clamp", msgBytes, meshPayloadCapacity);
        msgBytes = meshPayloadCapacity;
    }
    p->decoded.payload.size = msgBytes;
    if (msgBytes > 0) {
        memcpy(p->decoded.payload.bytes, message.c_str(), msgBytes);
    }
    if (msgBytes < meshPayloadCapacity) {
        p->decoded.payload.bytes[msgBytes] = '\0';
    }

    LOG_INFO("Broadcasting channel info to primary channel - size: %d", p->decoded.payload.size);

    if (service) {
        p->from = nodeDB->getNodeNum();
        service->sendToMesh(p, RX_SRC_USER, true);
        LOG_DEBUG("Channel info broadcast sent to mesh network");
        return true;
    }

    LOG_ERROR("MeshService not available for broadcasting");
    packetPool.release(p);
    return false;
}

#endif // HAS_ALERTING
