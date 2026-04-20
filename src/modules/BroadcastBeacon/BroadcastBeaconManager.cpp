#if HAS_BROADCAST_BEACON

#include "BroadcastBeaconManager.h"
#include "BroadcastBeaconModule.h"
#include "mesh/Channels.h"
#include "mesh/MeshService.h"
#include "mesh/NodeDB.h"
#include "mesh/Router.h"
#include "DisplayFormatters.h"
#include "FSCommon.h"
#include "SPILock.h"
#include "RTC.h"
#include "main.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>

// ========== Constructor ==========

BroadcastBeaconManager::BroadcastBeaconManager(BroadcastBeaconModule *module)
    : ownerModule(module), numMessages(0)
{
    for (int i = 0; i < MAX_SESSIONS; i++) {
        memset(&sessions[i], 0, sizeof(UserSession));
    }
    memset(&broadcastConfig, 0, sizeof(BroadcastConfig));
    memset(messages, 0, sizeof(messages));
    memset(&broadcastState, 0, sizeof(BroadcastState));
    // enabled stays false until the operator runs /bb on after a complete config.
}

void BroadcastBeaconManager::loadFromDisk()
{
    loadConfig();
    loadMessages();
    loadState();
    LOG_INFO("[BroadcastBeacon] Loaded config (home=%s, %d presets, %dmin interval, %s), %d messages, state: %s (idx %d)",
             broadcastConfig.homePresetValid ? presetDisplayName(broadcastConfig.homePreset) : "unset",
             broadcastConfig.numPresets, broadcastConfig.intervalMinutes,
             broadcastConfig.enabled ? "on" : "off",
             numMessages,
             broadcastState.broadcasting ? "broadcasting" : "idle",
             broadcastState.currentPresetIndex);
}

// ========== Observer Callback ==========

int BroadcastBeaconManager::onNotify(const meshtastic_MeshPacket *mp)
{
    if (!mp) {
        return 0;
    }

    // Only handle DMs addressed to this node
    if (mp->to != nodeDB->getNodeNum()) {
        return 0;
    }

    // Ignore broadcasts
    if (mp->to == NODENUM_BROADCAST) {
        return 0;
    }

    // Only text messages
    if (mp->decoded.portnum != meshtastic_PortNum_TEXT_MESSAGE_APP) {
        return 0;
    }

    const char *text = (const char *)mp->decoded.payload.bytes;
    size_t textLen = mp->decoded.payload.size;
    if (textLen == 0 || text == nullptr) {
        return 0;
    }

    // Null-terminate safely
    char buf[241];
    size_t copyLen = (textLen < sizeof(buf) - 1) ? textLen : sizeof(buf) - 1;
    memcpy(buf, text, copyLen);
    buf[copyLen] = '\0';

    // Trim whitespace
    char *start = buf;
    while (*start == ' ' || *start == '\t') {
        start++;
    }
    char *end = start + strlen(start) - 1;
    while (end > start && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) {
        *end-- = '\0';
    }

    // Expire stale sessions
    expireStaleSessions();

    // Check for active session first (takes priority over command parsing)
    UserSession *session = findSession(mp->from);
    if (session) {
        // '?' re-sends the current prompt (MT app doesn't allow sending just a space)
        if (strlen(start) == 0 || (start[0] == '?' && start[1] == '\0')) {
            session->lastActivityMs = millis();
            promptForField(session, mp->from);
            return 1;
        }
        LOG_INFO("[BroadcastBeacon] RX session input from 0x%x (state=%d): %.40s",
                 mp->from, (int)session->state, start);
        handleSessionInput(mp, start, session);
        return 1;
    }

    // Check for /bb prefix (case-insensitive)
    if (strncasecmp(start, "/bb", 3) != 0) {
        return 0; // Not for us, let other modules handle it
    }

    // Strip /bb prefix
    char *cmd = start + 3;
    while (*cmd == ' ') {
        cmd++;
    }

    // Verify admin access
    if (!isAdmin(mp)) {
        LOG_WARN("[BroadcastBeacon] RX /bb from 0x%x (not admin, rejected): %s", mp->from, cmd);
        sendReply(mp->from, "Not authorized. Admin PKI key required.");
        return 1;
    }

    LOG_INFO("[BroadcastBeacon] RX /bb from 0x%x: %s", mp->from, *cmd ? cmd : "(help)");
    handleCommand(mp, cmd);
    return 1;
}

// ========== Auth ==========

bool BroadcastBeaconManager::isAdmin(const meshtastic_MeshPacket *mp) const
{
    if (!mp->pki_encrypted) {
        return false;
    }
    for (int i = 0; i < 3; i++) {
        if (config.security.admin_key[i].size == 32 &&
            memcmp(mp->public_key.bytes, config.security.admin_key[i].bytes, 32) == 0) {
            return true;
        }
    }
    return false;
}

// ========== Session Methods ==========

BroadcastBeaconManager::UserSession *BroadcastBeaconManager::findSession(uint32_t nodeNum)
{
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (sessions[i].nodeNum == nodeNum) {
            return &sessions[i];
        }
    }
    return nullptr;
}

BroadcastBeaconManager::UserSession *BroadcastBeaconManager::allocateSession(uint32_t nodeNum)
{
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (sessions[i].nodeNum == 0) {
            memset(&sessions[i], 0, sizeof(UserSession));
            sessions[i].nodeNum = nodeNum;
            sessions[i].lastActivityMs = millis();
            sessions[i].hops = BB_HOP_LIMIT_DEFAULT;
            return &sessions[i];
        }
    }
    return nullptr;
}

void BroadcastBeaconManager::clearSession(UserSession *session)
{
    if (session) {
        memset(session, 0, sizeof(UserSession));
    }
}

void BroadcastBeaconManager::expireStaleSessions()
{
    unsigned long now = millis();
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (sessions[i].nodeNum != 0 && (now - sessions[i].lastActivityMs) > SESSION_TIMEOUT_MS) {
            LOG_INFO("[BroadcastBeacon] Session expired for node 0x%x", sessions[i].nodeNum);
            clearSession(&sessions[i]);
        }
    }
}

// ========== Command Dispatch ==========

void BroadcastBeaconManager::handleCommand(const meshtastic_MeshPacket *mp, const char *text)
{
    uint32_t from = mp->from;

    // Case-insensitive comparison
    char lower[241];
    strncpy(lower, text, sizeof(lower) - 1);
    lower[sizeof(lower) - 1] = '\0';
    for (char *p = lower; *p; p++) {
        *p = tolower(*p);
    }

    // Empty command after /bb = help
    if (strlen(lower) == 0 || strcmp(lower, "help") == 0) {
        cmdHelp(from);
        return;
    }

    if (strcmp(lower, "on") == 0) {
        cmdOn(mp);
        return;
    }
    if (strcmp(lower, "off") == 0) {
        cmdOff(mp);
        return;
    }
    if (strcmp(lower, "status") == 0) {
        cmdStatus(from);
        return;
    }
    if (strcmp(lower, "list") == 0) {
        cmdList(from);
        return;
    }
    if (strncmp(lower, "list ", 5) == 0) {
        int num = atoi(lower + 5);
        if (num > 0) {
            cmdListDetail(from, num);
            return;
        }
    }
    if (strcmp(lower, "create") == 0) {
        cmdCreate(mp);
        return;
    }
    if (strncmp(lower, "edit ", 5) == 0) {
        int num = atoi(lower + 5);
        if (num > 0) {
            cmdEdit(mp, num);
            return;
        }
    }
    if (strncmp(lower, "delete ", 7) == 0) {
        int num = atoi(lower + 7);
        if (num > 0) {
            cmdDelete(mp, num);
            return;
        }
    }
    if (strcmp(lower, "config") == 0) {
        cmdConfig(mp);
        return;
    }

    sendReply(from, "Unknown command. Send '/bb help' for available commands.");
}

