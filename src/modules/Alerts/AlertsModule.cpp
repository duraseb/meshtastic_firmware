#if defined(HAS_ALERTING) && HAS_ALERTING

#include "AlertsModule.h"
#include "AIService.h"
#include "sources/RCBAlertSource.h"
#include "sources/IMGWAlertSource.h"
#include "dynamic_sources/IMGWSynopSource.h"
#include "dynamic_sources/AiWeatherSource.h"
#include "mesh/wifi/WiFiAPClient.h"
#include "FSCommon.h"
#include "main.h"
#include "mesh/generated/meshtastic/mesh.pb.h"
#include "mesh/Router.h"
#include "mesh/MeshService.h"
#include "mesh/Channels.h"
#include "mesh/NodeDB.h"
#include "SPILock.h"
#include "RTC.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ctime>
#include <ctype.h>
#include <vector>
#include <sstream>

#ifdef ARCH_ESP32
#include "esp_task_wdt.h"
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

AlertsModule::AlertsModule() : OSThread("AlertsModule")
{
    LOG_INFO("[AlertsModule] Initializing Multi-Source Alert System");

#ifdef ALERT_CHANNEL_NAME
    alertChannelName = ALERT_CHANNEL_NAME;
#else
    alertChannelName = "Alert";
#endif

    currentState = ModuleState::INIT;
    initializationDone = false;
    lastCleanupTime = 0;
    intervalMs = 5 * 60 * 1000; // Default 5 minute check interval

    lastMemoryCheckTime = 0;
    lastReportedMemoryUsage = 0;

    processingCtx.active = false;
    processingCtx.source = nullptr;
    processingCtx.stateStartTime = 0;

    numSources = 0;
    currentSourceIndex = 0;

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
             20); // MIN_HOUR_OF_DAY from AiWeatherSource
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
    }
}

