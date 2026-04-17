#if HAS_BROADCAST_BEACON

#include "BroadcastBeaconModule.h"
#include "mesh/Channels.h"
#include "mesh/MeshService.h"
#include "mesh/NodeDB.h"
#include "mesh/Router.h"
#include "modules/TextMessageModule.h"
#include "main.h"
#include "RTC.h"
#include <cstdio>
#include <cstring>
#include <ctime>

// ========== Constructor ==========

BroadcastBeaconModule::BroadcastBeaconModule()
    : concurrency::OSThread("BroadcastBeacon"),
      manager(nullptr), initialized(false), messagesSent(false), bootMs(0)
{
}

// ========== Main Loop ==========

int32_t BroadcastBeaconModule::runOnce()
{
    // ===== Phase 1: Initialization =====
    if (!initialized) {
        if (millis() < INIT_DELAY_MS) {
            return 5000; // Check again in 5s
        }

        // textMessageModule should be available by now
        if (!textMessageModule) {
            LOG_WARN("[BroadcastBeacon] textMessageModule not available yet");
            return 5000;
        }

        manager = new BroadcastBeaconManager(this);
        manager->loadFromDisk();
        manager->observe(textMessageModule);
        LOG_INFO("[BroadcastBeacon] Initialized, observing text messages");

        initialized = true;
        bootMs = millis();
    }

    const auto &cfg = manager->getConfig();
    const auto &state = manager->getState();

    // ===== Phase 2: Disabled or no config =====
    if (!cfg.enabled) {
        if (state.broadcasting) {
            // Was broadcasting but got disabled (e.g., by /bb off in another context)
            // The /bb off command handles the reboot itself, but in case state is stale:
            restoreHomePresetAndReboot();
            return -1;
        }
        return 10000; // Check every 10s in case it gets enabled
    }

    // ===== Phase 3: No presets or messages configured =====
    if (cfg.numPresets == 0 || manager->getNumMessages() == 0) {
        if (state.broadcasting) {
            LOG_INFO("[BroadcastBeacon] No presets or messages, restoring home preset");
            restoreHomePresetAndReboot();
            return -1;
        }
        return 30000; // Check every 30s
    }

    // ===== Phase 4: Check for active messages =====
    if (!hasAnyActiveMessages()) {
        if (state.broadcasting) {
            LOG_INFO("[BroadcastBeacon] All messages expired, restoring home preset");
            restoreHomePresetAndReboot();
            return -1;
        }
        return 30000;
    }

    // ===== Phase 5: Start broadcasting if not already =====
    if (!state.broadcasting) {
        // Begin broadcast cycle
        manager->setHomePreset(config.lora.modem_preset);
        manager->setCurrentPresetIndex(0);
        manager->setBroadcasting(true);
        manager->saveState();
        bootMs = millis();
        LOG_INFO("[BroadcastBeacon] Starting broadcast cycle, home preset saved");
    }

    // ===== Phase 6: Send messages after post-boot delay =====
    if (!messagesSent) {
        unsigned long elapsed = millis() - bootMs;
        if (elapsed < BroadcastBeaconManager::POST_BOOT_DELAY_MS) {
            return BroadcastBeaconManager::POST_BOOT_DELAY_MS - elapsed;
        }

        sendPendingMessages();
        messagesSent = true;
        LOG_INFO("[BroadcastBeacon] Messages sent on preset %d (%s)",
                 state.currentPresetIndex,
                 cfg.numPresets > 0 ? manager->presetDisplayName(cfg.presets[state.currentPresetIndex]) : "?");
    }

    // ===== Phase 7: Wait for window to elapse, then switch =====
    unsigned long windowDurationMs = ((unsigned long)cfg.intervalMinutes * 60UL * 1000UL) / cfg.numPresets;
    unsigned long elapsed = millis() - bootMs;

    if (elapsed >= windowDurationMs) {
        switchToNextPresetAndReboot();
        return -1; // Disable thread, reboot is pending
    }

    // Sleep until window end
    return (int32_t)(windowDurationMs - elapsed);
}

// ========== Message Sending ==========