// ========== Commands ==========

void BroadcastBeaconManager::cmdHelp(uint32_t toNode)
{
    sendReply(toNode,
              "BroadcastBeacon /bb commands:\n"
              "on, off, status, config\n"
              "list, list <n>\n"
              "create, edit <n>, delete <n>");
}

void BroadcastBeaconManager::cmdOn(const meshtastic_MeshPacket *mp)
{
    uint32_t from = mp->from;

    if (!broadcastConfig.homePresetValid) {
        sendReply(from, "No home preset set. Use '/bb config' first.");
        return;
    }
    if (broadcastConfig.numPresets == 0) {
        sendReply(from, "No presets configured. Use '/bb config' first.");
        return;
    }
    if (numMessages == 0 && !broadcastConfig.sendPosition) {
        sendReply(from, "No messages and position broadcast is off. Add messages or enable position via '/bb config'.");
        return;
    }

    if (!broadcastState.broadcasting) {
        broadcastState.currentPresetIndex = 0;
        broadcastState.broadcasting = true;
        if (!saveState()) {
            // Roll back in-memory flag so /bb on can be retried.
            broadcastState.broadcasting = false;
            sendReply(from, "Failed to persist state. Check device storage.");
            return;
        }
    }

    broadcastConfig.enabled = true;
    if (!saveConfig()) {
        broadcastConfig.enabled = false;
        sendReply(from, "Failed to persist config. Check device storage.");
        return;
    }

    sendReplyFmt(from, "Broadcasting enabled. Home preset: %s. Cycling %d presets every %d min.",
                 presetDisplayName(broadcastConfig.homePreset),
                 broadcastConfig.numPresets,
                 broadcastConfig.intervalMinutes);
}

void BroadcastBeaconManager::cmdOff(const meshtastic_MeshPacket *mp)
{
    uint32_t from = mp->from;

    broadcastConfig.enabled = false;
    if (!saveConfig()) {
        broadcastConfig.enabled = true;
        sendReply(from, "Failed to persist config. Check device storage.");
        return;
    }

    if (broadcastState.broadcasting && broadcastConfig.homePresetValid) {
        clearBroadcastingState();

        sendReplyFmt(from, "Broadcasting disabled. Restoring preset %s and rebooting...",
                     presetDisplayName(broadcastConfig.homePreset));

        // Restore home preset via proper API and reboot
        config.lora.modem_preset = broadcastConfig.homePreset;
        service->reloadConfig(SEGMENT_CONFIG);
        rebootAtMsec = millis() + 5000;
    } else {
        clearBroadcastingState();
        sendReply(from, "Broadcasting disabled.");
    }
}

void BroadcastBeaconManager::cmdStatus(uint32_t toNode)
{
    char buf[MAX_REPLY_LEN + 1];
    int pos = 0;

    // Effective state: don't claim the cycle is running unless it actually is.
    const char *state;
    const char *hint = nullptr;
    if (!broadcastConfig.homePresetValid || broadcastConfig.numPresets == 0) {
        state = "NEEDS CONFIG";
        hint = "Run '/bb config' to set home preset, cycle, and interval.";
    } else if (numMessages == 0 && !broadcastConfig.sendPosition) {
        state = "NEEDS MESSAGES";
        hint = "Run '/bb create' or enable position in '/bb config'.";
    } else if (!broadcastConfig.enabled) {
        state = "OFF";
        hint = "Run '/bb on' to start the cycle.";
    } else if (!broadcastState.broadcasting) {
        state = "Starting the cycle...";
    } else {
        state = "CYCLING";
    }

    pos += snprintf(buf + pos, sizeof(buf) - pos, "BroadcastBeacon: %s\n", state);

    if (broadcastConfig.homePresetValid) {
        pos += snprintf(buf + pos, sizeof(buf) - pos, "Home: %s\n",
                        presetDisplayName(broadcastConfig.homePreset));
    }

    if (broadcastConfig.numPresets > 0) {
        pos += snprintf(buf + pos, sizeof(buf) - pos, "Interval: %d min\nPresets:",
                        broadcastConfig.intervalMinutes);
        for (int i = 0; i < broadcastConfig.numPresets && pos < (int)sizeof(buf) - 20; i++) {
            pos += snprintf(buf + pos, sizeof(buf) - pos, " %s%s",
                            presetDisplayName(broadcastConfig.presets[i]),
                            (i == broadcastState.currentPresetIndex && broadcastState.broadcasting) ? "*" : "");
        }
        pos += snprintf(buf + pos, sizeof(buf) - pos, "\n");
    }

    pos += snprintf(buf + pos, sizeof(buf) - pos, "Messages: %d", numMessages);

    if (broadcastState.broadcasting && broadcastConfig.numPresets > 0 && ownerModule) {
        uint32_t remaining = ownerModule->getWindowRemainingSec();
        pos += snprintf(buf + pos, sizeof(buf) - pos, "\nNext switch: ~%us", remaining);
    }

    if (hint) {
        pos += snprintf(buf + pos, sizeof(buf) - pos, "\n%s", hint);
    }

    sendReply(toNode, buf);
}

void BroadcastBeaconManager::cmdList(uint32_t toNode)
{
    if (numMessages == 0) {
        sendReply(toNode, "No messages. Use '/bb create' to add one.");
        return;
    }

    char buf[MAX_REPLY_LEN + 1];
    int pos = 0;
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Messages (%d):\n", numMessages);

    for (int i = 0; i < numMessages && pos < (int)sizeof(buf) - 60; i++) {
        char channelList[MAX_CHANNELS_PER_MESSAGE * (CHANNEL_NAME_LEN + 2) + 1];
        formatChannelList(channelList, sizeof(channelList), messages[i].channels, messages[i].numChannels);
        pos += snprintf(buf + pos, sizeof(buf) - pos, "%d. %.40s%s [%s]\n",
                        i + 1,
                        messages[i].body,
                        strlen(messages[i].body) > 40 ? "..." : "",
                        channelList);
    }

    sendReply(toNode, buf);
}

