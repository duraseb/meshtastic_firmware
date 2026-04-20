#pragma once

#if HAS_BROADCAST_BEACON

#include "BroadcastBeaconManager.h"
#include "concurrency/OSThread.h"

/**
 * BroadcastBeaconModule -- cross-preset broadcasting for Meshtastic
 *
 * ## What it does
 *
 * BroadcastBeacon lets a node deliver the same text message(s) to neighbours on
 * several different LoRa presets in turn. This is useful during a network
 * migration (e.g. LONG_FAST -> MEDIUM_FAST) when not every user has moved yet,
 * or any time an operator needs to reach users who are listening on a different
 * radio preset. The node rotates through a configured list of presets on a
 * timer, re-broadcasting each active message once per preset window, then
 * returns to its original "home" preset when the broadcast period ends or the
 * operator disables it.
 *
 * ## How it works
 *
 * Switching the LoRa preset at runtime is fragile (the radio state machine,
 * channel hashes, and queued packets all have to be rebuilt), so instead the
 * module takes the simple reliable path: it changes `config.lora.modem_preset`
 * through the same API the phone/web client uses (`service->reloadConfig`) and
 * reboots. On every boot the module loads its persisted state, sends any
 * messages that are currently within their valid window on the current preset,
 * waits out the remainder of that window, then saves the next preset index and
 * triggers another reboot. The home preset is always slot 0 of the rotation so
 * the node is periodically reachable for admin DMs and normal mesh traffic on
 * its original network.
 *
 * Three files under `/broadcast_beacon/` hold state across reboots:
 * - `config.bin`   -- preset list, interval, enabled flag
 * - `messages.bin` -- up to 20 broadcast messages
 * - `state.bin`    -- home preset, current rotation index, broadcasting flag
 *
 * All three use an atomic write pattern (write to `.tmp`, then rename) with a
 * magic + version header so partial writes and format changes are detected.
 *
 * ## How to use it
 *
 * The module is controlled exclusively through private text messages beginning
 * with `/bb`. Only nodes whose PKI public keys appear in `config.security.
 * admin_key[]` are authorised; all other senders receive a rejection. Commands:
 *
 *   /bb help          -- list commands
 *   /bb config        -- set the preset rotation and full-cycle interval
 *   /bb create        -- add a new broadcast message
 *   /bb list          -- list messages
 *   /bb list <n>      -- show full details of message n
 *   /bb edit <n>      -- edit message n
 *   /bb delete <n>    -- remove message n
 *   /bb on            -- enable broadcasting, save home preset, start cycling
 *   /bb off           -- disable, restore home preset, reboot
 *   /bb status        -- current preset, rotation, active messages, next switch
 *
 * `create`, `edit`, and `config` open a short guided session: the module
 * prompts for one field at a time; the operator replies with the value, `.` to
 * accept the default/keep the current value, or `!` to abort. A literal dot or
 * bang in a value can be escaped by doubling (`..` -> `.`, `!!` -> `!`). A
 * single `?` re-sends the current prompt (useful because the Meshtastic app
 * does not let users send a bare space). Sessions time out after five minutes
 * of silence.
 *
 * A broadcast message has: body text, a channel (selected from the node's
 * configured channel list -- PSKs are whatever that channel already defines),
 * optional ISO-8601 start/end dates, and a hop limit. When no end date is set
 * the message broadcasts indefinitely.
 *
 * Every preset window starts with a NodeInfo broadcast (throttle bypassed) so
 * neighbours on that preset immediately learn who we are, followed by the
 * active broadcast messages, and then -- if configured -- a position packet.
 * The position packet uses the node's default hop limit and is only sent when
 * a GPS device is connected and has acquired a fix
 * (`nodeDB->hasLocalPositionSinceBoot()`).
 *
 * Two config toggles refine the behaviour:
 *  - "send position" -- broadcast our position on each preset switch. With
 *    this on, having no text messages is fine -- position alone keeps the
 *    cycle running.
 *  - "skip home messages" -- during the home preset window, send only
 *    NodeInfo (and position, if enabled) and omit the configured text
 *    messages. This keeps the home network quiet while still letting the
 *    node receive admin commands there.
 *
 * Once every configured message has expired (and position is off), or the
 * operator runs `/bb off`, the module restores the home preset and reboots
 * back into normal operation.
 *
 * ## Compile guard
 *
 * The entire module is wrapped in `#if HAS_BROADCAST_BEACON` and is off by
 * default. Variants that want it add `-DHAS_BROADCAST_BEACON=1` to their
 * build_flags.
 */
class BroadcastBeaconModule : private concurrency::OSThread {
public:
    BroadcastBeaconModule();

    /// Seconds remaining in the current preset window, or 0 if not cycling yet.
    uint32_t getWindowRemainingSec() const;

protected:
    int32_t runOnce() override;

private:
    BroadcastBeaconManager *manager;

    // Initialization state
    bool initialized;

    // Post-boot message sending
    bool messagesSent;
    bool positionSent;     // true once the position packet has gone out this window
    bool wasBroadcasting;  // previous observed state.broadcasting -- used to detect
                           // the false->true transition that starts a new window
    unsigned long bootMs;  // millis() anchor for the current window

    // How often to re-check for GPS lock while position is still pending
    static constexpr int32_t POSITION_POLL_MS = 5000;

    // Broadcast sending
    void sendPendingMessages();
    bool isMessageActive(const BroadcastBeaconManager::BroadcastMessage &msg) const;
    bool hasAnyActiveMessages() const;

    // Preset switching
    void switchToNextPresetAndReboot();
    void restoreHomePresetAndReboot();
};

#endif // HAS_BROADCAST_BEACON
