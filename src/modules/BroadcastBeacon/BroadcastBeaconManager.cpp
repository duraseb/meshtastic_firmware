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
    broadcastConfig.enabled = true; // on by default when config exists
}

void BroadcastBeaconManager::loadFromDisk()
{
    loadConfig();
    loadMessages();
    loadState();
    LOG_INFO("[BroadcastBeacon] Loaded config (%d presets, %dmin interval, %s), %d messages, state: %s (idx %d)",
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
        sendReply(mp->from, "Not authorized. Admin PKI key required.");
        return 1;
    }

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
              "BroadcastBeacon commands:\n"
              "/bb on - Enable broadcasting\n"
              "/bb off - Disable, restore preset\n"
              "/bb status - Show state\n"
              "/bb list - List messages\n"
              "/bb list <n> - Show message details\n"
              "/bb create - New message\n"
              "/bb edit <n> - Edit message\n"
              "/bb delete <n> - Delete message\n"
              "/bb config - Set presets/interval");
}

void BroadcastBeaconManager::cmdOn(const meshtastic_MeshPacket *mp)
{
    uint32_t from = mp->from;

    if (broadcastConfig.numPresets == 0) {
        sendReply(from, "No presets configured. Use '/bb config' first.");
        return;
    }
    if (numMessages == 0) {
        sendReply(from, "No messages configured. Use '/bb create' first.");
        return;
    }

    // Save home preset if not already broadcasting
    if (!broadcastState.broadcasting) {
        broadcastState.homePreset = config.lora.modem_preset;
        broadcastState.currentPresetIndex = 0;
        broadcastState.broadcasting = true;
        saveState();
    }

    broadcastConfig.enabled = true;
    saveConfig();

    sendReplyFmt(from, "Broadcasting enabled. Home preset: %s. Cycling %d presets every %d min.",
                 presetDisplayName(broadcastState.homePreset),
                 broadcastConfig.numPresets,
                 broadcastConfig.intervalMinutes);
}

void BroadcastBeaconManager::cmdOff(const meshtastic_MeshPacket *mp)
{
    uint32_t from = mp->from;

    broadcastConfig.enabled = false;
    saveConfig();

    if (broadcastState.broadcasting) {
        meshtastic_Config_LoRaConfig_ModemPreset home = broadcastState.homePreset;
        clearBroadcastingState();

        sendReplyFmt(from, "Broadcasting disabled. Restoring preset %s and rebooting...",
                     presetDisplayName(home));

        // Restore home preset via proper API and reboot
        config.lora.modem_preset = home;
        service->reloadConfig(SEGMENT_CONFIG);
        rebootAtMsec = millis() + 5000;
    } else {
        sendReply(from, "Broadcasting disabled.");
    }
}

