#if defined(HAS_ALERTING) && HAS_ALERTING

#include "AlertsModule.h"
#include "sources/RCBAlertSource.h"
#include "sources/IMGWAlertSource.h"
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

    LOG_INFO("[AlertsModule] Total sources registered: %d", numSources);
    // AI provider fallback chain (Gemini → Perplexity → Mistral → Groq)
    aiProviders[0].name = "Gemini-2.5";
    aiProviders[0].endpoint = "https://generativelanguage.googleapis.com/v1beta/models/gemini-2.5-flash:generateContent";
    #ifdef GEMINI_API_KEY
    aiProviders[0].apiKey = GEMINI_API_KEY;
    #else
    aiProviders[0].apiKey = "";
    #endif
    aiProviders[0].requestFormat = "gemini";

    aiProviders[1].name = "Perplexity-Sonar";
    aiProviders[1].endpoint = "https://api.perplexity.ai/chat/completions";
    aiProviders[1].model = "sonar";
    #ifdef PERPLEXITY_API_KEY
    aiProviders[1].apiKey = PERPLEXITY_API_KEY;
    #else
    aiProviders[1].apiKey = "";
    #endif
    aiProviders[1].requestFormat = "perplexity";

    aiProviders[2].name = "Mistral-7B";
    aiProviders[2].endpoint = "https://api.mistral.ai/v1/chat/completions";
    aiProviders[2].model = "open-mistral-7b";
    #ifdef MISTRAL_API_KEY
    aiProviders[2].apiKey = MISTRAL_API_KEY;
    #else
    aiProviders[2].apiKey = "";
    #endif
    aiProviders[2].requestFormat = "mistral";

    aiProviders[3].name = "Groq";
    aiProviders[3].endpoint = "https://api.groq.com/openai/v1/chat/completions";
    aiProviders[3].model = "llama-3.3-70b-versatile";
    #ifdef GROQ_API_KEY
    aiProviders[3].apiKey = GROQ_API_KEY;
    #else
    aiProviders[3].apiKey = "";
    #endif
    aiProviders[3].requestFormat = "groq";
    
    currentAIProviderIndex = 0; // Start with first provider
    
    // Log which providers are configured and validate at least one is available
    int configuredCount = 0;
    for (int i = 0; i < MAX_AI_PROVIDERS; i++) {
        if (aiProviders[i].apiKey.length() > 0) {
            LOG_INFO("[AlertsModule] AI provider configured: %s", aiProviders[i].name.c_str());
            configuredCount++;
        }
    }

    if (configuredCount == 0) {
        LOG_ERROR("[AlertsModule] ==============================================================================");
        LOG_ERROR("[AlertsModule] FATAL - No AI providers configured!");
        LOG_ERROR("[AlertsModule] At least one API key must be set in .env file:");
        LOG_ERROR("[AlertsModule]  - GEMINI_API_KEY (free tier: 1500 req/day, recommended)");
        LOG_ERROR("[AlertsModule]  - PERPLEXITY_API_KEY (Pro: $5/month credit)");
        LOG_ERROR("[AlertsModule]  - MISTRAL_API_KEY (free tier, good for Polish)");
        LOG_ERROR("[AlertsModule]  - GROQ_API_KEY (free tier: 14,400 req/day, fallback)");
        LOG_ERROR("[AlertsModule] See src/modules/Alerts/ALERTING_SETUP.md for setup instructions");
        LOG_ERROR("[AlertsModule] ==============================================================================");
        // Module will still initialize but AI extraction will always fail
    } else {
        LOG_INFO("[AlertsModule] %d AI provider(s) available", configuredCount);
    }

    alertChannelName = "Alert";

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
                a.lastSent = binAlert.lastSent * 1000UL;
                a.nextSendAt = binAlert.nextSendAt * 1000UL;
                a.id = binAlert.id;
                a.alert_type = a.title;

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
                            LOG_WARN("Failed to re-send alert [%s, sev:%d]: %s",
                                     alert.source.c_str(), alert.severity, alert.title.c_str());
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
            
            // Priority 4: Run cleanup if needed
            if (lastCleanupTime == 0 || (currentMillis - lastCleanupTime) > CLEANUP_INTERVAL_MS) {
                LOG_DEBUG("Running cleanup");
                cleanupOldAlerts();
                lastCleanupTime = currentMillis;
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
            return minInterval > 0 ? minInterval : MAX_RUNONCE_INTERVAL_MS;
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
            
            // Save to disk
            LOG_DEBUG("Saving alert to disk");
            if (!saveAlertToDisk(processingCtx.alert)) {
                LOG_ERROR("Failed to save alert to disk");
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
                    LOG_ERROR("Failed to send new alert [%s, sev:%d]: %s", 
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
    
    // Try each configured AI provider until one succeeds
    for (int providerIdx = 0; providerIdx < MAX_AI_PROVIDERS; providerIdx++) {
        AIProvider &provider = aiProviders[providerIdx];
        
        // Skip if provider not configured
        if (provider.endpoint.length() == 0 || provider.apiKey.length() == 0) {
            LOG_DEBUG("Skipping provider %s (not configured)", provider.name.c_str());
            continue;
        }
        
        LOG_INFO("Attempting AI extraction with [%s]...", provider.name.c_str());
        
        bool success = false;
        if (provider.requestFormat == "gemini") {
            success = callGeminiAPI(provider, prompt, outMessage, outStart, outEnd, outWhere, outSeverity);
        } else if (provider.requestFormat == "perplexity") {
            success = callMistralAPI(provider, prompt, outMessage, outStart, outEnd, outWhere, outSeverity); // Reuse Mistral (OpenAI-compatible)
        } else if (provider.requestFormat == "mistral") {
            success = callMistralAPI(provider, prompt, outMessage, outStart, outEnd, outWhere, outSeverity);
        } else if (provider.requestFormat == "groq") {
            success = callGroqAPI(provider, prompt, outMessage, outStart, outEnd, outWhere, outSeverity);
        }
        
        if (success) {
            LOG_INFO("AI extraction successful with [%s]", provider.name.c_str());
            currentAIProviderIndex = providerIdx; // Remember successful provider for next time
            return true;
        } else {
            LOG_WARN("Provider [%s] failed, trying next fallback...", provider.name.c_str());
        }
    }
    
    LOG_ERROR("All AI providers failed");
    return false;
}

bool AlertsModule::callGeminiAPI(const AIProvider &provider, const String &prompt, String &outMessage, String &outStart, 
                                 String &outEnd, String &outWhere, uint8_t &outSeverity)
{
    // Call AI endpoint with POST
    HTTPClient http;
    WiFiClientSecure client;
    client.setInsecure();

    // Add API key to URL as query parameter for Gemini API
    String url = provider.endpoint;
    if (provider.apiKey.length() > 0) {
        url += "?key=" + provider.apiKey;
    }
    http.begin(client, url.c_str());
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(AI_TIMEOUT_MS); // AI service timeout

    // Build Gemini API request format
    String body = "{\"contents\":[{\"parts\":[{\"text\":\"";

    // Escape prompt for JSON
    for (int i = 0; i < prompt.length(); i++) {
        char c = prompt.charAt(i);
        if (c == '"') {
            body += "\\\"";
        } else if (c == '\\') {
            body += "\\\\";
        } else if (c == '\n') {
            body += "\\n";
        } else if (c == '\r') {
            body += "\\r";
        } else {
            body += c;
        }
    }
    body += "\"}]}]}";

    LOG_DEBUG("Sending AI request to %s (prompt length: %d)", provider.name.c_str(), prompt.length());
    int httpCode = http.POST(body);

    // Reset watchdog after potentially long AI API call
    feedWatchdog();

    bool success = false;

    if (httpCode == HTTP_CODE_OK) {
        String response = http.getString();
        LOG_DEBUG("AI response received from %s (%d bytes)", provider.name.c_str(), response.length());
        success = parseAIResponse(response, outMessage, outStart, outEnd, outWhere, outSeverity);
        if (success) {
            LOG_INFO("AI extraction successful - severity: %d, location: %s", outSeverity, outWhere.c_str());
        } else {
            // Log truncated response for debugging (first 500 chars)
            String truncatedResponse = response;
            if (truncatedResponse.length() > 500) {
                truncatedResponse = truncatedResponse.substring(0, 500) + "...";
            }
            LOG_ERROR("Failed to parse AI response from %s. Response (first 500 chars): %s", 
                     provider.name.c_str(), truncatedResponse.c_str());
        }
    } else {
        String errorResponse = http.getString();
        LOG_ERROR("AI request to %s failed with HTTP code %d. Response: %s", 
                 provider.name.c_str(), httpCode, errorResponse.c_str());
    }
    http.end();

    return success;
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

bool AlertsModule::callMistralAPI(const AIProvider &provider, const String &prompt, String &outMessage, String &outStart,
                                   String &outEnd, String &outWhere, uint8_t &outSeverity)
{
    HTTPClient http;
    WiFiClientSecure client;
    client.setInsecure();

    http.begin(client, provider.endpoint.c_str());
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", "Bearer " + provider.apiKey);
    http.setTimeout(AI_TIMEOUT_MS);

    // Build Mistral API request format (OpenAI-compatible)
    String body = "{\"model\":\"" + provider.model + "\",\"messages\":[{\"role\":\"user\",\"content\":\"";

    // Escape prompt for JSON
    for (int i = 0; i < prompt.length(); i++) {
        char c = prompt.charAt(i);
        if (c == '"') {
            body += "\\\"";
        } else if (c == '\\') {
            body += "\\\\";
        } else if (c == '\n') {
            body += "\\n";
        } else if (c == '\r') {
            body += "\\r";
        } else {
            body += c;
        }
    }
    body += "\"}],\"temperature\":0.1,\"max_tokens\":500}";

    LOG_DEBUG("Sending AI request to %s (prompt length: %d)", provider.name.c_str(), prompt.length());
    int httpCode = http.POST(body);

    // Reset watchdog after potentially long AI API call
    feedWatchdog();

    bool success = false;

    if (httpCode == HTTP_CODE_OK) {
        String response = http.getString();
        LOG_DEBUG("AI response received from %s (%d bytes)", provider.name.c_str(), response.length());

        // Parse Mistral/OpenAI response format: {"choices":[{"message":{"content":"..."}}]}
        int contentPos = response.indexOf("\"content\"");
        if (contentPos < 0) {
            LOG_ERROR("'content' field not found in Mistral response");
            http.end();
            return false;
        }
        
        int colonPos = response.indexOf(':', contentPos);
        int textStart = colonPos + 1;
        while (textStart < response.length() && (response.charAt(textStart) == ' ' || response.charAt(textStart) == '"')) {
            textStart++;
        }
        
        // Find the closing quote, handling escaped quotes
        int textEnd = textStart;
        bool escaped = false;
        while (textEnd < response.length()) {
            char c = response.charAt(textEnd);
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                break;
            }
            textEnd++;
        }
        
        if (textEnd >= response.length()) {
            LOG_ERROR("Could not parse Mistral response");
            http.end();
            return false;
        }
        
        String extractedText = response.substring(textStart, textEnd);
        
        // Decode Unicode escape sequences (\uXXXX) that Mistral/Perplexity return for Polish characters
        extractedText = decodeUnicodeEscapes(extractedText);
        
        // Then handle standard JSON escapes
        extractedText.replace("\\n", "\n");
        extractedText.replace("\\\"", "\"");
        extractedText.replace("\\\\", "\\");
        
        // Create a fake Gemini-style response for the existing parser
        String geminiStyleResponse = "{\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"" + extractedText + "\"}]}}]}";
        success = parseAIResponse(geminiStyleResponse, outMessage, outStart, outEnd, outWhere, outSeverity);
    } else {
        String errorResponse = http.getString();
        LOG_ERROR("AI request to %s failed with HTTP code %d. Response: %s",
                 provider.name.c_str(), httpCode, errorResponse.c_str());
    }
    http.end();

    return success;
}

bool AlertsModule::callGroqAPI(const AIProvider &provider, const String &prompt, String &outMessage, String &outStart,
                                 String &outEnd, String &outWhere, uint8_t &outSeverity)
{
    HTTPClient http;
    WiFiClientSecure client;
    client.setInsecure();

    http.begin(client, provider.endpoint.c_str());
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", "Bearer " + provider.apiKey);
    http.setTimeout(AI_TIMEOUT_MS);

    // Build Groq API request format (OpenAI-compatible)
    String body = "{\"model\":\"" + provider.model + "\",\"messages\":[{\"role\":\"user\",\"content\":\"";

    // Escape prompt for JSON
    for (int i = 0; i < prompt.length(); i++) {
        char c = prompt.charAt(i);
        if (c == '"') {
            body += "\\\"";
        } else if (c == '\\') {
            body += "\\\\";
        } else if (c == '\n') {
            body += "\\n";
        } else if (c == '\r') {
            body += "\\r";
        } else {
            body += c;
        }
    }
    body += "\"}],\"temperature\":0.1,\"max_tokens\":500}";

    LOG_DEBUG("Sending AI request to %s (prompt length: %d)", provider.name.c_str(), prompt.length());
    int httpCode = http.POST(body);

    // Reset watchdog after potentially long AI API call
    feedWatchdog();

    bool success = false;

    if (httpCode == HTTP_CODE_OK) {
        String response = http.getString();
        LOG_DEBUG("AI response received from %s (%d bytes)", provider.name.c_str(), response.length());

        // Parse Groq/OpenAI response format: {"choices":[{"message":{"content":"..."}}]}
        int contentPos = response.indexOf("\"content\"");
        if (contentPos < 0) {
            LOG_ERROR("'content' field not found in Groq response");
            http.end();
            return false;
        }
        
        int colonPos = response.indexOf(':', contentPos);
        int textStart = colonPos + 1;
        while (textStart < response.length() && (response.charAt(textStart) == ' ' || response.charAt(textStart) == '"')) {
            textStart++;
        }
        
        // Find the closing quote, handling escaped quotes
        int textEnd = textStart;
        bool escaped = false;
        while (textEnd < response.length()) {
            char c = response.charAt(textEnd);
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                break;
            }
            textEnd++;
        }
        
        if (textEnd >= response.length()) {
            LOG_ERROR("Could not parse Groq response");
            http.end();
            return false;
        }
        
        String extractedText = response.substring(textStart, textEnd);
        extractedText.replace("\\n", "\n");
        extractedText.replace("\\\"", "\"");
        extractedText.replace("\\\\", "\\");
        
        // Create a fake Gemini-style response for the existing parser
        String geminiStyleResponse = "{\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"" + extractedText + "\"}]}}]}";
        success = parseAIResponse(geminiStyleResponse, outMessage, outStart, outEnd, outWhere, outSeverity);
        
        if (success) {
            LOG_INFO("AI extraction successful - severity: %d, location: %s", outSeverity, outWhere.c_str());
        }
    } else {
        String errorResponse = http.getString();
        LOG_ERROR("AI request to %s failed with HTTP code %d. Response: %s",
                 provider.name.c_str(), httpCode, errorResponse.c_str());
    }
    http.end();

    return success;
}

bool AlertsModule::parseAIResponse(const String &response, String &outMessage, String &outStart, String &outEnd, String &outWhere, uint8_t &outSeverity)
{
    // Gemini API response format: {"candidates":[{"content":{"parts":[{"text":"message|||___|||start|||___|||end|||___|||where|||___|||severity"}]}}]}
    // We need to extract the text field, then parse the delimited format

    // Find "text" field - handle whitespace variations
    int textKeyPos = response.indexOf("\"text\"");
    if (textKeyPos < 0) {
        LOG_WARN("'text' field not found in AI response");
        return false;
    }
    
    // Find the colon after "text"
    int colonPos = response.indexOf(':', textKeyPos);
    if (colonPos < 0) {
        LOG_WARN("Colon not found after 'text' field");
        return false;
    }
    
    // Find the opening quote of the text value (skip whitespace)
    int textStart = colonPos + 1;
    while (textStart < response.length() && (response.charAt(textStart) == ' ' || response.charAt(textStart) == '\t' || response.charAt(textStart) == '\n' || response.charAt(textStart) == '\r')) {
        textStart++;
    }
    
    if (textStart >= response.length() || response.charAt(textStart) != '"') {
        LOG_WARN("Opening quote not found for 'text' value");
        return false;
    }
    textStart++; // Move past the opening quote

    // Find the end of the text string by tracking escaped quotes
    int textEnd = textStart;
    bool escaped = false;
    while (textEnd < response.length()) {
        char c = response.charAt(textEnd);
        if (escaped) {
            escaped = false;
        } else if (c == '\\') {
            escaped = true;
        } else if (c == '"') {
            // Check if this is the closing quote (not escaped)
            // Look ahead to see if we're at the end of the text field
            int nextChar = textEnd + 1;
            while (nextChar < response.length() && (response.charAt(nextChar) == ' ' || response.charAt(nextChar) == '\t' || response.charAt(nextChar) == '\n' || response.charAt(nextChar) == '\r')) {
                nextChar++;
            }
            if (nextChar < response.length() && (response.charAt(nextChar) == '}' || response.charAt(nextChar) == ']' || response.charAt(nextChar) == ',')) {
                // This looks like the end of the text field
                break;
            }
        }
        textEnd++;
    }

    if (textEnd >= response.length()) {
        LOG_WARN("Could not find end of 'text' field (reached end of response)");
        return false;
    }

    String delimitedData = response.substring(textStart, textEnd);
    
    // Unescape the string (handle escaped quotes and newlines from JSON)
    delimitedData.replace("\\\"", "\"");   // Replace \"
    delimitedData.replace("\\n", "\n");    // Replace \n
    delimitedData.replace("\\r", "\r");    // Replace \r
    delimitedData.replace("\\t", "\t");    // Replace \t
    delimitedData.replace("\\\\", "\\");   // Replace \\
    
    // Log the extracted data for debugging (truncate if too long)
    if (delimitedData.length() > 200) {
        LOG_DEBUG("Extracted data (first 200 chars): %s...", delimitedData.substring(0, 200).c_str());
    } else {
        LOG_DEBUG("Extracted data: %s", delimitedData.c_str());
    }
    
    // Parse delimited format: message|||___|||start|||___|||end|||___|||where|||___|||severity
    const char *delimiter = "|||___|||";
    const int delimiterLen = 9; // Length of "|||___|||"
    
    int pos = 0;
    int field = 0;
    
    while (pos < delimitedData.length() && field < 5) {
        int nextDelim = delimitedData.indexOf(delimiter, pos);
        if (nextDelim < 0) {
            // Last field - take everything remaining
            nextDelim = delimitedData.length();
        }
        
        String value = delimitedData.substring(pos, nextDelim);
        value.trim();
        
        switch (field) {
            case 0:
                outMessage = value;
                break;
            case 1:
                outStart = value;
                break;
            case 2:
                outEnd = value;
                break;
            case 3:
                outWhere = value;
                break;
            case 4:
                outSeverity = value.toInt();
                // Validate severity range
                if (outSeverity > 10) {
                    LOG_WARN("Severity out of range (%d), using default 3", outSeverity);
                    outSeverity = 3;
                }
                break;
        }
        
        pos = nextDelim + delimiterLen; // Move past delimiter
        field++;
    }
    
    // Validate that we extracted all required fields
    bool valid = (field == 5 && outMessage.length() > 0 && outStart.length() > 0 && outEnd.length() > 0 && outWhere.length() > 0);
    
    if (!valid) {
        LOG_WARN("Missing required fields in parsed data - fields found: %d, message: %d, start: %d, end: %d, where: %d", 
                 field, outMessage.length(), outStart.length(), outEnd.length(), outWhere.length());
        LOG_DEBUG("Parsed values - message: '%s', start: '%s', end: '%s', where: '%s', severity: %d", 
                  outMessage.c_str(), outStart.c_str(), outEnd.c_str(), outWhere.c_str(), outSeverity);
    }
    
    return valid;
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
    
    // If current time is not valid, we can't check expiry
    if (now < MIN_VALID_EPOCH) {
        return false; // Don't process alerts without valid time
    }
    
    time_t validFrom = parseDateString(alert.valid_from);
    time_t validTo = parseDateString(alert.valid_to);

    // If we have a valid_to date and can parse it, check expiry
    if (validTo > 0 && now > validTo) {
        return false; // Expired
    }
    
    // If we have a valid_from date and can parse it, check if it's started
    if (validFrom > 0 && now < validFrom) {
        return false; // Not yet valid
    }

    return true; // Alert is valid
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
    
    ChannelIndex alertChannelIndex = ensureAlertChannel();
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

ChannelIndex AlertsModule::ensureAlertChannel()
{
    // If channel name is empty, use primary channel
    if (alertChannelName.length() == 0) {
        LOG_DEBUG("Using primary channel");
        return channels.getPrimaryIndex();
    }
    
    // Check if Alert channel exists
    for (ChannelIndex i = 0; i < channels.getNumChannels(); i++) {
        const char *channelName = channels.getName(i);
        if (channelName && strcasecmp(channelName, alertChannelName.c_str()) == 0) {
            meshtastic_Channel &ch = channels.getByIndex(i);
            if (ch.role != meshtastic_Channel_Role_DISABLED) {
                LOG_DEBUG("Found Alert channel at index %d", i);
                return i;
            }
        }
    }
    
    // Channel doesn't exist, create it
    LOG_INFO("Creating Alert channel: %s", alertChannelName.c_str());
    for (ChannelIndex i = 1; i < channels.getNumChannels(); i++) {
        meshtastic_Channel &ch = channels.getByIndex(i);
        if (ch.role == meshtastic_Channel_Role_DISABLED || 
            (ch.role == meshtastic_Channel_Role_SECONDARY && (!ch.has_settings || ch.settings.name[0] == '\0'))) {
            meshtastic_Channel newChannel = {};
            newChannel.index = i;
            newChannel.role = meshtastic_Channel_Role_SECONDARY;
            newChannel.has_settings = true;
            strncpy(newChannel.settings.name, alertChannelName.c_str(), sizeof(newChannel.settings.name) - 1);
            newChannel.settings.name[sizeof(newChannel.settings.name) - 1] = '\0';
            newChannel.settings.psk.bytes[0] = ALERT_CHANNEL_PSK;
            newChannel.settings.psk.size = 1;
            newChannel.settings.uplink_enabled = true;
            newChannel.settings.downlink_enabled = true;
            
            channels.setChannel(newChannel);
            LOG_INFO("Successfully created Alert channel at index %d", i);
            return i;
        }
    }
    
    LOG_WARN("No available channel slot, using primary channel");
    return channels.getPrimaryIndex();
}

unsigned long AlertsModule::getSendInterval(uint8_t severity)
{
    // Severity 0 = configured interval, severity 10 = configured max interval
    // Linear interpolation: interval = baseInterval + (severity * (maxInterval - baseInterval) / 10)
    unsigned long baseIntervalMs = SEVERITY_0_INTERVAL_MS;
    unsigned long maxIntervalMs = SEVERITY_10_INTERVAL_MS;
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