void BroadcastBeaconManager::cmdListDetail(uint32_t toNode, int num)
{
    int idx = num - 1;
    if (idx < 0 || idx >= numMessages) {
        sendReplyFmt(toNode, "Message #%d not found.", num);
        return;
    }

    const BroadcastMessage &m = messages[idx];
    char hopsStr[8];
    if (m.hops == BB_HOP_LIMIT_DEFAULT) {
        strncpy(hopsStr, "default", sizeof(hopsStr));
    } else {
        snprintf(hopsStr, sizeof(hopsStr), "%d", m.hops);
    }

    char channelList[MAX_CHANNELS_PER_MESSAGE * (CHANNEL_NAME_LEN + 2) + 1];
    formatChannelList(channelList, sizeof(channelList), m.channels, m.numChannels);

    char header[MAX_REPLY_LEN + 1];
    snprintf(header, sizeof(header),
             "#%d hops:%s\nFrom: %s\nTo: %s\nCh: %s",
             num, hopsStr,
             strlen(m.dateFrom) > 0 ? m.dateFrom : "(now)",
             strlen(m.dateTo) > 0 ? m.dateTo : "(no expiry)",
             channelList);

    // Prefer a single packet when the combined text fits; otherwise put the
    // body first so the user sees the full message before the metadata.
    char combined[MAX_REPLY_LEN + 1];
    int combinedLen = snprintf(combined, sizeof(combined), "%s\n%s", m.body, header);
    if (combinedLen > 0 && combinedLen < (int)sizeof(combined)) {
        sendReply(toNode, combined);
    } else {
        sendReply(toNode, m.body);
        sendReply(toNode, header);
    }
}

void BroadcastBeaconManager::cmdCreate(const meshtastic_MeshPacket *mp)
{
    if (numMessages >= MAX_BROADCAST_MESSAGES) {
        sendReply(mp->from, "Message limit reached. Delete some first.");
        return;
    }

    UserSession *session = allocateSession(mp->from);
    if (!session) {
        sendReply(mp->from, "Too many active sessions. Try later.");
        return;
    }

    session->state = SessionState::MSG_AWAIT_BODY;
    session->isEdit = false;
    promptForField(session, mp->from);
}

void BroadcastBeaconManager::cmdEdit(const meshtastic_MeshPacket *mp, int num)
{
    int idx = num - 1;
    if (idx < 0 || idx >= numMessages) {
        sendReplyFmt(mp->from, "Invalid message number. Valid range: 1-%d", numMessages);
        return;
    }

    UserSession *session = allocateSession(mp->from);
    if (!session) {
        sendReply(mp->from, "Too many active sessions. Try later.");
        return;
    }

    session->state = SessionState::MSG_AWAIT_BODY;
    session->isEdit = true;
    session->editIndex = idx;

    // Pre-populate with existing values
    const BroadcastMessage &msg = messages[idx];
    strncpy(session->body, msg.body, sizeof(session->body) - 1);
    memcpy(session->channels, msg.channels, sizeof(session->channels));
    session->numChannels = msg.numChannels;
    strncpy(session->dateFrom, msg.dateFrom, sizeof(session->dateFrom) - 1);
    strncpy(session->dateTo, msg.dateTo, sizeof(session->dateTo) - 1);
    session->hops = msg.hops;

    promptForField(session, mp->from);
}

void BroadcastBeaconManager::cmdDelete(const meshtastic_MeshPacket *mp, int num)
{
    int idx = num - 1;
    if (idx < 0 || idx >= numMessages) {
        sendReplyFmt(mp->from, "Invalid message number. Valid range: 1-%d", numMessages);
        return;
    }

    // Shift remaining messages
    for (int i = idx; i < numMessages - 1; i++) {
        messages[i] = messages[i + 1];
    }
    numMessages--;
    memset(&messages[numMessages], 0, sizeof(BroadcastMessage));
    if (!saveMessages()) {
        sendReply(mp->from, "Delete failed to persist. Check device storage.");
        return;
    }
    sendReplyFmt(mp->from, "Message %d deleted. %d remaining.", num, numMessages);
}

void BroadcastBeaconManager::cmdConfig(const meshtastic_MeshPacket *mp)
{
    UserSession *session = allocateSession(mp->from);
    if (!session) {
        sendReply(mp->from, "Too many active sessions. Try later.");
        return;
    }

    session->state = SessionState::CFG_AWAIT_HOME_PRESET;
    // Pre-populate with current config; default home to the current preset on
    // first-time setup so the operator gets a sensible starting point.
    session->pendingHomePreset = broadcastConfig.homePresetValid
                                    ? broadcastConfig.homePreset
                                    : config.lora.modem_preset;
    session->pendingNumPresets = broadcastConfig.numPresets;
    memcpy(session->pendingPresets, broadcastConfig.presets, sizeof(session->pendingPresets));
    session->pendingIntervalMinutes = broadcastConfig.intervalMinutes;
    session->pendingSendPosition = broadcastConfig.sendPosition;
    session->pendingSkipHomeMessages = broadcastConfig.skipHomeMessages;

    promptForField(session, mp->from);
}

// ========== Session Input Handler ==========