void BroadcastBeaconManager::cmdStatus(uint32_t toNode)
{
    char buf[MAX_REPLY_LEN + 1];
    int pos = 0;

    pos += snprintf(buf + pos, sizeof(buf) - pos, "BroadcastBeacon: %s\n",
                    broadcastConfig.enabled ? "ON" : "OFF");

    if (broadcastConfig.numPresets > 0) {
        pos += snprintf(buf + pos, sizeof(buf) - pos, "Interval: %d min\nPresets:",
                        broadcastConfig.intervalMinutes);
        for (int i = 0; i < broadcastConfig.numPresets && pos < (int)sizeof(buf) - 20; i++) {
            pos += snprintf(buf + pos, sizeof(buf) - pos, " %s%s",
                            presetDisplayName(broadcastConfig.presets[i]),
                            (i == broadcastState.currentPresetIndex && broadcastState.broadcasting) ? "*" : "");
        }
        pos += snprintf(buf + pos, sizeof(buf) - pos, "\n");
    } else {
        pos += snprintf(buf + pos, sizeof(buf) - pos, "No presets configured.\n");
    }

    pos += snprintf(buf + pos, sizeof(buf) - pos, "Messages: %d", numMessages);

    if (broadcastState.broadcasting) {
        unsigned long windowMs = 0;
        if (broadcastConfig.numPresets > 0) {
            windowMs = ((unsigned long)broadcastConfig.intervalMinutes * 60UL * 1000UL) / broadcastConfig.numPresets;
        }
        unsigned long elapsed = millis();
        unsigned long remaining = (elapsed < windowMs) ? (windowMs - elapsed) / 1000 : 0;
        pos += snprintf(buf + pos, sizeof(buf) - pos, "\nHome: %s\nNext switch: ~%lus",
                        presetDisplayName(broadcastState.homePreset), remaining);
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

    for (int i = 0; i < numMessages && pos < (int)sizeof(buf) - 40; i++) {
        pos += snprintf(buf + pos, sizeof(buf) - pos, "%d. %.40s%s [%s]\n",
                        i + 1,
                        messages[i].body,
                        strlen(messages[i].body) > 40 ? "..." : "",
                        strlen(messages[i].channel) > 0 ? messages[i].channel : "default");
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

    char header[MAX_REPLY_LEN + 1];
    snprintf(header, sizeof(header),
             "#%d hops:%s\nFrom: %s\nTo: %s\nCh: %s",
             num, hopsStr,
             strlen(m.dateFrom) > 0 ? m.dateFrom : "(now)",
             strlen(m.dateTo) > 0 ? m.dateTo : "(no expiry)",
             strlen(m.channel) > 0 ? m.channel : "(primary)");

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
    strncpy(session->channel, msg.channel, sizeof(session->channel) - 1);
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
    saveMessages();

    sendReplyFmt(mp->from, "Message %d deleted. %d remaining.", num, numMessages);
}

void BroadcastBeaconManager::cmdConfig(const meshtastic_MeshPacket *mp)
{
    UserSession *session = allocateSession(mp->from);
    if (!session) {
        sendReply(mp->from, "Too many active sessions. Try later.");
        return;
    }

    session->state = SessionState::CFG_AWAIT_PRESETS;
    // Pre-populate with current config
    session->pendingNumPresets = broadcastConfig.numPresets;
    memcpy(session->pendingPresets, broadcastConfig.presets, sizeof(session->pendingPresets));
    session->pendingIntervalMinutes = broadcastConfig.intervalMinutes;

    promptForField(session, mp->from);
}

// ========== Session Input Handler ==========

void BroadcastBeaconManager::handleSessionInput(const meshtastic_MeshPacket *mp, const char *text, UserSession *session)
{
    session->lastActivityMs = millis();
    uint32_t toNode = mp->from;

    // Check for abort
    if (isAbort(text)) {
        sendReply(toNode, "Cancelled.");
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
            int chNum = atoi(input.c_str());
            int numCh = channels.getNumChannels();
            if (chNum < 1 || chNum > numCh) {
                sendReplyFmt(toNode, "Invalid channel number. Enter 1-%d:", numCh);
                return;
            }
            const char *name = channels.getName(chNum - 1);
            strncpy(session->channel, name ? name : "", sizeof(session->channel) - 1);
            session->channel[sizeof(session->channel) - 1] = '\0';
        }
        session->state = SessionState::MSG_AWAIT_DATE_FROM;
        promptForField(session, toNode);
        break;
    }

    case SessionState::MSG_AWAIT_DATE_FROM: {
        if (!accepted) {
            strncpy(session->dateFrom, input.c_str(), sizeof(session->dateFrom) - 1);
            session->dateFrom[sizeof(session->dateFrom) - 1] = '\0';
        }
        session->state = SessionState::MSG_AWAIT_DATE_TO;
        promptForField(session, toNode);
        break;
    }

    case SessionState::MSG_AWAIT_DATE_TO: {
        if (!accepted) {
            strncpy(session->dateTo, input.c_str(), sizeof(session->dateTo) - 1);
            session->dateTo[sizeof(session->dateTo) - 1] = '\0';
        }
        session->state = SessionState::MSG_AWAIT_HOPS;
        promptForField(session, toNode);
        break;
    }

    case SessionState::MSG_AWAIT_HOPS: {
        if (!accepted) {
            int h = atoi(input.c_str());
            if (h < 0 || h > 7) {
                sendReply(toNode, "Hops must be 0-7. Try again:");
                return;
            }
            session->hops = (uint8_t)h;
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
        if (accepted) {
            // Start another message
            memset(session->body, 0, sizeof(session->body));
            memset(session->channel, 0, sizeof(session->channel));
            memset(session->dateFrom, 0, sizeof(session->dateFrom));
            memset(session->dateTo, 0, sizeof(session->dateTo));
            session->hops = BB_HOP_LIMIT_DEFAULT;
            session->isEdit = false;
            session->state = SessionState::MSG_AWAIT_BODY;
            promptForField(session, toNode);
        } else {
            sendReply(toNode, "Done.");
            clearSession(session);
        }
        break;
    }

    // ===== Config States =====

    case SessionState::CFG_AWAIT_PRESETS: {
        if (!accepted) {
            // Parse comma-separated preset numbers
            session->pendingNumPresets = 0;
            char parseBuf[241];
            strncpy(parseBuf, input.c_str(), sizeof(parseBuf) - 1);
            parseBuf[sizeof(parseBuf) - 1] = '\0';

            char *token = strtok(parseBuf, ",");
            while (token && session->pendingNumPresets < MAX_BROADCAST_PRESETS) {
                while (*token == ' ') {
                    token++;
                }
                int num = atoi(token);
                // Numbers are 1-indexed in the display, but preset enum is 0-indexed
                int presetIdx = num - 1;
                if (presetIdx >= 0 && presetIdx <= _meshtastic_Config_LoRaConfig_ModemPreset_MAX) {
                    session->pendingPresets[session->pendingNumPresets++] =
                        (meshtastic_Config_LoRaConfig_ModemPreset)presetIdx;
                }
                token = strtok(nullptr, ",");
            }

            if (session->pendingNumPresets == 0) {
                sendReply(toNode, "No valid presets selected. Try again:");
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
        int pos = snprintf(buf, sizeof(buf), "Select channel:\n");
        int numCh = channels.getNumChannels();
        for (int i = 0; i < numCh && pos < (int)sizeof(buf) - 30; i++) {
            const char *name = channels.getName(i);
            if (name && *name) {
                pos += snprintf(buf + pos, sizeof(buf) - pos, "%d. %s\n", i + 1, name);
            }
        }
        if (strlen(session->channel) > 0) {
            pos += snprintf(buf + pos, sizeof(buf) - pos, "Current: %s\n'.' to keep, '!' to abort:", session->channel);
        } else {
            pos += snprintf(buf + pos, sizeof(buf) - pos, "'.' for primary, '!' to abort:");
        }
        break;
    }

    case SessionState::MSG_AWAIT_DATE_FROM:
        if (strlen(session->dateFrom) > 0) {
            snprintf(buf, sizeof(buf), "Start date (ISO: YYYY-MM-DD HH:MM:SS)\nCurrent: %s\n'.' to keep, '!' to abort:",
                     session->dateFrom);
        } else {
            snprintf(buf, sizeof(buf), "Start date (ISO: YYYY-MM-DD HH:MM:SS)\n'.' for now, '!' to abort:");
        }
        break;

    case SessionState::MSG_AWAIT_DATE_TO:
        if (strlen(session->dateTo) > 0) {
            snprintf(buf, sizeof(buf), "End date (ISO: YYYY-MM-DD HH:MM:SS)\nCurrent: %s\n'.' to keep, '!' to abort:",
                     session->dateTo);
        } else {
            snprintf(buf, sizeof(buf), "End date (ISO: YYYY-MM-DD HH:MM:SS)\n'.' for no expiry, '!' to abort:");
        }
        break;

    case SessionState::MSG_AWAIT_HOPS:
        if (session->hops != BB_HOP_LIMIT_DEFAULT) {
            snprintf(buf, sizeof(buf), "Hops (0-7)\nCurrent: %d\n'.' to keep, '!' to abort:", session->hops);
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
        // Body was already confirmed at AWAIT_BODY; omit it here to stay under
        // the PKI DM size limit.
        snprintf(buf, sizeof(buf),
                 "Summary:\nChannel: %s\nFrom: %s\nTo: %s\nHops: %s\n'.' to confirm, '!' to abort",
                 strlen(session->channel) > 0 ? session->channel : "(primary)",
                 strlen(session->dateFrom) > 0 ? session->dateFrom : "(now)",
                 strlen(session->dateTo) > 0 ? session->dateTo : "(no expiry)",
                 hopsStr);
        break;
    }

    case SessionState::MSG_AWAIT_ANOTHER:
        snprintf(buf, sizeof(buf), "Message saved. Add another? ('.' yes, '!' no)");
        break;

    case SessionState::CFG_AWAIT_PRESETS: {
        int pos = snprintf(buf, sizeof(buf), "Select presets (comma-separated numbers):\n");
        // Show all valid presets
        for (int i = 0; i <= _meshtastic_Config_LoRaConfig_ModemPreset_MAX && pos < (int)sizeof(buf) - 30; i++) {
            const char *name = presetDisplayName((meshtastic_Config_LoRaConfig_ModemPreset)i);
            if (name && *name) {
                pos += snprintf(buf + pos, sizeof(buf) - pos, "%d. %s\n", i + 1, name);
            }
        }
        if (broadcastConfig.numPresets > 0) {
            pos += snprintf(buf + pos, sizeof(buf) - pos, "Current:");
            for (int i = 0; i < broadcastConfig.numPresets && pos < (int)sizeof(buf) - 20; i++) {
                pos += snprintf(buf + pos, sizeof(buf) - pos, " %s", presetDisplayName(broadcastConfig.presets[i]));
            }
            pos += snprintf(buf + pos, sizeof(buf) - pos, "\n'.' to keep:");
        }
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

    case SessionState::CFG_CONFIRM: {
        int pos = snprintf(buf, sizeof(buf), "Config summary:\nPresets:");
        for (int i = 0; i < session->pendingNumPresets && pos < (int)sizeof(buf) - 20; i++) {
            pos += snprintf(buf + pos, sizeof(buf) - pos, " %s", presetDisplayName(session->pendingPresets[i]));
        }
        pos += snprintf(buf + pos, sizeof(buf) - pos, "\nInterval: %d min", session->pendingIntervalMinutes);
        pos += snprintf(buf + pos, sizeof(buf) - pos, "\nWindow: %d min per preset",
                        session->pendingNumPresets > 0
                            ? session->pendingIntervalMinutes / session->pendingNumPresets
                            : 0);
        pos += snprintf(buf + pos, sizeof(buf) - pos, "\nHome preset auto-included.\n'.' to confirm, '!' to abort");
        break;
    }

    default:
        return;
    }

    sendReply(toNode, buf);
}

// ========== Finalize Message ==========

static void formatTimestamp(char *buf, size_t bufLen, time_t t)
{
    struct tm timeinfo;
    gmtime_r(&t, &timeinfo);
    snprintf(buf, bufLen, "%04d-%02d-%02d %02d:%02d:%02d",
             timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
             timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
}

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
        strncpy(entry.channel, session->channel, sizeof(entry.channel) - 1);
        entry.channel[sizeof(entry.channel) - 1] = '\0';
        strncpy(entry.dateFrom, session->dateFrom, sizeof(entry.dateFrom) - 1);
        entry.dateFrom[sizeof(entry.dateFrom) - 1] = '\0';
        strncpy(entry.dateTo, session->dateTo, sizeof(entry.dateTo) - 1);
        entry.dateTo[sizeof(entry.dateTo) - 1] = '\0';
        entry.hops = session->hops;
        saveMessages();

        sendReply(toNode, "Message updated.");
    } else {
        if (numMessages >= MAX_BROADCAST_MESSAGES) {
            sendReply(toNode, "Message limit reached.");
            clearSession(session);
            return;
        }

        time_t now = getTime();

        // Fill defaults for empty date fields
        if (strlen(session->dateFrom) == 0) {
            if (now > 0) {
                formatTimestamp(session->dateFrom, sizeof(session->dateFrom), now);
            }
        }

        BroadcastMessage &entry = messages[numMessages];
        memset(&entry, 0, sizeof(BroadcastMessage));
        entry.createdAt = (uint32_t)now;
        entry.hops = session->hops;
        strncpy(entry.body, session->body, sizeof(entry.body) - 1);
        strncpy(entry.channel, session->channel, sizeof(entry.channel) - 1);
        strncpy(entry.dateFrom, session->dateFrom, sizeof(entry.dateFrom) - 1);
        strncpy(entry.dateTo, session->dateTo, sizeof(entry.dateTo) - 1);
        entry.id = hashMessageId(entry.body, entry.createdAt);
        numMessages++;
        saveMessages();

        sendReply(toNode, "Message created.");
    }

    // Transition to ask about adding another
    session->state = SessionState::MSG_AWAIT_ANOTHER;
    promptForField(session, toNode);
}

// ========== Finalize Config ==========

void BroadcastBeaconManager::finalizeConfig(UserSession *session, uint32_t toNode)
{
    // Ensure home preset is included in the list
    meshtastic_Config_LoRaConfig_ModemPreset homePreset = broadcastState.broadcasting
        ? broadcastState.homePreset
        : config.lora.modem_preset;

    bool hasHome = false;
    for (int i = 0; i < session->pendingNumPresets; i++) {
        if (session->pendingPresets[i] == homePreset) {
            hasHome = true;
            break;
        }
    }

    if (!hasHome && session->pendingNumPresets < MAX_BROADCAST_PRESETS) {
        // Prepend home preset as slot 0
        memmove(&session->pendingPresets[1], &session->pendingPresets[0],
                session->pendingNumPresets * sizeof(session->pendingPresets[0]));
        session->pendingPresets[0] = homePreset;
        session->pendingNumPresets++;
    }

    // Apply config
    broadcastConfig.numPresets = session->pendingNumPresets;
    memcpy(broadcastConfig.presets, session->pendingPresets, sizeof(broadcastConfig.presets));
    broadcastConfig.intervalMinutes = session->pendingIntervalMinutes;
    saveConfig();

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

void BroadcastBeaconManager::setHomePreset(meshtastic_Config_LoRaConfig_ModemPreset preset)
{
    broadcastState.homePreset = preset;
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
        header.magic != CONFIG_MAGIC || header.version != STORAGE_VERSION) {
        f.close();
        FSCom.remove(CONFIG_FILE);
        return false;
    }

    if (f.read((uint8_t *)&broadcastConfig, sizeof(BroadcastConfig)) != sizeof(BroadcastConfig)) {
        f.close();
        FSCom.remove(CONFIG_FILE);
        memset(&broadcastConfig, 0, sizeof(BroadcastConfig));
        return false;
    }

    f.close();
    return true;
}

bool BroadcastBeaconManager::saveConfig()
{
    concurrency::LockGuard g(spiLock);

    File f = FSCom.open(CONFIG_FILE_TMP, FILE_O_WRITE);
    if (!f) {
        return false;
    }

    StorageHeader header = {};
    header.magic = CONFIG_MAGIC;
    header.version = STORAGE_VERSION;
    header.count = 1;
    f.write((const uint8_t *)&header, sizeof(header));
    f.write((const uint8_t *)&broadcastConfig, sizeof(BroadcastConfig));

    f.flush();
    f.close();

    FSCom.remove(CONFIG_FILE);
    return renameFile(CONFIG_FILE_TMP, CONFIG_FILE);
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

    StorageHeader header;
    if (f.read((uint8_t *)&header, sizeof(header)) != sizeof(header) ||
        header.magic != MESSAGES_MAGIC || header.version != STORAGE_VERSION) {
        f.close();
        FSCom.remove(MESSAGES_FILE);
        return false;
    }

    int count = (header.count < MAX_BROADCAST_MESSAGES) ? header.count : MAX_BROADCAST_MESSAGES;
    for (int i = 0; i < count; i++) {
        BroadcastMessage entry;
        if (f.read((uint8_t *)&entry, sizeof(entry)) == sizeof(entry)) {
            messages[numMessages++] = entry;
        }
    }

    f.close();
    return true;
}

bool BroadcastBeaconManager::saveMessages()
{
    concurrency::LockGuard g(spiLock);

    File f = FSCom.open(MESSAGES_FILE_TMP, FILE_O_WRITE);
    if (!f) {
        return false;
    }

    StorageHeader header = {};
    header.magic = MESSAGES_MAGIC;
    header.version = STORAGE_VERSION;
    header.count = numMessages;
    f.write((const uint8_t *)&header, sizeof(header));

    for (int i = 0; i < numMessages; i++) {
        f.write((const uint8_t *)&messages[i], sizeof(BroadcastMessage));
    }

    f.flush();
    f.close();

    FSCom.remove(MESSAGES_FILE);
    return renameFile(MESSAGES_FILE_TMP, MESSAGES_FILE);
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
        header.magic != STATE_MAGIC || header.version != STORAGE_VERSION) {
        f.close();
        FSCom.remove(STATE_FILE);
        return false;
    }

    if (f.read((uint8_t *)&broadcastState, sizeof(BroadcastState)) != sizeof(BroadcastState)) {
        f.close();
        FSCom.remove(STATE_FILE);
        memset(&broadcastState, 0, sizeof(BroadcastState));
        return false;
    }

    f.close();
    return true;
}

bool BroadcastBeaconManager::saveState()
{
    concurrency::LockGuard g(spiLock);

    File f = FSCom.open(STATE_FILE_TMP, FILE_O_WRITE);
    if (!f) {
        return false;
    }

    StorageHeader header = {};
    header.magic = STATE_MAGIC;
    header.version = STORAGE_VERSION;
    header.count = 1;
    f.write((const uint8_t *)&header, sizeof(header));
    f.write((const uint8_t *)&broadcastState, sizeof(BroadcastState));

    f.flush();
    f.close();

    FSCom.remove(STATE_FILE);
    return renameFile(STATE_FILE_TMP, STATE_FILE);
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

int8_t BroadcastBeaconManager::resolveChannelIndex(const char *channelName) const
{
    if (!channelName || strlen(channelName) == 0) {
        return 0; // Primary channel
    }
    int numCh = channels.getNumChannels();
    for (int i = 0; i < numCh; i++) {
        const char *name = channels.getName(i);
        if (name && strcasecmp(name, channelName) == 0) {
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