void BroadcastBeaconModule::sendPendingMessages()
{
    if (!manager || !router || !service) {
        return;
    }

    int numMessages = manager->getNumMessages();
    auto *msgs = manager->getMessages();

    for (int i = 0; i < numMessages; i++) {
        if (!isMessageActive(msgs[i])) {
            continue;
        }

        // Resolve channel
        int8_t chIdx = manager->resolveChannelIndex(msgs[i].channel);
        if (chIdx < 0) {
            LOG_WARN("[BroadcastBeacon] Cannot resolve channel '%s', using primary", msgs[i].channel);
            chIdx = 0;
        }

        meshtastic_MeshPacket *p = router->allocForSending();
        if (!p) {
            LOG_ERROR("[BroadcastBeacon] Failed to allocate packet for message %d", i);
            continue;
        }

        p->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
        p->to = NODENUM_BROADCAST;
        p->channel = chIdx;
        p->want_ack = false;
        p->decoded.want_response = false;

        if (msgs[i].hops != BroadcastBeaconManager::BB_HOP_LIMIT_DEFAULT) {
            p->hop_limit = msgs[i].hops;
        }

        size_t bodyLen = strlen(msgs[i].body);
        size_t maxLen = sizeof(p->decoded.payload.bytes);
        if (bodyLen > maxLen) {
            bodyLen = maxLen;
        }
        p->decoded.payload.size = bodyLen;
        memcpy(p->decoded.payload.bytes, msgs[i].body, bodyLen);

        service->sendToMesh(p);
        LOG_INFO("[BroadcastBeacon] Sent message %d on channel %d: %.40s", i, chIdx, msgs[i].body);
    }
}

static time_t parseIsoDate(const char *dateStr)
{
    if (!dateStr || strlen(dateStr) == 0) {
        return 0;
    }
    int year, month, day, hour = 0, minute = 0, second = 0;
    if (sscanf(dateStr, "%d-%d-%d %d:%d:%d", &year, &month, &day, &hour, &minute, &second) >= 3) {
        if (year < 2020 || year > 2100 || month < 1 || month > 12 || day < 1 || day > 31 ||
            hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) {
            return 0;
        }
        struct tm tm = {};
        tm.tm_year = year - 1900;
        tm.tm_mon = month - 1;
        tm.tm_mday = day;
        tm.tm_hour = hour;
        tm.tm_min = minute;
        tm.tm_sec = second;
        tm.tm_isdst = -1;
        return mktime(&tm);
    }
    return 0;
}

bool BroadcastBeaconModule::isMessageActive(const BroadcastBeaconManager::BroadcastMessage &msg) const
{
    time_t now = getTime();

    // If no RTC time available, treat all messages as active
    if (now == 0) {
        return true;
    }

    // Check start date
    if (strlen(msg.dateFrom) > 0) {
        time_t from = parseIsoDate(msg.dateFrom);
        if (from > 0 && now < from) {
            return false; // Not started yet
        }
    }

    // Check end date
    if (strlen(msg.dateTo) > 0) {
        time_t to = parseIsoDate(msg.dateTo);
        if (to > 0 && now > to) {
            return false; // Expired
        }
    }

    return true;
}

bool BroadcastBeaconModule::hasAnyActiveMessages() const
{
    if (!manager) {
        return false;
    }

    int numMessages = manager->getNumMessages();
    auto *msgs = manager->getMessages();

    for (int i = 0; i < numMessages; i++) {
        if (isMessageActive(msgs[i])) {
            return true;
        }
    }
    return false;
}

// ========== Preset Switching ==========

void BroadcastBeaconModule::switchToNextPresetAndReboot()
{
    if (!manager) {
        return;
    }

    const auto &cfg = manager->getConfig();
    const auto &state = manager->getState();

    uint8_t nextIdx = (state.currentPresetIndex + 1) % cfg.numPresets;

    // Save next index to state
    manager->setCurrentPresetIndex(nextIdx);
    manager->saveState();

    // Apply new preset via proper config API
    config.lora.modem_preset = cfg.presets[nextIdx];
    service->reloadConfig(SEGMENT_CONFIG);

    LOG_INFO("[BroadcastBeacon] Switching to preset %d (%s), rebooting...",
             nextIdx, manager->presetDisplayName(cfg.presets[nextIdx]));

    rebootAtMsec = millis() + 5000;
}

void BroadcastBeaconModule::restoreHomePresetAndReboot()
{
    if (!manager) {
        return;
    }

    const auto &state = manager->getState();
    meshtastic_Config_LoRaConfig_ModemPreset homePreset = state.homePreset;

    manager->clearBroadcastingState();

    // Restore home preset via proper config API
    config.lora.modem_preset = homePreset;
    service->reloadConfig(SEGMENT_CONFIG);

    LOG_INFO("[BroadcastBeacon] Restoring home preset (%s), rebooting...",
             manager->presetDisplayName(homePreset));

    rebootAtMsec = millis() + 5000;
}

#endif // HAS_BROADCAST_BEACON