void BroadcastBeaconManager::handleSessionInput(const meshtastic_MeshPacket *mp, const char *text, UserSession *session)
{
    session->lastActivityMs = millis();
    uint32_t toNode = mp->from;

    // Check for abort. In MSG_AWAIT_ANOTHER the previous message was already
    // saved, so '!' really means "no, don't add another" -- use "Done" (with
    // the setup hint) to avoid implying the saved message was discarded.
    if (isAbort(text)) {
        if (session->state == SessionState::MSG_AWAIT_ANOTHER) {
            sendDoneWithHint(toNode);
        } else {
            sendReply(toNode, "Cancelled.");
        }
        clearSession(session);
        return;
    }

    bool accepted = isAccept(text);
    String input = accepted ? "" : unescapeInput(text);

    switch (session->state) {

    // ===== Message States =====

    case SessionState::MSG_AWAIT_BODY: {
        if (accepted) {
            if (session->isEdit && strlen(session->body) > 0) {
                // Keep existing body during edit
            } else {
                sendReply(toNode, "Body is required. Enter message text:");
                return;
            }
        } else {
            strncpy(session->body, input.c_str(), sizeof(session->body) - 1);
            session->body[sizeof(session->body) - 1] = '\0';
        }
        session->state = SessionState::MSG_AWAIT_CHANNEL;
        promptForField(session, toNode);
        break;
    }

    case SessionState::MSG_AWAIT_CHANNEL: {
        if (!accepted) {
            // Parse space-separated channel numbers. Store raw settings names
            // (empty for the primary) so resolution stays stable across preset
            // changes during the broadcast cycle.
            int numCh = channels.getNumChannels();
            char parseBuf[241];
            strncpy(parseBuf, input.c_str(), sizeof(parseBuf) - 1);
            parseBuf[sizeof(parseBuf) - 1] = '\0';

            uint8_t newCount = 0;
            char newChannels[MAX_CHANNELS_PER_MESSAGE][CHANNEL_NAME_LEN];
            memset(newChannels, 0, sizeof(newChannels));

            char *token = strtok(parseBuf, " \t");
            while (token && newCount < MAX_CHANNELS_PER_MESSAGE) {
                int chNum = atoi(token);
                if (chNum < 1 || chNum > numCh) {
                    sendReplyFmt(toNode, "Invalid channel number '%s'. Valid: 1-%d.", token, numCh);
                    return;
                }
                const meshtastic_Channel &ch = channels.getByIndex(chNum - 1);
                if (ch.role == meshtastic_Channel_Role_DISABLED || !ch.has_settings) {
                    sendReplyFmt(toNode, "Channel %d is disabled. Pick another.", chNum);
                    return;
                }
                strncpy(newChannels[newCount], ch.settings.name, CHANNEL_NAME_LEN - 1);
                newChannels[newCount][CHANNEL_NAME_LEN - 1] = '\0';
                newCount++;
                token = strtok(nullptr, " \t");
            }

            if (newCount == 0) {
                sendReply(toNode, "Pick at least one channel:");
                return;
            }

            memcpy(session->channels, newChannels, sizeof(newChannels));
            session->numChannels = newCount;
        }
        session->state = SessionState::MSG_AWAIT_DATE_FROM;
        promptForField(session, toNode);
        break;
    }

    case SessionState::MSG_AWAIT_DATE_FROM: {
        if (!accepted) {
            if (input == "-") {
                session->dateFrom[0] = '\0'; // explicit clear -- "no start restriction"
            } else {
                strncpy(session->dateFrom, input.c_str(), sizeof(session->dateFrom) - 1);
                session->dateFrom[sizeof(session->dateFrom) - 1] = '\0';
            }
        }
        session->state = SessionState::MSG_AWAIT_DATE_TO;
        promptForField(session, toNode);
        break;
    }

    case SessionState::MSG_AWAIT_DATE_TO: {
        if (!accepted) {
            if (input == "-") {
                session->dateTo[0] = '\0'; // explicit clear -- "no expiry"
            } else {
                strncpy(session->dateTo, input.c_str(), sizeof(session->dateTo) - 1);
                session->dateTo[sizeof(session->dateTo) - 1] = '\0';
            }
        }
        session->state = SessionState::MSG_AWAIT_HOPS;
        promptForField(session, toNode);
        break;
    }

    case SessionState::MSG_AWAIT_HOPS: {
        if (!accepted) {
            if (input == "-") {
                session->hops = BB_HOP_LIMIT_DEFAULT; // reset to device default
            } else {
                int h = atoi(input.c_str());
                if (h < 0 || h > 7) {
                    sendReply(toNode, "Hops must be 0-7 (or '-' for device default). Try again:");
                    return;
                }
                session->hops = (uint8_t)h;
            }
        }
        session->state = SessionState::MSG_CONFIRM;
        promptForField(session, toNode);
        break;
    }

    case SessionState::MSG_CONFIRM: {
        if (accepted) {
            finalizeMessage(session, toNode);
        } else {
            sendReply(toNode, "'.' to confirm, '!' to abort");
        }
        break;
    }

    case SessionState::MSG_AWAIT_ANOTHER: {
        // Expect y/n -- '.' also treated as yes, 'n' as no. '!' is intercepted
        // upstream as global abort but we swap its message to "Done" afterwards.
        char c = input.length() > 0 ? tolower(input[0]) : 0;
        bool yes = accepted || c == 'y';
        bool no = c == 'n';
        if (yes) {
            memset(session->body, 0, sizeof(session->body));
            memset(session->channels, 0, sizeof(session->channels));
            session->numChannels = 0;
            memset(session->dateFrom, 0, sizeof(session->dateFrom));
            memset(session->dateTo, 0, sizeof(session->dateTo));
            session->hops = BB_HOP_LIMIT_DEFAULT;
            session->isEdit = false;
            session->state = SessionState::MSG_AWAIT_BODY;
            promptForField(session, toNode);
        } else if (no) {
            sendDoneWithHint(toNode);
            clearSession(session);
        } else {
            sendReply(toNode, "Reply 'y' or 'n':");
        }
        break;
    }

    // ===== Config States =====

    case SessionState::CFG_AWAIT_HOME_PRESET: {
        if (!accepted) {
            int num = atoi(input.c_str());
            int presetIdx = num - 1;
            if (presetIdx < 0 || presetIdx > _meshtastic_Config_LoRaConfig_ModemPreset_MAX) {
                sendReply(toNode, "Invalid preset number. Try again:");
                return;
            }
            const char *name = presetDisplayName((meshtastic_Config_LoRaConfig_ModemPreset)presetIdx);
            if (!name || !*name || strcmp(name, "Invalid") == 0) {
                sendReply(toNode, "That preset isn't available. Try again:");
                return;
            }
            session->pendingHomePreset = (meshtastic_Config_LoRaConfig_ModemPreset)presetIdx;
            // If the additional-presets list still contains the new home, drop
            // it -- the additional list must not overlap with home.
            uint8_t filtered = 0;
            for (uint8_t i = 0; i < session->pendingNumPresets; i++) {
                if (session->pendingPresets[i] != session->pendingHomePreset) {
                    session->pendingPresets[filtered++] = session->pendingPresets[i];
                }
            }
            session->pendingNumPresets = filtered;
        }
        session->state = SessionState::CFG_AWAIT_PRESETS;
        promptForField(session, toNode);
        break;
    }

    case SessionState::CFG_AWAIT_PRESETS: {
        if (!accepted) {
            // Parse space-separated preset numbers. strtok with " \t" as the
            // delimiter set collapses any run of whitespace, so "1  3   5" and
            // "1 3 5" both parse the same way.
            meshtastic_Config_LoRaConfig_ModemPreset homePreset = session->pendingHomePreset;

            session->pendingNumPresets = 0;
            char parseBuf[241];
            strncpy(parseBuf, input.c_str(), sizeof(parseBuf) - 1);
            parseBuf[sizeof(parseBuf) - 1] = '\0';

            char *token = strtok(parseBuf, " \t");
            while (token && session->pendingNumPresets < MAX_BROADCAST_PRESETS) {
                int num = atoi(token);
                // Numbers are 1-indexed in the display, but preset enum is 0-indexed
                int presetIdx = num - 1;
                if (presetIdx >= 0 && presetIdx <= _meshtastic_Config_LoRaConfig_ModemPreset_MAX &&
                    (meshtastic_Config_LoRaConfig_ModemPreset)presetIdx != homePreset) {
                    session->pendingPresets[session->pendingNumPresets++] =
                        (meshtastic_Config_LoRaConfig_ModemPreset)presetIdx;
                }
                token = strtok(nullptr, " \t");
            }

            if (session->pendingNumPresets == 0) {
                sendReply(toNode, "No valid cycle presets selected. Try again:");
                return;
            }
        }
        session->state = SessionState::CFG_AWAIT_INTERVAL;
        promptForField(session, toNode);
        break;
    }

    case SessionState::CFG_AWAIT_INTERVAL: {
        if (!accepted) {
            int minutes = atoi(input.c_str());
            if (minutes <= 0) {
                sendReply(toNode, "Interval must be a positive number of minutes:");
                return;
            }
            session->pendingIntervalMinutes = (uint32_t)minutes;
        }
        session->state = SessionState::CFG_AWAIT_SEND_POSITION;
        promptForField(session, toNode);
        break;
    }

    case SessionState::CFG_AWAIT_SEND_POSITION: {
        if (!accepted) {
            char c = input.length() > 0 ? tolower(input[0]) : 0;
            if (c == 'y') {
                session->pendingSendPosition = true;
            } else if (c == 'n') {
                session->pendingSendPosition = false;
            } else {
                sendReply(toNode, "Reply 'y' or 'n' (or '.' to keep, '!' to abort):");
                return;
            }
        }
        session->state = SessionState::CFG_AWAIT_SKIP_HOME_MESSAGES;
        promptForField(session, toNode);
        break;
    }

    case SessionState::CFG_AWAIT_SKIP_HOME_MESSAGES: {
        if (!accepted) {
            char c = input.length() > 0 ? tolower(input[0]) : 0;
            if (c == 'y') {
                session->pendingSkipHomeMessages = true;
            } else if (c == 'n') {
                session->pendingSkipHomeMessages = false;
            } else {
                sendReply(toNode, "Reply 'y' or 'n' (or '.' to keep, '!' to abort):");
                return;
            }
        }
        session->state = SessionState::CFG_CONFIRM;
        promptForField(session, toNode);
        break;
    }

    case SessionState::CFG_CONFIRM: {
        if (accepted) {
            finalizeConfig(session, toNode);
        } else {
            sendReply(toNode, "'.' to confirm, '!' to abort");
        }
        break;
    }

    default:
        clearSession(session);
        break;
    }
}