bool AlertsModule::loadConfig()
{
    if (config.network.wifi_enabled) {
    }
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
    std::unique_ptr<HTTPClient> http(new HTTPClient());
    std::unique_ptr<WiFiClientSecure> client(new WiFiClientSecure());

    if (!http || !client) {
        LOG_ERROR("Failed to allocate HTTP client resources");
        return payload;
    }

    client->setInsecure();
    http->begin(*client, url);
    http->setTimeout(HTTP_TIMEOUT_MS);

    // Add headers to reduce server load and identify ourselves
    http->addHeader("User-Agent", "Meshtastic-Alerts/1.0");
    http->addHeader("Accept", "text/html,application/json,text/plain,*/*");

    httpCode = http->GET();

    // Reset watchdog after potentially long HTTP operation
    feedWatchdog();

    if (httpCode > 0) {
        if (httpCode == HTTP_CODE_OK) {
            payload = http->getString();
            LOG_DEBUG("HTTP GET successful, received %d bytes", payload.length());

            // Sanity check payload size (prevent memory exhaustion)
            if (payload.length() > 50000) { // 50KB limit
                LOG_WARN("Response too large (%d bytes), truncating", payload.length());
                payload = payload.substring(0, 50000);
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


bool AlertsModule::alertExists(uint32_t id)
{
    for (auto &a : alerts) {
        if (a.id == id)
            return true;
    }
    return false;
}

String AlertsModule::getAlertFilename(uint32_t id, const String &dateStr)
{
    // Create directory if it doesn't exist
    FSCom.mkdir(ALERTS_DIR);
    
    String datePrefix;
    if (dateStr.length() > 0) {
        if (dateStr.indexOf('-') >= 0) {
            datePrefix = dateStr;
            datePrefix.replace("-", "");
            datePrefix = datePrefix.substring(0, 8);
        } else if (dateStr.indexOf('.') >= 0) {
            int firstDot = dateStr.indexOf('.');
            int secondDot = dateStr.indexOf('.', firstDot + 1);
            if (firstDot > 0 && secondDot > firstDot) {
                String day = dateStr.substring(0, firstDot);
                String month = dateStr.substring(firstDot + 1, secondDot);
                String year = dateStr.substring(secondDot + 1);
                if (day.length() == 1) day = "0" + day;
                if (month.length() == 1) month = "0" + month;
                datePrefix = year + month + day;
            } else {
                datePrefix = "";
            }
        } else {
            datePrefix = dateStr.substring(0, 8);
        }
    } else {
        uint32_t now = getTime(false);
        if (now > 0) {
            struct tm *timeinfo = gmtime((time_t *)&now);
            char dateBuf[9];
            snprintf(dateBuf, sizeof(dateBuf), "%04d%02d%02d",
                     timeinfo->tm_year + 1900, timeinfo->tm_mon + 1, timeinfo->tm_mday);
            datePrefix = String(dateBuf);
        } else {
            datePrefix = "00000000";
        }
    }

    return String(ALERTS_DIR) + "/" + datePrefix + "_" + String(id, HEX) + ".bin";
}

String AlertsModule::extractDateFromFilename(const String &filename)
{
    // Extract date from filename: {YYYYMMDD}_{alertId}.bin or {ALERTS_DIR}/{YYYYMMDD}_{alertId}.bin
    // Find the start of the actual filename (after last slash if present)
    int lastSlash = filename.lastIndexOf('/');
    int startPos = (lastSlash >= 0) ? lastSlash + 1 : 0;
    
    // Find the underscore that separates date from hash
    int underscore = filename.indexOf('_', startPos);
    if (underscore > startPos) {
        String dateStr = filename.substring(startPos, underscore);
        // Validate it's 8 digits (YYYYMMDD)
        if (dateStr.length() == 8) {
            bool allDigits = true;
            for (int i = 0; i < 8; i++) {
                if (!isdigit(dateStr.charAt(i))) {
                    allDigits = false;
                    break;
                }
            }
            if (allDigits) {
                return dateStr;
            }
        }
    }
    return String();
}

bool AlertsModule::isAlertProcessed(uint32_t id)
{
    concurrency::LockGuard g(spiLock);
    String hashStr = String(id, HEX);

    FSCom.mkdir(ALERTS_DIR);
    File root = FSCom.open(ALERTS_DIR, FILE_O_READ);
    if (!root || !root.isDirectory()) {
        if (root) root.close();
        return false;
    }

    File file = root.openNextFile();
    while (file) {
        if (!file.isDirectory()) {
            String filename = file.name();
            if (filename.endsWith(".bin") && filename.indexOf("_" + hashStr + ".bin") >= 0) {
                file.close();
                root.close();
                return true;
            }
        }
        file.close();
        file = root.openNextFile();
    }
    root.close();
    return false;
}


bool AlertsModule::saveAlertToDisk(const Alert &alert)
{
    return saveAlertToFile(alert, alert.id);
}

bool AlertsModule::saveAlertToFile(const Alert &alert, uint32_t id, const String &dateStr)
{
    concurrency::LockGuard g(spiLock);

    if (id == 0) {
        LOG_ERROR("Cannot save alert with invalid ID");
        return false;
    }

    String useDate = dateStr.length() > 0 ? dateStr : (alert.valid_from.length() > 0 ? alert.valid_from : String());
    String filename = getAlertFilename(id, useDate);

    AlertBinary binAlert = {};

    // Copy strings with bounds checking and null termination
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
    binAlert.lastSent = alert.lastSent / 1000;
    binAlert.nextSendAt = alert.nextSendAt / 1000;
    binAlert.id = alert.id;

    // Atomic write with temp file
    String tempFilename = filename + ".tmp";

    File f = FSCom.open(tempFilename.c_str(), FILE_O_WRITE);
    if (!f) {
        LOG_ERROR("Failed to open temp file for writing: %s", tempFilename.c_str());
        return false;
    }

    size_t written = f.write((const uint8_t*)&binAlert, sizeof(AlertBinary));
    f.flush();
    bool flushOk = (written == sizeof(AlertBinary));

    if (written != sizeof(AlertBinary) || !flushOk) {
        LOG_ERROR("Failed to write/flush temp file %s (wrote %d bytes, flush: %s)",
                  tempFilename.c_str(), written, flushOk ? "ok" : "failed");
        f.close();
        FSCom.remove(tempFilename.c_str());
        return false;
    }

    f.close();

    if (!FSCom.rename(tempFilename.c_str(), filename.c_str())) {
        LOG_ERROR("Failed to rename temp file to %s", filename.c_str());
        FSCom.remove(tempFilename.c_str());
        return false;
    }

    LOG_DEBUG("Saved binary alert file: %s (%d bytes)", filename.c_str(), written);
    return true;
}

bool AlertsModule::loadAlertsFromDisk()
{
    LOG_DEBUG("Loading alerts from disk (memory limited)");
    alerts.clear();

    concurrency::LockGuard g(spiLock);

    FSCom.mkdir(ALERTS_DIR);
    File root = FSCom.open(ALERTS_DIR, FILE_O_READ);

    if (!root || !root.isDirectory()) {
        if (root) root.close();
        LOG_DEBUG("%s directory does not exist or is not a directory", ALERTS_DIR);
        return false;
    }

    std::vector<std::pair<String, uint32_t>> alertFiles;
    int filesScanned = 0;

    File file = root.openNextFile();
    while (file) {
        if (!file.isDirectory()) {
            String filename = file.name();
            if (filename.endsWith(".bin")) {
                AlertBinary binAlert;
                size_t bytesRead = file.read((uint8_t*)&binAlert, sizeof(AlertBinary));
                file.close();

                if (bytesRead == sizeof(AlertBinary) && binAlert.id > 0) {
                    alertFiles.push_back(std::make_pair(filename, binAlert.addedAt));
                } else {
                    LOG_WARN("Invalid binary alert file: %s (%d bytes, expected %d)",
                             filename.c_str(), bytesRead, sizeof(AlertBinary));
                }
            } else {
                file.close();
            }
        } else {
            file.close();
        }

        // Reset watchdog periodically during file scanning
        filesScanned++;
        if (filesScanned % 10 == 0) {
            feedWatchdog();
        }

        file = root.openNextFile();
    }
    root.close();

    // Reset watchdog after scanning
    feedWatchdog();

    std::sort(alertFiles.begin(), alertFiles.end(),
              [](const std::pair<String, uint32_t>& a, const std::pair<String, uint32_t>& b) {
                  return a.second > b.second;
              });

    int loadedCount = 0;
    for (const auto& fileInfo : alertFiles) {
        if (loadedCount >= MAX_ALERTS_IN_MEMORY) {
            LOG_INFO("Memory limit reached (%d alerts), skipping older alerts. Total available: %d",
                     MAX_ALERTS_IN_MEMORY, alertFiles.size());
            break;
        }

        String filename = fileInfo.first;
        String fullPath = String(ALERTS_DIR) + "/" + filename;

        File alertFile = FSCom.open(fullPath.c_str(), FILE_O_READ);
        if (alertFile) {
            AlertBinary binAlert;
            size_t bytesRead = alertFile.read((uint8_t*)&binAlert, sizeof(AlertBinary));
            alertFile.close();

            if (bytesRead == sizeof(AlertBinary)) {
                Alert a;
                a.title = String(binAlert.title);
                a.link = "";
                a.valid_from = String(binAlert.valid_from);
                a.valid_to = String(binAlert.valid_to);
                a.location = String(binAlert.location);
                a.message = String(binAlert.message);
                a.source = String(binAlert.source);
                a.severity = binAlert.severity;
                a.addedAt = binAlert.addedAt;
                a.id = binAlert.id;
                a.alert_type = a.title;

                // Reset millis()-based timestamps on load since they don't persist across reboots
                // Set lastSent to 0 and nextSendAt to 0 so alerts are checked immediately
                // after time sync, proper intervals will be applied after first resend
                a.lastSent = 0;
                a.nextSendAt = 0;

                if (isAlertValid(a)) {
                    alerts.push_back(a);
                    loadedCount++;
                } else {
                    LOG_DEBUG("Skipping expired alert during load: %s", a.title.c_str());
                }
            }
        }

        // Reset watchdog periodically during loading
        if (loadedCount % 10 == 0) {
            feedWatchdog();
        }
    }

    LOG_INFO("Loaded %d valid (non-expired) alerts from disk (of %d total files)", loadedCount, alertFiles.size());
    return loadedCount > 0;
}



// ========== Alert Processing Functions ==========


int32_t AlertsModule::runOnce()
{
    unsigned long currentMillis = millis();

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
            currentState = ModuleState::IDLE;
            LOG_INFO("Initialization complete - system responsive");
            return ALERT_PROCESSING_YIELD_MS; // Quick return to continue
        }
        
        case ModuleState::IDLE: {
            // Normal operation - check what needs to be done
            
            // Priority 1: Check for alerts that need re-sending (works without WiFi)
            // Only send if time is synced (to check dates) and radio is available
            uint32_t currentTime = getTime(false);
            if (currentTime > 0 && currentTime >= MIN_VALID_EPOCH) {
                // Time is synced, we can check and resend existing alerts
                // Limit processing to avoid blocking - only check a few alerts per cycle
                static size_t lastCheckedIndex = 0;
                size_t alertsCheckedThisCycle = 0;
                const size_t MAX_CHECKS_PER_CYCLE = 5; // Check max 5 alerts per runOnce call

                for (size_t i = 0; i < alerts.size() && alertsCheckedThisCycle < MAX_CHECKS_PER_CYCLE; i++) {
                    size_t checkIndex = (lastCheckedIndex + i) % alerts.size();
                    auto &alert = alerts[checkIndex];

                    if (!isAlertValid(alert)) {
                        continue;
                    }

                    alertsCheckedThisCycle++;

                    // Check if it's time to send based on pre-calculated nextSendAt
                    if (currentMillis >= alert.nextSendAt) {
                        // Time to re-send this alert (mesh only, no WiFi needed)
                        if (sendAlertToMesh(alert)) {
                            alert.lastSent = currentMillis;
                            // Calculate next send time based on severity
                            unsigned long interval = getSendInterval(alert.severity);
                            alert.nextSendAt = currentMillis + interval;
                            saveAlertToDisk(alert);
                            LOG_INFO("Re-sent alert [%s, sev:%d]: %s (next in %lu min)",
                                     alert.source.c_str(), alert.severity, alert.title.c_str(), interval / 60000);
                        } else {
                            // Failed to send - set retry delay to avoid tight loop
                            alert.nextSendAt = currentMillis + 60000; // Retry in 1 minute
                        }
                        // Only resend one alert per cycle to avoid blocking
                        lastCheckedIndex = (checkIndex + 1) % alerts.size(); // Resume after this alert
                        return RESEND_CHECK_YIELD_MS;
                    } else {
                        // Alert is pending but not yet due
                        unsigned long remainingMs = alert.nextSendAt - currentMillis;
                        unsigned long remainingSec = remainingMs / 1000;
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

                // Update starting index for next cycle (guard against division by zero)
                if (!alerts.empty()) {
                    lastCheckedIndex = (lastCheckedIndex + alertsCheckedThisCycle) % alerts.size();
                }
            }
            
            // Priority 2: Fetch full content for pending alerts (one at a time to avoid watchdog)
            if (!pendingAlerts.empty() && !processingCtx.active) {
                // Find first alert that needs full content fetch
                for (auto& pending : pendingAlerts) {
                    if (pending.needsFullFetch) {
                        LOG_INFO("Fetching full content for alert: %s (queue: %d)",
                                 pending.rawAlert.title.c_str(), pendingAlerts.size());

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

                        // Return to main loop to reset watchdog before processing next alert
                        return ALERT_PROCESSING_YIELD_MS;
                    }
                }

                // All content fetched, start AI processing for first alert
                PendingAlert pending = pendingAlerts.front();
                pendingAlerts.erase(pendingAlerts.begin());

                processingCtx.active = true;
                processingCtx.source = pending.source;
                processingCtx.rawAlert = pending.rawAlert;
                processingCtx.stateStartTime = currentMillis;

                LOG_INFO("Processing alert from [%s]: %s (queue: %d remaining)",
                         pending.source->getSourceId().c_str(), pending.rawAlert.title.c_str(), pendingAlerts.size());

                // Proceed to AI extraction
                currentState = ModuleState::CALLING_AI;
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
                return MAX_RUNONCE_INTERVAL_MS; // Return 1 minute, not WIFI_UNAVAILABLE_WAIT_MS
            }
            
            // Also check if time is synced before fetching (need valid dates)
            if (currentTime == 0 || currentTime < MIN_VALID_EPOCH) {
                LOG_DEBUG("Time not synced yet (now=%lu), waiting 60s", currentTime);
                return MAX_RUNONCE_INTERVAL_MS; // Return 1 minute, not TIME_SYNC_WAIT_MS
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
                    currentState = ModuleState::FETCHING_PAGE;
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
                    currentState = ModuleState::FETCHING_DYNAMIC;
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
                    if (elapsed < alertInterval) {
                        unsigned long timeUntilNext = alertInterval - elapsed;
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

            return returnInterval;
        }
        
        case ModuleState::FETCHING_PAGE: {
            LOG_INFO("Fetching alerts from source [%s]...", sources[currentSourceIndex]->getSourceId().c_str());

            // Create HTTP GET callback for the source to use
            auto httpGetCallback = [this](const char* url, int& httpCode) -> String {
                return httpGet(url, httpCode);
            };

            // Call source plugin to fetch and parse alerts (first pass - minimal data)
            std::vector<AlertSource::RawAlert> rawAlerts = sources[currentSourceIndex]->fetchAndParseAlerts(httpGetCallback);

            // Update last fetch time for this source
            sourceLastFetchTime[currentSourceIndex] = currentMillis;

            if (rawAlerts.empty()) {
                LOG_INFO("No new alerts from source %s", sources[currentSourceIndex]->getSourceId().c_str());
                currentState = ModuleState::IDLE;
                return ALERT_PROCESSING_YIELD_MS;
            }

            LOG_INFO("Found %d new alerts from source %s",
                     rawAlerts.size(), sources[currentSourceIndex]->getSourceId().c_str());

            // Queue raw alerts for full content fetching (without fetching content yet)
            // Content will be fetched one-at-a-time in FETCHING_ARTICLE state
            int queuedCount = 0;
            for (const auto& rawAlert : rawAlerts) {
                // Check if this alert has already been processed
                if (isAlertProcessed(rawAlert.id)) {
                    LOG_DEBUG("Alert already processed (ID: 0x%x), skipping", rawAlert.id);
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
                            expiredMarker.nextSendAt = 0;
                            saveAlertToDisk(expiredMarker);
                            continue;
                        }
                    }

                    // Queue alert for full content fetching - don't fetch here to avoid watchdog
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

            currentState = ModuleState::IDLE;
            return ALERT_PROCESSING_YIELD_MS; // Quick return to start processing
        }
        
        
        case ModuleState::CALLING_AI: {
            // Create base alert object from raw alert
            processingCtx.alert.link = processingCtx.rawAlert.link;
            processingCtx.alert.title = processingCtx.rawAlert.title;
            processingCtx.alert.id = processingCtx.rawAlert.id; // Store the unique identifier hash
            processingCtx.alert.source = processingCtx.source->getSourceId();
            processingCtx.alert.severity = processingCtx.source->getDefaultSeverity();
            processingCtx.alert.lastSent = 0;
            
            uint32_t currentTime = getTime(false);
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
            
            // Call AI for extraction (message, location, and dates if not structured)
            String message, aiStart, aiEnd, where;
            uint8_t aiSeverity = processingCtx.source->getDefaultSeverity();
            
            LOG_DEBUG("Calling AI for extraction (source: %s)", 
                     processingCtx.source->getSourceId().c_str());
            
            if (!callAIForExtraction(processingCtx.source, processingCtx.rawAlert, 
                                    message, aiStart, aiEnd, where, aiSeverity)) {
                LOG_ERROR("AI extraction failed for alert: %s", 
                         processingCtx.alert.title.c_str());
                processingCtx.active = false;
                currentState = ModuleState::IDLE;
                return ALERT_PROCESSING_YIELD_MS;
            }
            
            // Use structured dates if available, otherwise use AI-extracted or fallback
            if (hasStructuredDates) {
                // Source provided structured dates - use them (ignore AI dates)
                processingCtx.alert.valid_from = processingCtx.rawAlert.structuredStartDate;
                processingCtx.alert.valid_to = processingCtx.rawAlert.structuredEndDate;
            } else {
                // No structured dates - use AI extraction with fallbacks
                if (aiStart.length() > 0) {
                    processingCtx.alert.valid_from = aiStart;
                } else {
                    // Fallback to source's publish date
                    processingCtx.alert.valid_from = processingCtx.rawAlert.dateStr;
                    LOG_DEBUG("No start date from AI, using publish date: %s", 
                             processingCtx.rawAlert.dateStr.c_str());
                }
                
                // For end date, use AI result, or start date, or publish date
                if (aiEnd.length() > 0) {
                    processingCtx.alert.valid_to = aiEnd;
                } else if (aiStart.length() > 0) {
                    processingCtx.alert.valid_to = aiStart;
                } else {
                    processingCtx.alert.valid_to = processingCtx.rawAlert.dateStr;
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
                currentState = ModuleState::IDLE;
                return ALERT_PROCESSING_YIELD_MS;
            }

            LOG_DEBUG("AI extraction successful - severity: %d, location: %s",
                     processingCtx.alert.severity, processingCtx.alert.location.c_str());

            currentState = ModuleState::SAVING_ALERT;
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
                currentState = ModuleState::IDLE;
                return ALERT_PROCESSING_YIELD_MS;
            }

            LOG_DEBUG("Alert saved to disk");

            currentState = ModuleState::SENDING_ALERT;
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
                currentState = ModuleState::IDLE;
                return ALERT_PROCESSING_YIELD_MS;
            }
            
            // Add to alerts vector if not duplicate
            if (!alertExists(processingCtx.alert.id)) {
                // Send to mesh
                if (sendAlertToMesh(processingCtx.alert)) {
                    processingCtx.alert.lastSent = currentMillis;
                    // Calculate next send time based on severity
                    unsigned long interval = getSendInterval(processingCtx.alert.severity);
                    processingCtx.alert.nextSendAt = currentMillis + interval;
                    saveAlertToFile(processingCtx.alert, id, processingCtx.alert.valid_from);
                    LOG_INFO("Sent NEW alert [%s, sev:%d]: %s (next in %lu min)",
                             processingCtx.alert.source.c_str(), processingCtx.alert.severity,
                             processingCtx.alert.title.c_str(), interval / 60000);
                } else {
                    // Failed to send - set retry delay to avoid tight loop
                    processingCtx.alert.nextSendAt = currentMillis + 60000; // Retry in 1 minute
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
            currentState = ModuleState::IDLE;
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
                currentState = ModuleState::IDLE;
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

            currentState = ModuleState::IDLE;
            return ALERT_PROCESSING_YIELD_MS;
        }

        default:
            LOG_ERROR("Invalid state %d", (int)currentState);
            currentState = ModuleState::IDLE;
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

    // Now parse the delimited format from the extracted text
    // Format: message|||___|||start|||___|||end|||___|||where|||___|||severity

    String delimiter = "|||___|||";
    int firstDelim = extractedText.indexOf(delimiter);
    if (firstDelim < 0) {
        LOG_WARN("[parseAIResponse] Delimiter not found in AI response");
        return false;
    }

    outMessage = extractedText.substring(0, firstDelim);
    outMessage.trim();

    String remaining = extractedText.substring(firstDelim + delimiter.length());
    int secondDelim = remaining.indexOf(delimiter);
    if (secondDelim < 0) {
        LOG_WARN("[parseAIResponse] Second delimiter not found in AI response");
        return false;
    }

    outStart = remaining.substring(0, secondDelim);
    outStart.trim();

    remaining = remaining.substring(secondDelim + delimiter.length());
    int thirdDelim = remaining.indexOf(delimiter);
    if (thirdDelim < 0) {
        LOG_WARN("[parseAIResponse] Third delimiter not found in AI response");
        return false;
    }

    outEnd = remaining.substring(0, thirdDelim);
    outEnd.trim();

    remaining = remaining.substring(thirdDelim + delimiter.length());
    int fourthDelim = remaining.indexOf(delimiter);
    if (fourthDelim < 0) {
        LOG_WARN("[parseAIResponse] Fourth delimiter not found in AI response");
        return false;
    }

    outWhere = remaining.substring(0, fourthDelim);
    outWhere.trim();

    String severityStr = remaining.substring(fourthDelim + delimiter.length());
    severityStr.trim();

    outSeverity = severityStr.toInt();
    if (outSeverity > 10) {
        LOG_WARN("[parseAIResponse] Invalid severity value: %d (expected 0-10)", outSeverity);
        outSeverity = DEFAULT_SOURCE_SEVERITY;
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
    
    p->decoded.payload.size = utf8ByteLength(msg);
    memcpy(p->decoded.payload.bytes, msg.c_str(), p->decoded.payload.size);
    
    LOG_INFO("Sending alert to mesh - channel: %d, size: %d, priority: %d", 
             alertChannelIndex, p->decoded.payload.size, p->priority);
    LOG_INFO("Message: %s", msg.c_str());
    
    if (service) {
        p->from = nodeDB->getNodeNum();
        service->sendToMesh(p, RX_SRC_USER, true);
        LOG_DEBUG("Alert sent to mesh network");
    } else {
        LOG_ERROR("MeshService not available");
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
        // Simple truncation - not ideal for UTF-8 but acceptable for now
        msgBytes = maxPayload;
    }

    // Allocate and prepare mesh packet
    meshtastic_MeshPacket *p = router->allocForSending();
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

    p->decoded.payload.size = msgBytes;
    memcpy(p->decoded.payload.bytes, message.c_str(), msgBytes);

    LOG_INFO("Sending message to mesh - channel: %d, size: %d", alertChannelIndex, p->decoded.payload.size);

    if (service) {
        p->from = nodeDB->getNodeNum();
        service->sendToMesh(p, RX_SRC_USER, true);
        LOG_DEBUG("Message sent to mesh network");
        return true;
    }

    LOG_ERROR("MeshService not available");
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
    unsigned long baseIntervalMs = SEVERITY_MIN_INTERVAL_MS;
    unsigned long maxIntervalMs = SEVERITY_MAX_INTERVAL_MS;
    unsigned long rangeMs = maxIntervalMs - baseIntervalMs;

    // Calculate proportional interval
    unsigned long intervalMs = baseIntervalMs + (severity * rangeMs / 10);

    return intervalMs;
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
    LOG_DEBUG("Starting cleanup of old alerts");
    concurrency::LockGuard g(spiLock);

    // Calculate cutoff date (configured retention period ago) in YYYYMMDD format
    uint32_t now = getTime(false); // Get current Unix timestamp (UTC)

    // If time is not available, skip cleanup (time might not be synced yet)
    if (now == 0 || now < MIN_VALID_EPOCH) {
        LOG_DEBUG("Time not synced yet (now=%lu), skipping cleanup", now);
        return;
    }

    // Calculate cutoff date (configured retention period ago)
    time_t cutoffTime = now - (ALERT_RETENTION_DAYS * 24UL * 60UL * 60UL);
    struct tm *cutoffTm = gmtime(&cutoffTime);
    char cutoffDateStr[9];
    snprintf(cutoffDateStr, sizeof(cutoffDateStr), "%04d%02d%02d",
             cutoffTm->tm_year + 1900, cutoffTm->tm_mon + 1, cutoffTm->tm_mday);
    String cutoffDate = String(cutoffDateStr);

    LOG_DEBUG("Cleanup cutoff date: %s (%d days ago)", cutoffDate.c_str(), ALERT_RETENTION_DAYS);

    // Create directory if it doesn't exist
    FSCom.mkdir(ALERTS_DIR);

    // List all files in /alerts directory
    File root = FSCom.open(ALERTS_DIR, FILE_O_READ);
    if (!root || !root.isDirectory()) {
        if (root) root.close();
        LOG_DEBUG("%s directory not found, skipping cleanup", ALERTS_DIR);
        return;
    }

    // Collect IDs of expired alerts (don't delete files - keep as processed markers)
    std::vector<uint32_t> idsToRemoveFromMemory;

    int filesScanned = 0;
    File file = root.openNextFile();
    while (file) {
        if (!file.isDirectory()) {
            String filename = file.name();
            // Only process .bin files with date prefix format: {YYYYMMDD}_{alertId}.bin
            if (filename.endsWith(".bin") && filename.indexOf('_') >= 0) {
                // Extract date from filename
                String fileDate = extractDateFromFilename(filename);

                if (fileDate.length() == 8) {
                    // Check if this file is for an expired alert
                    if (fileDate < cutoffDate) {
                        LOG_DEBUG("Found expired alert file (keeping as processed marker): %s (date: %s)",
                                 filename.c_str(), fileDate.c_str());

                        // Extract ID from filename for memory cleanup
                        int underscorePos = filename.indexOf('_');
                        int dotPos = filename.lastIndexOf('.');
                        if (underscorePos >= 0 && dotPos > underscorePos) {
                            String idHex = filename.substring(underscorePos + 1, dotPos);
                            uint32_t id = strtoul(idHex.c_str(), nullptr, 16);
                            if (id > 0) {
                                idsToRemoveFromMemory.push_back(id);
                            }
                        }
                    }
                } else {
                    // Invalid date format, skip (might be legacy file without date prefix)
                    LOG_DEBUG("Skipping file with invalid date format: %s", filename.c_str());
                }
            }
        }
        file.close();

        // Reset watchdog periodically during cleanup
        filesScanned++;
        if (filesScanned % 10 == 0) {
            feedWatchdog();
        }

        file = root.openNextFile();
    }
    root.close();

    // Remove from memory (after file operations to avoid issues)
    int memoryRemoved = 0;
    for (uint32_t id : idsToRemoveFromMemory) {
        for (auto it = alerts.begin(); it != alerts.end();) {
            if (it->id == id) {
                LOG_DEBUG("Removing expired alert from memory: %s", it->title.c_str());
                it = alerts.erase(it);
                memoryRemoved++;
                break; // Only one alert per ID
            } else {
                ++it;
            }
        }
    }

    // Also check for any alerts in memory that are expired (defensive programming)
    int expiredInMemory = 0;
    for (auto it = alerts.begin(); it != alerts.end();) {
        if (!isAlertValid(*it)) {
            LOG_DEBUG("Found and removing expired alert from memory: %s", it->title.c_str());
            it = alerts.erase(it);
            expiredInMemory++;
        } else {
            ++it;
        }
    }

    if (memoryRemoved > 0 || expiredInMemory > 0) {
        LOG_INFO("Cleanup completed - kept %d expired files as processed markers, removed %d from memory, found %d extra expired in memory",
                 (int)idsToRemoveFromMemory.size(), memoryRemoved, expiredInMemory);
    } else {
        LOG_DEBUG("Cleanup completed - no expired alerts in memory to remove");
    }
}


void AlertsModule::purgeAllAlerts()
{
    LOG_INFO("Purging all alerts");
    concurrency::LockGuard g(spiLock);
    
    // Create directory if it doesn't exist (shouldn't happen, but safe)
    FSCom.mkdir(ALERTS_DIR);
    
    // List all files in /alerts directory
    File root = FSCom.open(ALERTS_DIR, FILE_O_READ);
    if (!root || !root.isDirectory()) {
        if (root) root.close();
        LOG_DEBUG("%s directory not found or is not a directory", ALERTS_DIR);
        return;
    }
    
    int deletedCount = 0;
    int filesProcessed = 0;
    File file = root.openNextFile();
    while (file) {
        String filename;
        bool isAlertFile = false;

        if (!file.isDirectory()) {
            filename = file.name();
            // Only process .bin files (binary format)
            if (filename.endsWith(".bin")) {
                isAlertFile = true;
            }
        }

        // IMPORTANT: Close the file before trying to delete it
        // LittleFS cannot delete files that are currently open
        file.close();

        // Now try to delete if it's an alert file
        if (isAlertFile && filename.length() > 0) {
            // file.name() might return full path or just filename
            String fullPath;
            if (filename.startsWith("/")) {
                // Already a full path
                fullPath = filename;
            } else {
                // Just filename, construct full path
                fullPath = String(ALERTS_DIR) + "/" + filename;
            }

            // Try to delete the file
            if (FSCom.remove(fullPath.c_str())) {
                deletedCount++;
                LOG_DEBUG("Deleted alert file: %s", fullPath.c_str());
            } else {
                // Try alternative: if fullPath failed, try just filename
                if (filename.startsWith("/")) {
                    // Try without leading slash
                    String altPath = filename.substring(1);
                    if (FSCom.remove(altPath.c_str())) {
                        deletedCount++;
                        LOG_DEBUG("Deleted alert file (alt path): %s", altPath.c_str());
                    } else {
                        LOG_ERROR("Failed to delete alert file: %s (also tried: %s)", fullPath.c_str(), altPath.c_str());
                    }
                } else {
                    LOG_ERROR("Failed to delete alert file: %s", fullPath.c_str());
                }
            }
        }

        // Reset watchdog periodically during purge
        filesProcessed++;
        if (filesProcessed % 10 == 0) {
            feedWatchdog();
        }

        // Get next file
        file = root.openNextFile();
    }
    root.close();
    
    // Clear the in-memory alerts vector
    alerts.clear();
    LOG_INFO("Purged %d alert files and cleared in-memory cache", deletedCount);
}

#endif // HAS_ALERTING
