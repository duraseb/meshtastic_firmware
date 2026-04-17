#pragma once

#if HAS_BROADCAST_BEACON

#include "BroadcastBeaconManager.h"
#include "concurrency/OSThread.h"

/**
 * BroadcastBeaconModule -- cycles through LoRa presets via reboot, broadcasting
 * configured messages on each preset to reach users on different radio settings.
 *
 * Uses a reboot-based switching approach:
 * 1. Boot -> load state -> send messages on current preset
 * 2. Wait for window duration -> save next preset -> reboot
 * 3. Repeat until all messages expire or disabled
 */
class BroadcastBeaconModule : private concurrency::OSThread {
public:
    BroadcastBeaconModule();

protected:
    int32_t runOnce() override;

private:
    BroadcastBeaconManager *manager;

    // Initialization state
    bool initialized;
    static constexpr unsigned long INIT_DELAY_MS = 30000; // Wait for textMessageModule

    // Post-boot message sending
    bool messagesSent;
    unsigned long bootMs; // millis() when broadcasting window started

    // Broadcast sending
    void sendPendingMessages();
    bool isMessageActive(const BroadcastBeaconManager::BroadcastMessage &msg) const;
    bool hasAnyActiveMessages() const;

    // Preset switching
    void switchToNextPresetAndReboot();
    void restoreHomePresetAndReboot();
};

#endif // HAS_BROADCAST_BEACON