// ========== Prompt Display ==========

void BroadcastBeaconManager::promptForField(UserSession *session, uint32_t toNode)
{
    char buf[MAX_REPLY_LEN + 1];

    switch (session->state) {
    case SessionState::MSG_AWAIT_BODY:
        if (session->isEdit && strlen(session->body) > 0) {
            // Body can be up to 237B; send it as its own packet so the follow-up
            // instructions don't push us over the PKI DM size limit.
            sendReply(toNode, session->body);
            snprintf(buf, sizeof(buf), "'.' to keep, '!' to abort, or send new body:");
        } else {
            snprintf(buf, sizeof(buf), "Enter message text ('!' to abort):");
        }
        break;

    case SessionState::MSG_AWAIT_CHANNEL: {
        int pos = snprintf(buf, sizeof(buf),
                           "Select channels (space-separated, * = current):\n");
        int numCh = channels.getNumChannels();
        for (int i = 0; i < numCh && pos < (int)sizeof(buf) - 30; i++) {
            const meshtastic_Channel &ch = channels.getByIndex(i);
            if (ch.role == meshtastic_Channel_Role_DISABLED || !ch.has_settings) {
                continue;
            }
            // Use the raw settings name -- channels.getName() substitutes the
            // current preset name for an unnamed primary, but our cycle changes
            // the preset so that label isn't stable.
            const char *rawName = ch.settings.name;
            const char *displayName;
            if (*rawName) {
                displayName = rawName;
            } else if (i == 0) {
                displayName = "PRIMARY";
            } else {
                continue; // unnamed non-primary -- skip
            }
            bool selected = false;
            for (int j = 0; j < session->numChannels; j++) {
                if (!*rawName && !*session->channels[j]) {
                    selected = true; // both empty = primary
                    break;
                }
                if (*rawName && strcasecmp(rawName, session->channels[j]) == 0) {
                    selected = true;
                    break;
                }
            }
            pos += snprintf(buf + pos, sizeof(buf) - pos, "%2d%s %s\n",
                            i + 1, selected ? "*" : " ", displayName);
        }
        pos += snprintf(buf + pos, sizeof(buf) - pos, "'.' to keep, '!' to abort:");
        break;
    }

    case SessionState::MSG_AWAIT_DATE_FROM:
        if (strlen(session->dateFrom) > 0) {
            snprintf(buf, sizeof(buf),
                     "Start date (ISO: YYYY-MM-DD HH:MM:SS)\nCurrent: %s\n'.' keep, '-' clear, '!' abort:",
                     session->dateFrom);
        } else {
            snprintf(buf, sizeof(buf),
                     "Start date (ISO: YYYY-MM-DD HH:MM:SS)\n'.' for no start (now), '!' to abort:");
        }
        break;

    case SessionState::MSG_AWAIT_DATE_TO:
        if (strlen(session->dateTo) > 0) {
            snprintf(buf, sizeof(buf),
                     "End date (ISO: YYYY-MM-DD HH:MM:SS)\nCurrent: %s\n'.' keep, '-' clear, '!' abort:",
                     session->dateTo);
        } else {
            snprintf(buf, sizeof(buf),
                     "End date (ISO: YYYY-MM-DD HH:MM:SS)\n'.' for no expiry, '!' to abort:");
        }
        break;

    case SessionState::MSG_AWAIT_HOPS:
        if (session->hops != BB_HOP_LIMIT_DEFAULT) {
            snprintf(buf, sizeof(buf),
                     "Hops (0-7)\nCurrent: %d\n'.' keep, '-' device default, '!' abort:", session->hops);
        } else {
            snprintf(buf, sizeof(buf), "Hops (0-7)\n'.' for device default, '!' to abort:");
        }
        break;

    case SessionState::MSG_CONFIRM: {
        char hopsStr[8];
        if (session->hops == BB_HOP_LIMIT_DEFAULT) {
            strncpy(hopsStr, "default", sizeof(hopsStr));
        } else {
            snprintf(hopsStr, sizeof(hopsStr), "%d", session->hops);
        }
        char channelList[MAX_CHANNELS_PER_MESSAGE * (CHANNEL_NAME_LEN + 2) + 1];
        formatChannelList(channelList, sizeof(channelList), session->channels, session->numChannels);
        // Body was already confirmed at AWAIT_BODY; omit it here to stay under
        // the PKI DM size limit.
        snprintf(buf, sizeof(buf),
                 "Summary:\nChannels: %s\nFrom: %s\nTo: %s\nHops: %s\n'.' to confirm, '!' to abort",
                 channelList,
                 strlen(session->dateFrom) > 0 ? session->dateFrom : "(now)",
                 strlen(session->dateTo) > 0 ? session->dateTo : "(no expiry)",
                 hopsStr);
        break;
    }

    case SessionState::MSG_AWAIT_ANOTHER:
        snprintf(buf, sizeof(buf), "Message saved. Add another? (y/n)");
        break;

    case SessionState::CFG_AWAIT_HOME_PRESET: {
        int pos = snprintf(buf, sizeof(buf),
                           "Home preset (node returns here between cycles, * = current):\n");
        for (int i = 0; i <= _meshtastic_Config_LoRaConfig_ModemPreset_MAX && pos < (int)sizeof(buf) - 30; i++) {
            meshtastic_Config_LoRaConfig_ModemPreset preset = (meshtastic_Config_LoRaConfig_ModemPreset)i;
            const char *name = presetDisplayName(preset);
            if (!name || !*name || strcmp(name, "Invalid") == 0) {
                continue;
            }
            bool selected = preset == session->pendingHomePreset;
            pos += snprintf(buf + pos, sizeof(buf) - pos, "%2d%s %s\n",
                            i + 1, selected ? "*" : " ", name);
        }
        pos += snprintf(buf + pos, sizeof(buf) - pos, "'.' to keep, '!' to abort:");
        break;
    }

    case SessionState::CFG_AWAIT_PRESETS: {
        // Home preset is always in the cycle (so the node is reachable on its
        // original network at least once per rotation). The picker shows only
        // the "additional" presets the operator can choose to broadcast on.
        meshtastic_Config_LoRaConfig_ModemPreset homePreset = session->pendingHomePreset;

        int pos = snprintf(buf, sizeof(buf),
                           "Select cycle presets (home %s auto-included)\n"
                           "space-separated numbers, * = current:\n",
                           presetDisplayName(homePreset));
        for (int i = 0; i <= _meshtastic_Config_LoRaConfig_ModemPreset_MAX && pos < (int)sizeof(buf) - 30; i++) {
            meshtastic_Config_LoRaConfig_ModemPreset preset = (meshtastic_Config_LoRaConfig_ModemPreset)i;
            if (preset == homePreset) {
                continue; // home is implicit
            }
            const char *name = presetDisplayName(preset);
            if (!name || !*name || strcmp(name, "Invalid") == 0) {
                continue;
            }
            bool selected = false;
            for (int j = 0; j < broadcastConfig.numPresets; j++) {
                if (broadcastConfig.presets[j] == preset) {
                    selected = true;
                    break;
                }
            }
            pos += snprintf(buf + pos, sizeof(buf) - pos, "%2d%s %s\n",
                            i + 1, selected ? "*" : " ", name);
        }
        pos += snprintf(buf + pos, sizeof(buf) - pos, "'.' to keep, '!' to abort:");
        break;
    }

    case SessionState::CFG_AWAIT_INTERVAL:
        if (broadcastConfig.intervalMinutes > 0) {
            snprintf(buf, sizeof(buf), "Broadcast interval in minutes\nCurrent: %d\n'.' to keep, '!' to abort:",
                     broadcastConfig.intervalMinutes);
        } else {
            snprintf(buf, sizeof(buf), "Broadcast interval in minutes ('!' to abort):");
        }
        break;

    case SessionState::CFG_AWAIT_SEND_POSITION:
        snprintf(buf, sizeof(buf),
                 "Also broadcast our position on each preset switch?\nCurrent: %s\n'y' yes, 'n' no, '.' to keep, '!' to abort:",
                 broadcastConfig.sendPosition ? "yes" : "no");
        break;

    case SessionState::CFG_AWAIT_SKIP_HOME_MESSAGES:
        snprintf(buf, sizeof(buf),
                 "Skip text messages on the home preset? (home still sends NodeInfo/position)\nCurrent: %s\n'y' yes, 'n' no, '.' to keep, '!' to abort:",
                 broadcastConfig.skipHomeMessages ? "yes" : "no");
        break;

    case SessionState::CFG_CONFIRM: {
        meshtastic_Config_LoRaConfig_ModemPreset homePreset = session->pendingHomePreset;
        uint8_t effectiveCount = session->pendingNumPresets + 1; // + home

        int pos = snprintf(buf, sizeof(buf), "Config summary:\nPresets: %s(home)",
                           presetDisplayName(homePreset));
        for (int i = 0; i < session->pendingNumPresets && pos < (int)sizeof(buf) - 20; i++) {
            pos += snprintf(buf + pos, sizeof(buf) - pos, " %s", presetDisplayName(session->pendingPresets[i]));
        }
        pos += snprintf(buf + pos, sizeof(buf) - pos, "\nInterval: %d min", session->pendingIntervalMinutes);
        pos += snprintf(buf + pos, sizeof(buf) - pos, "\nWindow: %d min per preset",
                        session->pendingIntervalMinutes / effectiveCount);
        pos += snprintf(buf + pos, sizeof(buf) - pos, "\nPosition: %s",
                        session->pendingSendPosition ? "yes" : "no");
        pos += snprintf(buf + pos, sizeof(buf) - pos, "\nSkip msgs on home: %s",
                        session->pendingSkipHomeMessages ? "yes" : "no");
        pos += snprintf(buf + pos, sizeof(buf) - pos, "\n'.' to confirm, '!' to abort");
        break;
    }

    default:
        return;
    }

    sendReply(toNode, buf);
}

