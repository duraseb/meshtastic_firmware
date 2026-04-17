#pragma once

#if HAS_BROADCAST_BEACON

#include "Observer.h"
#include "mesh/generated/meshtastic/config.pb.h"
#include "mesh/generated/meshtastic/mesh.pb.h"
#include <Arduino.h>

class BroadcastBeaconModule;

/**
 * BroadcastBeaconManager -- handles interactive broadcast beacon management via private mesh messages.
 *
 * Observes TextMessageModule for incoming DMs prefixed with '/bb' and provides:
 * - Broadcast message creation/editing/deletion with a step-by-step state machine
 * - Config management for preset rotation list and broadcast interval
 * - Admin-only access via PKI key authentication
 *
 * Storage files:
 * - /broadcast_beacon/config.bin  -- broadcast config (presets, interval, enabled)
 * - /broadcast_beacon/messages.bin -- broadcast messages
 * - /broadcast_beacon/state.bin   -- runtime state surviving reboots (home preset, current index)
 */
class BroadcastBeaconManager : public Observer<const meshtastic_MeshPacket *> {
public:
    explicit BroadcastBeaconManager(BroadcastBeaconModule *module);

    /// Load all data from disk
    void loadFromDisk();

    // ===== Constants =====
    static constexpr int MAX_BROADCAST_MESSAGES = 20;
    static constexpr int MAX_BROADCAST_PRESETS = 10;
    static constexpr uint8_t BB_HOP_LIMIT_DEFAULT = 0xFF;
    static constexpr unsigned long POST_BOOT_DELAY_MS = 10000;

    // ===== Broadcast Message =====
    struct BroadcastMessage {
        uint32_t id;
        char body[237];
        char channel[32];
        char dateFrom[20];   // ISO 8601 or empty = immediate
        char dateTo[20];     // ISO 8601 or empty = indefinite
        uint8_t hops;        // 0-7 explicit, BB_HOP_LIMIT_DEFAULT = device default
        uint32_t createdAt;
    };

    // ===== Broadcast Config =====
    struct BroadcastConfig {
        meshtastic_Config_LoRaConfig_ModemPreset presets[MAX_BROADCAST_PRESETS];
        uint8_t numPresets;
        uint32_t intervalMinutes;
        bool enabled;
    };

    // ===== Broadcast State (survives reboots) =====
    struct BroadcastState {
        meshtastic_Config_LoRaConfig_ModemPreset homePreset;
        uint8_t currentPresetIndex;
        bool broadcasting;
    };

    // ===== Accessors for BroadcastBeaconModule =====
    const BroadcastConfig &getConfig() const { return broadcastConfig; }
    const BroadcastState &getState() const { return broadcastState; }
    BroadcastMessage *getMessages() { return messages; }
    int getNumMessages() const { return numMessages; }

    // ===== State persistence (called by module for reboot cycle) =====
    bool saveState();

    // ===== State mutators (called by module) =====
    void setCurrentPresetIndex(uint8_t idx);
    void setBroadcasting(bool active);
    void setHomePreset(meshtastic_Config_LoRaConfig_ModemPreset preset);
    void clearBroadcastingState();

    // ===== Helpers (used by module too) =====
    int8_t resolveChannelIndex(const char *channelName) const;
    const char *presetDisplayName(meshtastic_Config_LoRaConfig_ModemPreset preset) const;

protected:
    int onNotify(const meshtastic_MeshPacket *mp) override;

private:
    // ===== Constants =====
    static constexpr int MAX_SESSIONS = 3;
    static constexpr unsigned long SESSION_TIMEOUT_MS = 5UL * 60 * 1000;
    // Max text bytes that fit in a single PKI-encrypted DM on the wire
    // (MAX_LORA_PAYLOAD_LEN 255 - header 16 - PKC overhead 12 - ~7 bytes of
    // Data encoding = ~220). sendReply chunks anything longer into multiple packets.
    static constexpr int MAX_PACKET_TEXT_LEN = 220;
    static constexpr int MAX_REPLY_LEN = MAX_PACKET_TEXT_LEN;