// ========== Finalize Message ==========

void BroadcastBeaconManager::finalizeMessage(UserSession *session, uint32_t toNode)
{
    if (session->isEdit) {
        if (session->editIndex >= numMessages) {
            sendReply(toNode, "Message no longer exists.");
            clearSession(session);
            return;
        }

        BroadcastMessage &entry = messages[session->editIndex];
        strncpy(entry.body, session->body, sizeof(entry.body) - 1);
        entry.body[sizeof(entry.body) - 1] = '\0';
        memcpy(entry.channels, session->channels, sizeof(entry.channels));
        entry.numChannels = session->numChannels;
        strncpy(entry.dateFrom, session->dateFrom, sizeof(entry.dateFrom) - 1);
        entry.dateFrom[sizeof(entry.dateFrom) - 1] = '\0';
        strncpy(entry.dateTo, session->dateTo, sizeof(entry.dateTo) - 1);
        entry.dateTo[sizeof(entry.dateTo) - 1] = '\0';
        entry.hops = session->hops;
        if (!saveMessages()) {
            sendReply(toNode, "Update failed to persist. Check device storage.");
            clearSession(session);
            return;
        }
        sendReply(toNode, "Message updated.");
    } else {
        if (numMessages >= MAX_BROADCAST_MESSAGES) {
            sendReply(toNode, "Message limit reached.");
            clearSession(session);
            return;
        }

        time_t now = getTime();

        // Empty dateFrom / dateTo mean "active immediately" / "no expiry" --
        // leave them empty so they aren't shown as concrete timestamps. isMessageActive() treats
        // empty bounds as unbounded.

        BroadcastMessage &entry = messages[numMessages];
        memset(&entry, 0, sizeof(BroadcastMessage));
        entry.createdAt = (uint32_t)now;
        entry.hops = session->hops;
        strncpy(entry.body, session->body, sizeof(entry.body) - 1);
        memcpy(entry.channels, session->channels, sizeof(entry.channels));
        entry.numChannels = session->numChannels;
        strncpy(entry.dateFrom, session->dateFrom, sizeof(entry.dateFrom) - 1);
        strncpy(entry.dateTo, session->dateTo, sizeof(entry.dateTo) - 1);
        entry.id = hashMessageId(entry.body, entry.createdAt);
        numMessages++;
        if (!saveMessages()) {
            // Roll the in-memory change back so retrying isn't confused.
            numMessages--;
            memset(&messages[numMessages], 0, sizeof(BroadcastMessage));
            sendReply(toNode, "Create failed to persist. Check device storage.");
            clearSession(session);
            return;
        }
        sendReply(toNode, "Message created.");
    }

    // Transition to ask about adding another
    session->state = SessionState::MSG_AWAIT_ANOTHER;
    promptForField(session, toNode);
}

// ========== Finalize Config ==========

void BroadcastBeaconManager::finalizeConfig(UserSession *session, uint32_t toNode)
{
    meshtastic_Config_LoRaConfig_ModemPreset homePreset = session->pendingHomePreset;

    // Prepend home preset as slot 0 (the picker filters it out, but guard anyway).
    meshtastic_Config_LoRaConfig_ModemPreset newPresets[MAX_BROADCAST_PRESETS] = {};
    uint8_t newCount = 0;
    newPresets[newCount++] = homePreset;
    for (int i = 0; i < session->pendingNumPresets && newCount < MAX_BROADCAST_PRESETS; i++) {
        if (session->pendingPresets[i] != homePreset) {
            newPresets[newCount++] = session->pendingPresets[i];
        }
    }

    // Apply config
    broadcastConfig.homePreset = homePreset;
    broadcastConfig.homePresetValid = true;
    broadcastConfig.numPresets = newCount;
    memcpy(broadcastConfig.presets, newPresets, sizeof(broadcastConfig.presets));
    broadcastConfig.intervalMinutes = session->pendingIntervalMinutes;
    broadcastConfig.sendPosition = session->pendingSendPosition;
    broadcastConfig.skipHomeMessages = session->pendingSkipHomeMessages;
    if (!saveConfig()) {
        sendReply(toNode, "Config failed to persist. Check device storage.");
        clearSession(session);
        return;
    }

    sendReplyFmt(toNode, "Config saved. %d presets, %d min interval (%d min per preset).",
                 broadcastConfig.numPresets,
                 broadcastConfig.intervalMinutes,
                 broadcastConfig.numPresets > 0
                     ? broadcastConfig.intervalMinutes / broadcastConfig.numPresets
                     : 0);
    clearSession(session);
}

// ========== State Mutators ==========

void BroadcastBeaconManager::setCurrentPresetIndex(uint8_t idx)
{
    broadcastState.currentPresetIndex = idx;
}

void BroadcastBeaconManager::setBroadcasting(bool active)
{
    broadcastState.broadcasting = active;
}

void BroadcastBeaconManager::clearBroadcastingState()
{
    broadcastState.broadcasting = false;
    broadcastState.currentPresetIndex = 0;
    saveState();
}

// ========== Messaging ==========

void BroadcastBeaconManager::sendReply(uint32_t toNodeNum, const char *text)
{
    if (!text || !router || !service) {
        return;
    }

    LOG_INFO("[BroadcastBeacon] TX reply to 0x%x (%u bytes): %.60s%s",
             toNodeNum, (unsigned)strlen(text), text, strlen(text) > 60 ? "..." : "");

    // PKI-encrypted DMs have a ~220-byte on-air text limit; chunk longer text
    // into multiple packets rather than truncating.
    size_t totalLen = strlen(text);
    size_t offset = 0;
    do {
        size_t remaining = totalLen - offset;
        size_t chunkLen = remaining > MAX_PACKET_TEXT_LEN ? MAX_PACKET_TEXT_LEN : remaining;

        meshtastic_MeshPacket *p = router->allocForSending();
        if (!p) {
            LOG_ERROR("[BroadcastBeacon] Failed to allocate packet");
            return;
        }

        p->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
        p->to = toNodeNum;
        p->from = nodeDB->getNodeNum();
        p->channel = 0;
        p->want_ack = false;
        p->decoded.want_response = false;

        p->decoded.payload.size = chunkLen;
        memcpy(p->decoded.payload.bytes, text + offset, chunkLen);

        service->sendToMesh(p);
        offset += chunkLen;
    } while (offset < totalLen);
}

void BroadcastBeaconManager::sendReplyFmt(uint32_t toNodeNum, const char *fmt, ...)
{
    char buf[MAX_REPLY_LEN + 1];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    sendReply(toNodeNum, buf);
}

void BroadcastBeaconManager::sendDoneWithHint(uint32_t toNodeNum)
{
    bool needsConfig = !broadcastConfig.homePresetValid || broadcastConfig.numPresets == 0;
    bool needsOn = !needsConfig && !broadcastConfig.enabled;
    if (needsConfig) {
        sendReply(toNodeNum, "Done.\nNote: run '/bb config' to set up the cycle.");
    } else if (needsOn) {
        sendReply(toNodeNum, "Done.\nNote: run '/bb on' to start broadcasting.");
    } else {
        sendReply(toNodeNum, "Done.");
    }
}

// ========== Storage ==========

bool BroadcastBeaconManager::loadConfig()
{
    concurrency::LockGuard g(spiLock);

    if (!FSCom.exists(CONFIG_FILE)) {
        return true;
    }

    File f = FSCom.open(CONFIG_FILE, FILE_O_READ);
    if (!f) {
        return false;
    }

    StorageHeader header;
    if (f.read((uint8_t *)&header, sizeof(header)) != sizeof(header) ||
        header.magic != CONFIG_MAGIC) {
        f.close();
        FSCom.remove(CONFIG_FILE);
        return false;
    }

    // Tolerant read: zero the target first so missing tail bytes stay 0, then
    // read up to sizeof(). Short reads (older, smaller struct) are accepted;
    // any bytes beyond sizeof() in a larger on-disk record are ignored.
    // Requires discipline: BroadcastConfig fields are append-only.
    memset(&broadcastConfig, 0, sizeof(BroadcastConfig));
    int got = f.read((uint8_t *)&broadcastConfig, sizeof(BroadcastConfig));
    f.close();
    if (got <= 0) {
        FSCom.remove(CONFIG_FILE);
        memset(&broadcastConfig, 0, sizeof(BroadcastConfig));
        return false;
    }
    if ((size_t)got < sizeof(BroadcastConfig)) {
        LOG_INFO("[BroadcastBeacon] Config short read (%d/%u bytes) -- new fields defaulted",
                 got, (unsigned)sizeof(BroadcastConfig));
    }
    return true;
}

bool BroadcastBeaconManager::saveConfig()
{
    // IMPORTANT: renameFile() acquires spiLock internally (directly on ESP32,
    // via copyFile() on nrf52/other LittleFS platforms). We MUST release our
    // own spiLock before calling it, otherwise we recursive-lock and the node
    // dies. Keep all FSCom operations inside the inner scope; renameFile
    // happens after it closes.
    FSCom.mkdir(STORAGE_DIR); // idempotent; open()/write() silently fail without this
    {
        concurrency::LockGuard g(spiLock);

        File f = FSCom.open(CONFIG_FILE_TMP, FILE_O_WRITE);
        if (!f) {
            LOG_ERROR("[BroadcastBeacon] saveConfig: failed to open %s for writing", CONFIG_FILE_TMP);
            return false;
        }

        StorageHeader header = {};
        header.magic = CONFIG_MAGIC;
        header.count = 1;
        f.write((const uint8_t *)&header, sizeof(header));
        f.write((const uint8_t *)&broadcastConfig, sizeof(BroadcastConfig));

        f.flush();
        f.close();

        FSCom.remove(CONFIG_FILE);
    }
    bool ok = renameFile(CONFIG_FILE_TMP, CONFIG_FILE);
    if (ok) {
        LOG_INFO("[BroadcastBeacon] Config saved");
    } else {
        LOG_ERROR("[BroadcastBeacon] saveConfig: rename failed");
    }
    return ok;
}

bool BroadcastBeaconManager::loadMessages()
{
    concurrency::LockGuard g(spiLock);
    numMessages = 0;

    if (!FSCom.exists(MESSAGES_FILE)) {
        return true;
    }

    File f = FSCom.open(MESSAGES_FILE, FILE_O_READ);
    if (!f) {
        return false;
    }

    size_t fileSize = f.size();

    StorageHeader header;
    if (f.read((uint8_t *)&header, sizeof(header)) != sizeof(header) ||
        header.magic != MESSAGES_MAGIC) {
        f.close();
        FSCom.remove(MESSAGES_FILE);
        return false;
    }

    int count = (header.count < MAX_BROADCAST_MESSAGES) ? header.count : MAX_BROADCAST_MESSAGES;

    // Work out the on-disk record size. If the struct grew since the file was
    // written, each on-disk record is smaller than sizeof(BroadcastMessage),
    // and we zero-init the tail. If it shrank (shouldn't happen given
    // append-only discipline, but be defensive), we read sizeof() and skip
    // the extra bytes. BroadcastMessage fields are append-only.
    size_t payloadBytes = (fileSize > sizeof(header)) ? fileSize - sizeof(header) : 0;
    size_t recordSize = count > 0 ? (payloadBytes / count) : sizeof(BroadcastMessage);
    if (recordSize == 0) {
        f.close();
        FSCom.remove(MESSAGES_FILE);
        return false;
    }
    size_t readPerRecord = recordSize < sizeof(BroadcastMessage) ? recordSize : sizeof(BroadcastMessage);
    size_t skipPerRecord = recordSize > sizeof(BroadcastMessage) ? (recordSize - sizeof(BroadcastMessage)) : 0;

    if (recordSize != sizeof(BroadcastMessage)) {
        LOG_INFO("[BroadcastBeacon] Messages record size changed (%u on disk vs %u current)",
                 (unsigned)recordSize, (unsigned)sizeof(BroadcastMessage));
    }

    for (int i = 0; i < count; i++) {
        BroadcastMessage entry;
        memset(&entry, 0, sizeof(entry));
        if (f.read((uint8_t *)&entry, readPerRecord) != (int)readPerRecord) {
            break; // truncated file -- stop, keep what we read
        }
        if (skipPerRecord) {
            f.seek(f.position() + skipPerRecord);
        }
        messages[numMessages++] = entry;
    }

    f.close();
    return true;
}