    // Storage paths
    static constexpr const char *CONFIG_FILE = "/broadcast_beacon/config.bin";
    static constexpr const char *CONFIG_FILE_TMP = "/broadcast_beacon/config.bin.tmp";
    static constexpr const char *MESSAGES_FILE = "/broadcast_beacon/messages.bin";
    static constexpr const char *MESSAGES_FILE_TMP = "/broadcast_beacon/messages.bin.tmp";
    static constexpr const char *STATE_FILE = "/broadcast_beacon/state.bin";
    static constexpr const char *STATE_FILE_TMP = "/broadcast_beacon/state.bin.tmp";

    // Storage magic numbers
    static constexpr uint32_t CONFIG_MAGIC = 0x42424347;   // "BBCG"
    static constexpr uint32_t MESSAGES_MAGIC = 0x42424D53;  // "BBMS"
    static constexpr uint32_t STATE_MAGIC = 0x42425354;     // "BBST"
    static constexpr uint16_t STORAGE_VERSION = 1;

    // ===== Session State Machine =====
    enum class SessionState {
        NONE,
        // Message creation/editing
        MSG_AWAIT_BODY,
        MSG_AWAIT_CHANNEL,
        MSG_AWAIT_DATE_FROM,
        MSG_AWAIT_DATE_TO,
        MSG_AWAIT_HOPS,
        MSG_CONFIRM,
        MSG_AWAIT_ANOTHER,
        // Config editing
        CFG_AWAIT_PRESETS,
        CFG_AWAIT_INTERVAL,
        CFG_CONFIRM
    };

    struct UserSession {
        uint32_t nodeNum;           // 0 = slot unused
        unsigned long lastActivityMs;
        SessionState state;
        bool isEdit;
        uint16_t editIndex;
        // Message fields
        char body[237];
        char channel[32];
        char dateFrom[20];
        char dateTo[20];
        uint8_t hops;
        // Config fields (used during CFG_ states)
        meshtastic_Config_LoRaConfig_ModemPreset pendingPresets[MAX_BROADCAST_PRESETS];
        uint8_t pendingNumPresets;
        uint32_t pendingIntervalMinutes;
    };

    // ===== Storage Structures =====
    struct StorageHeader {
        uint32_t magic;
        uint16_t version;
        uint16_t count;
    };

    // ===== Members =====
    BroadcastBeaconModule *ownerModule;
    UserSession sessions[MAX_SESSIONS];

    BroadcastConfig broadcastConfig;
    BroadcastMessage messages[MAX_BROADCAST_MESSAGES];
    int numMessages;
    BroadcastState broadcastState;

    // ===== Auth =====
    bool isAdmin(const meshtastic_MeshPacket *mp) const;

    // ===== Session Methods =====
    UserSession *findSession(uint32_t nodeNum);
    UserSession *allocateSession(uint32_t nodeNum);
    void clearSession(UserSession *session);
    void expireStaleSessions();

    // ===== Command Handlers =====
    void handleCommand(const meshtastic_MeshPacket *mp, const char *text);
    void handleSessionInput(const meshtastic_MeshPacket *mp, const char *text, UserSession *session);

    void cmdHelp(uint32_t toNode);
    void cmdOn(const meshtastic_MeshPacket *mp);
    void cmdOff(const meshtastic_MeshPacket *mp);
    void cmdStatus(uint32_t toNode);
    void cmdList(uint32_t toNode);
    void cmdListDetail(uint32_t toNode, int num);
    void cmdCreate(const meshtastic_MeshPacket *mp);
    void cmdEdit(const meshtastic_MeshPacket *mp, int num);
    void cmdDelete(const meshtastic_MeshPacket *mp, int num);
    void cmdConfig(const meshtastic_MeshPacket *mp);

    // ===== State Machine =====
    void promptForField(UserSession *session, uint32_t toNode);
    void finalizeMessage(UserSession *session, uint32_t toNode);
    void finalizeConfig(UserSession *session, uint32_t toNode);

    // ===== Messaging =====
    void sendReply(uint32_t toNodeNum, const char *text);
    void sendReplyFmt(uint32_t toNodeNum, const char *fmt, ...);

    // ===== Storage =====
    bool loadConfig();
    bool saveConfig();
    bool loadMessages();
    bool saveMessages();
    bool loadState();

    // ===== Helpers =====
    uint32_t hashMessageId(const char *body, uint32_t createdAt) const;

    static String unescapeInput(const char *text);
    static bool isAccept(const char *text);
    static bool isAbort(const char *text);
};

#endif // HAS_BROADCAST_BEACON