bool BroadcastBeaconManager::saveMessages()
{
    // See saveConfig for the scoped-lock rationale: renameFile takes spiLock.
    FSCom.mkdir(STORAGE_DIR);
    {
        concurrency::LockGuard g(spiLock);

        File f = FSCom.open(MESSAGES_FILE_TMP, FILE_O_WRITE);
        if (!f) {
            LOG_ERROR("[BroadcastBeacon] saveMessages: failed to open %s for writing", MESSAGES_FILE_TMP);
            return false;
        }

        StorageHeader header = {};
        header.magic = MESSAGES_MAGIC;
        header.count = numMessages;
        f.write((const uint8_t *)&header, sizeof(header));

        for (int i = 0; i < numMessages; i++) {
            f.write((const uint8_t *)&messages[i], sizeof(BroadcastMessage));
        }

        f.flush();
        f.close();

        FSCom.remove(MESSAGES_FILE);
    }
    bool ok = renameFile(MESSAGES_FILE_TMP, MESSAGES_FILE);
    if (ok) {
        LOG_INFO("[BroadcastBeacon] Messages saved (%d)", numMessages);
    } else {
        LOG_ERROR("[BroadcastBeacon] saveMessages: rename failed");
    }
    return ok;
}

bool BroadcastBeaconManager::loadState()
{
    concurrency::LockGuard g(spiLock);

    if (!FSCom.exists(STATE_FILE)) {
        return true;
    }

    File f = FSCom.open(STATE_FILE, FILE_O_READ);
    if (!f) {
        return false;
    }

    StorageHeader header;
    if (f.read((uint8_t *)&header, sizeof(header)) != sizeof(header) ||
        header.magic != STATE_MAGIC) {
        f.close();
        FSCom.remove(STATE_FILE);
        return false;
    }

    // Tolerant read: see loadConfig() for rationale.
    memset(&broadcastState, 0, sizeof(BroadcastState));
    int got = f.read((uint8_t *)&broadcastState, sizeof(BroadcastState));
    f.close();
    if (got <= 0) {
        FSCom.remove(STATE_FILE);
        memset(&broadcastState, 0, sizeof(BroadcastState));
        return false;
    }
    if ((size_t)got < sizeof(BroadcastState)) {
        LOG_INFO("[BroadcastBeacon] State short read (%d/%u bytes) -- new fields defaulted",
                 got, (unsigned)sizeof(BroadcastState));
    }
    return true;
}

bool BroadcastBeaconManager::saveState()
{
    // See saveConfig for the scoped-lock rationale: renameFile takes spiLock.
    FSCom.mkdir(STORAGE_DIR);
    {
        concurrency::LockGuard g(spiLock);

        File f = FSCom.open(STATE_FILE_TMP, FILE_O_WRITE);
        if (!f) {
            LOG_ERROR("[BroadcastBeacon] saveState: failed to open %s for writing", STATE_FILE_TMP);
            return false;
        }

        StorageHeader header = {};
        header.magic = STATE_MAGIC;
        header.count = 1;
        f.write((const uint8_t *)&header, sizeof(header));
        f.write((const uint8_t *)&broadcastState, sizeof(BroadcastState));

        f.flush();
        f.close();

        FSCom.remove(STATE_FILE);
    }
    bool ok = renameFile(STATE_FILE_TMP, STATE_FILE);
    if (ok) {
        LOG_INFO("[BroadcastBeacon] State saved (broadcasting=%d idx=%d)",
                 broadcastState.broadcasting, broadcastState.currentPresetIndex);
    } else {
        LOG_ERROR("[BroadcastBeacon] saveState: rename failed");
    }
    return ok;
}

// ========== Helpers ==========

uint32_t BroadcastBeaconManager::hashMessageId(const char *body, uint32_t createdAt) const
{
    uint32_t hash = 5381;
    if (body) {
        for (const char *p = body; *p; p++) {
            hash = ((hash << 5) + hash) + (uint8_t)*p;
        }
    }
    hash = ((hash << 5) + hash) + (createdAt & 0xFF);
    hash = ((hash << 5) + hash) + ((createdAt >> 8) & 0xFF);
    hash = ((hash << 5) + hash) + ((createdAt >> 16) & 0xFF);
    hash = ((hash << 5) + hash) + ((createdAt >> 24) & 0xFF);
    return hash;
}

int BroadcastBeaconManager::formatChannelList(char *out, size_t outLen,
                                              const char channels[][CHANNEL_NAME_LEN],
                                              uint8_t count) const
{
    if (count == 0) {
        return snprintf(out, outLen, "PRIMARY");
    }
    int pos = 0;
    for (uint8_t i = 0; i < count && pos < (int)outLen - 1; i++) {
        const char *name = *channels[i] ? channels[i] : "PRIMARY";
        pos += snprintf(out + pos, outLen - pos, "%s%s", i == 0 ? "" : ", ", name);
    }
    return pos;
}

int8_t BroadcastBeaconManager::resolveChannelIndex(const char *channelName) const
{
    if (!channelName || strlen(channelName) == 0) {
        return 0; // primary channel (stored as empty raw name)
    }
    // Match against the raw settings name, not channels.getName(), because the
    // latter substitutes the current preset name for an unnamed primary and our
    // cycle changes the preset between sends.
    int numCh = channels.getNumChannels();
    for (int i = 0; i < numCh; i++) {
        const meshtastic_Channel &ch = channels.getByIndex(i);
        if (ch.role == meshtastic_Channel_Role_DISABLED || !ch.has_settings) {
            continue;
        }
        if (strcasecmp(ch.settings.name, channelName) == 0) {
            return i;
        }
    }
    return -1;
}

const char *BroadcastBeaconManager::presetDisplayName(meshtastic_Config_LoRaConfig_ModemPreset preset) const
{
    return DisplayFormatters::getModemPresetDisplayName(preset, false, true);
}

String BroadcastBeaconManager::unescapeInput(const char *text)
{
    if (!text) {
        return "";
    }
    if (text[0] == '.' && text[1] == '.') {
        return String(text + 1);
    }
    if (text[0] == '!' && text[1] == '!') {
        return String(text + 1);
    }
    return String(text);
}

bool BroadcastBeaconManager::isAccept(const char *text)
{
    return text && text[0] == '.' && text[1] == '\0';
}

bool BroadcastBeaconManager::isAbort(const char *text)
{
    return text && text[0] == '!' && text[1] == '\0';
}

#endif // HAS_BROADCAST_BEACON
