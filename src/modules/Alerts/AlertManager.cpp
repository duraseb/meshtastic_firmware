#if defined(HAS_ALERTING) && HAS_ALERTING && !MESHTASTIC_EXCLUDE_ALERT_INTERACTIVE

#include "AlertManager.h"
#include "mesh/Router.h"
#include "mesh/MeshService.h"
#include "mesh/NodeDB.h"
#include "mesh/Channels.h"
#include "FSCommon.h"
#include "SPILock.h"
#include "RTC.h"
extern int32_t getTZOffset(); // not declared in RTC.h public surface
#include <cstring>
#include <cstdarg>
#include <cstdio>
#include <ctime>

// ========== Constructor ==========

AlertManager::AlertManager(AlertsModule *module)
    : alertsModule(module), numAllowedUsers(0), numUserAlerts(0)
{
    for (int i = 0; i < MAX_SESSIONS; i++) {
        memset(&sessions[i], 0, sizeof(UserSession));
    }
    memset(allowedUsers, 0, sizeof(allowedUsers));
    memset(userAlerts, 0, sizeof(userAlerts));
}

void AlertManager::loadFromDisk()
{
    loadPermissions();
    loadUserAlerts();
    LOG_INFO("[AlertManager] Loaded %d allowed users, %d user alerts", numAllowedUsers, numUserAlerts);
}

// ========== Observer Callback ==========

int AlertManager::onNotify(const meshtastic_MeshPacket *mp)
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

    // Extract text payload
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

    // Trim leading/trailing whitespace
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

    // '?' message: re-send current prompt if session is active, else send info
    // (space can't be sent through the Meshtastic app, so '?' is used instead)
    if (strcmp(start, "?") == 0) {
        UserSession *session = findSession(mp->from);
        if (session) {
            session->lastActivityMs = millis();
            promptForField(session, mp->from);
        } else {
            cmdInfo(mp->from);
        }
        return 1;
    }

    // Check for active session first
    UserSession *session = findSession(mp->from);
    if (session) {
        handleSessionInput(mp, start, session);
        return 1; // Handled — don't show in normal text UI
    }

    // Determine access level
    AccessLevel access = getAccessLevel(mp);

    // Parse as command
    handleCommand(mp, start, access);

    // Return 1 for known commands to suppress normal text display
    // Return 0 for unknown input so it passes through
    // We always return 1 since handleCommand replies for unknown commands too
    return 1;
}

// ========== Permission Methods ==========

AlertManager::AccessLevel AlertManager::getAccessLevel(const meshtastic_MeshPacket *mp) const
{
    if (isAdmin(mp)) {
        return AccessLevel::ADMIN;
    }
    if (isAllowedUser(mp->from)) {
        return AccessLevel::ALLOWED;
    }
    return AccessLevel::NONE;
}

bool AlertManager::isAdmin(const meshtastic_MeshPacket *mp) const
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

bool AlertManager::isAllowedUser(uint32_t nodeNum) const
{
    for (int i = 0; i < numAllowedUsers; i++) {
        if (allowedUsers[i].nodeNum == nodeNum) {
            return true;
        }
    }
    return false;
}

// ========== Session Methods ==========

AlertManager::UserSession *AlertManager::findSession(uint32_t nodeNum)
{
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (sessions[i].nodeNum == nodeNum) {
            return &sessions[i];
        }
    }
    return nullptr;
}

AlertManager::UserSession *AlertManager::allocateSession(uint32_t nodeNum)
{
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (sessions[i].nodeNum == 0) {
            memset(&sessions[i], 0, sizeof(UserSession));
            sessions[i].nodeNum = nodeNum;
            sessions[i].lastActivityMs = millis();
            sessions[i].severity = 5; // default
            return &sessions[i];
        }
    }
    return nullptr;
}

void AlertManager::clearSession(UserSession *session)
{
    if (session) {
        memset(session, 0, sizeof(UserSession));
    }
}

void AlertManager::expireStaleSessions()
{
    unsigned long now = millis();
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (sessions[i].nodeNum != 0 && (now - sessions[i].lastActivityMs) > SESSION_TIMEOUT_MS) {
            LOG_INFO("[AlertManager] Session expired for node 0x%x", sessions[i].nodeNum);
            clearSession(&sessions[i]);
        }
    }
}

// ========== Command Dispatch ==========

void AlertManager::handleCommand(const meshtastic_MeshPacket *mp, const char *text, AccessLevel access)
{
    uint32_t from = mp->from;

    // Case-insensitive first word comparison
    char lower[241];
    strncpy(lower, text, sizeof(lower) - 1);
    lower[sizeof(lower) - 1] = '\0';
    for (char *p = lower; *p; p++) {
        *p = tolower(*p);
    }

    // Everyone: info
    if (strcmp(lower, "info") == 0) {
        cmdInfo(from);
        return;
    }

    // Unauthenticated users — only info allowed
    if (access == AccessLevel::NONE) {
        sendReply(from, "Not authorized. Send 'info' for details.");
        return;
    }

    // Allowed+ commands
    if (strcmp(lower, "help") == 0) {
        cmdHelp(from, access);
        return;
    }
    if (strncmp(lower, "help ", 5) == 0) {
        cmdHelpCommand(from, lower + 5, access);
        return;
    }
    if (strcmp(lower, "stats") == 0) {
        cmdStats(from, access);
        return;
    }
    if (strcmp(lower, "list") == 0) {
        cmdList(from, mp->from);
        return;
    }
    if (strncmp(lower, "list ", 5) == 0) {
        int num = atoi(lower + 5);
        if (num > 0) {
            cmdListDetail(from, num, access);
            return;
        }
    }
    if (strcmp(lower, "create") == 0) {
        cmdCreate(mp, access);
        return;
    }
    if (strncmp(lower, "edit ", 5) == 0) {
        int num = atoi(lower + 5);
        if (num > 0) {
            cmdEdit(mp, num, access);
            return;
        }
    }
    if (strncmp(lower, "delete ", 7) == 0) {
        int num = atoi(lower + 7);
        if (num > 0) {
            cmdDelete(mp, num, access);
            return;
        }
    }

    // Admin-only commands
    if (access == AccessLevel::ADMIN) {
        if (strcmp(lower, "all") == 0) {
            cmdAll(from);
            return;
        }
        if (strncmp(lower, "all ", 4) == 0) {
            const char *sub = lower + 4;
            while (*sub == ' ') {
                sub++;
            }
            if (strncmp(sub, "edit ", 5) == 0) {
                int num = atoi(sub + 5);
                if (num > 0) {
                    cmdAllEdit(mp, num);
                    return;
                }
            }
            if (strncmp(sub, "delete ", 7) == 0) {
                int num = atoi(sub + 7);
                if (num > 0) {
                    cmdAllDelete(mp, num);
                    return;
                }
            }
            // all <num> — detail view
            int num = atoi(sub);
            if (num > 0) {
                cmdAllDetail(from, num);
                return;
            }
        }
        if (strncmp(lower, "allow ", 6) == 0) {
            // Parse hex or decimal node number
            uint32_t target = strtoul(text + 6, nullptr, 0);
            if (target != 0) {
                cmdAllow(from, target);
                return;
            }
        }
        if (strncmp(lower, "deny ", 5) == 0) {
            uint32_t target = strtoul(text + 5, nullptr, 0);
            if (target != 0) {
                cmdDeny(from, target);
                return;
            }
        }
        if (strcmp(lower, "users") == 0) {
            cmdUsers(from);
            return;
        }
    }

    sendReply(from, "Unknown command. Send 'help' for available commands.");
}

// ========== Session Input Handler ==========

void AlertManager::handleSessionInput(const meshtastic_MeshPacket *mp, const char *text, UserSession *session)
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
    case SessionState::AWAIT_BODY: {
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
        session->state = SessionState::AWAIT_SEVERITY;
        promptForField(session, toNode);
        break;
    }
    case SessionState::AWAIT_SEVERITY: {
        if (!accepted) {
            int sev = atoi(input.c_str());
            if (sev < 0 || sev > 10) {
                sendReply(toNode, "Severity must be 0-10. Try again:");
                return;
            }
            session->severity = (uint8_t)sev;
        }
        session->state = SessionState::AWAIT_DATE_FROM;
        promptForField(session, toNode);
        break;
    }
    case SessionState::AWAIT_DATE_FROM: {
        if (!accepted) {
            strncpy(session->dateFrom, input.c_str(), sizeof(session->dateFrom) - 1);
            session->dateFrom[sizeof(session->dateFrom) - 1] = '\0';
        }
        session->state = SessionState::AWAIT_DATE_TO;
        promptForField(session, toNode);
        break;
    }
    case SessionState::AWAIT_DATE_TO: {
        if (!accepted) {
            strncpy(session->dateTo, input.c_str(), sizeof(session->dateTo) - 1);
            session->dateTo[sizeof(session->dateTo) - 1] = '\0';
        }
        session->state = SessionState::AWAIT_CHANNEL;
        promptForField(session, toNode);
        break;
    }
    case SessionState::AWAIT_CHANNEL: {
        if (!accepted) {
            // '-' resets to the default alert channel (empty string)
            if (input == "-") {
                session->channel[0] = '\0';
            } else {
                strncpy(session->channel, input.c_str(), sizeof(session->channel) - 1);
                session->channel[sizeof(session->channel) - 1] = '\0';
            }
        }
        session->state = SessionState::AWAIT_HOPS;
        promptForField(session, toNode);
        break;
    }
    case SessionState::AWAIT_HOPS: {
        if (!accepted) {
            int h = atoi(input.c_str());
            if (h < 0 || h > 7) {
                sendReply(toNode, "Hops must be 0-7. Try again:");
                return;
            }
            session->hops = (uint8_t)h;
        }
        session->state = SessionState::AWAIT_LOCATION;
        promptForField(session, toNode);
        break;
    }
    case SessionState::AWAIT_LOCATION: {
        if (!accepted) {
            strncpy(session->location, input.c_str(), sizeof(session->location) - 1);
            session->location[sizeof(session->location) - 1] = '\0';
        }
        session->state = SessionState::CONFIRM;
        promptForField(session, toNode);
        break;
    }
    case SessionState::CONFIRM: {
        if (accepted) {
            finalizeAlert(session, toNode);
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

// ========== State Machine Helpers ==========

void AlertManager::promptForField(UserSession *session, uint32_t toNode)
{
    char buf[MAX_REPLY_LEN + 1];

    switch (session->state) {
    case SessionState::AWAIT_BODY:
        if (session->isEdit && strlen(session->body) > 0) {
            // Always send the body as its own packet so long (near-240-byte) bodies
            // don't blow the PKI-encrypted DM on-air limit when combined with the
            // instructions line. The follow-up instructions go out as the usual
            // sendReply(buf) below.
            sendReply(toNode, session->body);
            snprintf(buf, sizeof(buf), "'.' to keep, '!' to abort, or send new body:");
        } else {
            snprintf(buf, sizeof(buf), "Enter alert body text ('!' to abort):");
        }
        break;
    case SessionState::AWAIT_SEVERITY: {
        // Build interval hints from the live firmware mapping
        char hints[96];
        int hintsOffset = 0;
        const uint8_t sampleSevs[] = {0, 1, 5, 8, 10};
        for (size_t i = 0; i < sizeof(sampleSevs) && hintsOffset < (int)sizeof(hints); i++) {
            unsigned long sec = alertsModule->getSendInterval(sampleSevs[i]);
            unsigned long h = sec / 3600;
            unsigned long m = (sec % 3600) / 60;
            char slot[16];
            if (h == 0) {
                snprintf(slot, sizeof(slot), "%u=%lum", sampleSevs[i], m);
            } else if (m == 0) {
                snprintf(slot, sizeof(slot), "%u=%luh", sampleSevs[i], h);
            } else {
                snprintf(slot, sizeof(slot), "%u=%luh%lum", sampleSevs[i], h, m);
            }
            hintsOffset += snprintf(hints + hintsOffset, sizeof(hints) - hintsOffset,
                                    "%s%s", i == 0 ? "" : " ", slot);
        }
        snprintf(buf, sizeof(buf),
                 "Severity 0-10 (0=critical, 10=minor)\nIntervals: %s\nCurrent: %d\n'.' to keep, '!' to abort:",
                 hints, session->severity);
        break;
    }
    case SessionState::AWAIT_DATE_FROM:
        if (strlen(session->dateFrom) > 0) {
            snprintf(buf, sizeof(buf), "Valid from (YYYY-MM-DD HH:MM:SS)\nCurrent: %s\n'.' to keep, '!' to abort:", session->dateFrom);
        } else {
            snprintf(buf, sizeof(buf), "Valid from (YYYY-MM-DD HH:MM:SS)\n'.' for now, '!' to abort:");
        }
        break;
    case SessionState::AWAIT_DATE_TO:
        if (strlen(session->dateTo) > 0) {
            snprintf(buf, sizeof(buf), "Valid to (YYYY-MM-DD HH:MM:SS)\nCurrent: %s\n'.' to keep, '!' to abort:", session->dateTo);
        } else {
            snprintf(buf, sizeof(buf), "Valid to (YYYY-MM-DD HH:MM:SS)\n'.' for +24h, '!' to abort:");
        }
        break;
    case SessionState::AWAIT_CHANNEL:
        if (strlen(session->channel) > 0) {
            const char *annotation = strcmp(session->channel, "*") == 0 ? " (PRIMARY)" : "";
            snprintf(buf, sizeof(buf),
                     "Channel name\nCurrent: %s%s\n'*'=primary, '-'=default alert ch, '.' keep, '!' abort:",
                     session->channel, annotation);
        } else {
            snprintf(buf, sizeof(buf),
                     "Channel name\n'*'=primary, '-' or '.' for default alert ch, '!' to abort:");
        }
        break;
    case SessionState::AWAIT_HOPS:
        if (session->hops != ALERT_HOP_LIMIT_DEFAULT) {
            snprintf(buf, sizeof(buf), "Hops (0-7)\nCurrent: %d\n'.' to keep, '!' to abort:", session->hops);
        } else {
            snprintf(buf, sizeof(buf), "Hops (0-7)\n'.' for default, '!' to abort:");
        }
        break;
    case SessionState::AWAIT_LOCATION:
        if (strlen(session->location) > 0) {
            snprintf(buf, sizeof(buf), "Location\nCurrent: %s\n'.' to keep, '!' to abort:", session->location);
        } else {
            snprintf(buf, sizeof(buf), "Location (optional)\n'.' to skip, '!' to abort:");
        }
        break;
    case SessionState::CONFIRM: {
        char hopsStr[8];
        if (session->hops == ALERT_HOP_LIMIT_DEFAULT) {
            strncpy(hopsStr, "default", sizeof(hopsStr));
        } else {
            snprintf(hopsStr, sizeof(hopsStr), "%d", session->hops);
        }
        // Body is confirmed at the AWAIT_BODY step, so omit it here — keeps the
        // packet under the PKI-encrypted DM size limit (~227 bytes on air).
        snprintf(buf, sizeof(buf), "Summary:\nSev: %d | Ch: %s | Hops: %s\nFrom: %s\nTo: %s\nLoc: %s\n'.' to confirm, '!' to abort",
                 session->severity,
                 strcmp(session->channel, "*") == 0 ? "(primary)" : (strlen(session->channel) > 0 ? session->channel : "(default)"),
                 hopsStr,
                 session->dateFrom, session->dateTo,
                 strlen(session->location) > 0 ? session->location : "(none)");
        break;
    }
    default:
        return;
    }

    sendReply(toNode, buf);
}

static void formatTimestamp(char *buf, size_t bufLen, time_t t)
{
    // Treat t as a UTC epoch and render it as local wall-clock time. We shift
    // by getTZOffset() and then gmtime the result so the fields reflect the
    // configured local timezone (config.device.tzdef).
    time_t localEpoch = t + getTZOffset();
    struct tm timeinfo;
    gmtime_r(&localEpoch, &timeinfo);
    snprintf(buf, bufLen, "%04d-%02d-%02d %02d:%02d:%02d",
             timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
             timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
}

void AlertManager::finalizeAlert(UserSession *session, uint32_t toNode)
{
    if (session->isSystemEdit) {
        // Editing a system alert (admin 'all edit')
        const auto &allAlerts = alertsModule->getAlerts();
        if (session->editIndex >= (uint16_t)allAlerts.size()) {
            sendReply(toNode, "Alert no longer exists.");
            clearSession(session);
            return;
        }

        Alert updated = allAlerts[session->editIndex];
        updated.message = session->body;
        updated.severity = session->severity;
        updated.valid_from = session->dateFrom;
        updated.valid_to = session->dateTo;
        updated.channel = session->channel;
        updated.hops = session->hops;
        updated.location = session->location;

        if (alertsModule->updateAlertById(session->editAlertId, updated)) {
            sendReply(toNode, "Alert updated.");
        } else {
            sendReply(toNode, "Failed to update alert.");
        }
        clearSession(session);
        return;
    }

    if (session->isEdit) {
        // Editing a user alert
        if (session->editIndex >= numUserAlerts) {
            sendReply(toNode, "Alert no longer exists.");
            clearSession(session);
            return;
        }

        UserAlertEntry &entry = userAlerts[session->editIndex];
        strncpy(entry.body, session->body, sizeof(entry.body) - 1);
        entry.body[sizeof(entry.body) - 1] = '\0';
        strncpy(entry.location, session->location, sizeof(entry.location) - 1);
        entry.location[sizeof(entry.location) - 1] = '\0';
        strncpy(entry.channel, session->channel, sizeof(entry.channel) - 1);
        entry.channel[sizeof(entry.channel) - 1] = '\0';
        strncpy(entry.dateFrom, session->dateFrom, sizeof(entry.dateFrom) - 1);
        entry.dateFrom[sizeof(entry.dateFrom) - 1] = '\0';
        strncpy(entry.dateTo, session->dateTo, sizeof(entry.dateTo) - 1);
        entry.dateTo[sizeof(entry.dateTo) - 1] = '\0';
        entry.severity = session->severity;
        entry.hops = session->hops;
        saveUserAlerts();

        // Update in main pipeline too
        Alert updated;
        updated.message = session->body;
        updated.location = session->location;
        updated.channel = session->channel;
        updated.hops = session->hops;
        updated.valid_from = session->dateFrom;
        updated.valid_to = session->dateTo;
        updated.severity = session->severity;
        alertsModule->updateAlertById(entry.id, updated);

        sendReply(toNode, "Alert updated.");
        clearSession(session);
        return;
    }

    // Creating a new alert
    if (numUserAlerts >= MAX_USER_ALERTS) {
        sendReply(toNode, "User alert limit reached. Delete some first.");
        clearSession(session);
        return;
    }

    // Fill defaults for empty date fields
    time_t now = getTime();
    if (strlen(session->dateFrom) == 0) {
        formatTimestamp(session->dateFrom, sizeof(session->dateFrom), now);
    }
    if (strlen(session->dateTo) == 0) {
        formatTimestamp(session->dateTo, sizeof(session->dateTo), now + 86400);
    }

    // Create user alert entry
    UserAlertEntry &entry = userAlerts[numUserAlerts];
    memset(&entry, 0, sizeof(UserAlertEntry));
    entry.ownerNodeNum = toNode;
    entry.createdAt = (uint32_t)now;
    entry.severity = session->severity;
    entry.hops = session->hops;
    strncpy(entry.body, session->body, sizeof(entry.body) - 1);
    strncpy(entry.location, session->location, sizeof(entry.location) - 1);
    strncpy(entry.channel, session->channel, sizeof(entry.channel) - 1);
    strncpy(entry.dateFrom, session->dateFrom, sizeof(entry.dateFrom) - 1);
    strncpy(entry.dateTo, session->dateTo, sizeof(entry.dateTo) - 1);
    entry.id = hashAlertId(entry.ownerNodeNum, entry.body, entry.createdAt);
    numUserAlerts++;
    saveUserAlerts();

    // Inject into AlertsModule pipeline
    Alert alert;
    alert.id = entry.id;
    alert.title = entry.body;
    alert.message = entry.body;
    alert.location = entry.location;
    alert.channel = entry.channel;
    alert.valid_from = entry.dateFrom;
    alert.valid_to = entry.dateTo;
    alert.source = "USR";
    alert.severity = entry.severity;
    alert.hops = entry.hops;
    alert.addedAt = entry.createdAt;

    if (alertsModule->addExternalAlert(alert)) {
        sendReply(toNode, "Alert created and broadcast.");
    } else {
        sendReply(toNode, "Alert saved but broadcast failed.");
    }
    clearSession(session);
}

// ========== Command Implementations ==========

void AlertManager::cmdInfo(uint32_t toNode)
{
    sendReply(toNode, "Alert Management System\nManage alerts via DM commands.\nSend 'help' for commands (requires access).\nContact an admin to request access.");
}

void AlertManager::cmdHelp(uint32_t toNode, AccessLevel access)
{
    if (access == AccessLevel::ADMIN) {
        sendReply(toNode, "Commands:\nstats, list, list <n>, create\nedit <n>, delete <n>\nall, all <n>, all edit <n>\nall delete <n>\nallow <node>, deny <node>\nusers\nhelp <cmd>");
    } else {
        sendReply(toNode, "Commands:\nstats, list, list <n>, create\nedit <n>, delete <n>\nhelp <cmd>");
    }
}

void AlertManager::cmdHelpCommand(uint32_t toNode, const char *cmd, AccessLevel access)
{
    if (strcmp(cmd, "create") == 0) {
        sendReply(toNode, "create - Start creating a new alert.\nYou'll be prompted for: body, severity (0-10), dates, channel, location.\n'.' accepts default, '!' aborts.\n'..' for literal '.', '!!' for literal '!'");
    } else if (strcmp(cmd, "edit") == 0) {
        sendReply(toNode, "edit <n> - Edit your alert #n from 'list'.\nEach field shown with current value.\n'.' keeps current, '!' aborts.");
    } else if (strcmp(cmd, "delete") == 0) {
        sendReply(toNode, "delete <n> - Delete your alert #n from 'list'.");
    } else if (strcmp(cmd, "list") == 0) {
        sendReply(toNode, "list - Show your user-created alerts.\nlist <n> - Show full details for alert #n.\nNumbers can be used with edit/delete.");
    } else if (strcmp(cmd, "stats") == 0) {
        sendReply(toNode, "stats - Show alert statistics:\nuser alerts, total system alerts, next broadcast.");
    } else if (strcmp(cmd, "all") == 0 && access == AccessLevel::ADMIN) {
        sendReply(toNode, "all - List ALL alerts from all sources.\nall <n> - Show details for alert #n.\nall edit <n> - Edit any alert.\nall delete <n> - Delete any alert.");
    } else if (strcmp(cmd, "allow") == 0 && access == AccessLevel::ADMIN) {
        sendReply(toNode, "allow <nodeNum> - Grant alert access.\nAccepts hex (0x...) or decimal.");
    } else if (strcmp(cmd, "deny") == 0 && access == AccessLevel::ADMIN) {
        sendReply(toNode, "deny <nodeNum> - Revoke alert access.");
    } else if (strcmp(cmd, "users") == 0 && access == AccessLevel::ADMIN) {
        sendReply(toNode, "users - List all allowed user nodes.");
    } else {
        sendReply(toNode, "Unknown command. Send 'help' for list.");
    }
}

void AlertManager::cmdStats(uint32_t toNode, AccessLevel access)
{
    const auto &allAlerts = alertsModule->getAlerts();
    int userCount = 0;
    for (int i = 0; i < numUserAlerts; i++) {
        userCount++;
    }
    sendReplyFmt(toNode, "Stats:\nUser alerts: %d\nTotal system: %d\nAllowed users: %d",
                 userCount, (int)allAlerts.size(), numAllowedUsers);
}

void AlertManager::cmdList(uint32_t toNode, uint32_t callerNode)
{
    char buf[MAX_REPLY_LEN + 1];
    int offset = 0;
    int count = 0;

    for (int i = 0; i < numUserAlerts && offset < MAX_REPLY_LEN - 40; i++) {
        if (userAlerts[i].ownerNodeNum == callerNode) {
            count++;
            int written = snprintf(buf + offset, sizeof(buf) - offset, "#%d [s%d] %.60s%s\n",
                                   count, userAlerts[i].severity,
                                   userAlerts[i].body,
                                   strlen(userAlerts[i].body) > 60 ? "..." : "");
            if (written > 0) {
                offset += written;
            }
        }
    }

    if (count == 0) {
        sendReply(toNode, "No alerts. Send 'create' to add one.");
    } else {
        buf[offset] = '\0';
        sendReply(toNode, buf);
    }
}

void AlertManager::cmdListDetail(uint32_t toNode, int num, AccessLevel access)
{
    int count = 0;
    int targetIdx = -1;
    for (int i = 0; i < numUserAlerts; i++) {
        if (userAlerts[i].ownerNodeNum == toNode || access == AccessLevel::ADMIN) {
            count++;
            if (count == num) {
                targetIdx = i;
                break;
            }
        }
    }
    if (targetIdx < 0) {
        sendReplyFmt(toNode, "Alert #%d not found.", num);
        return;
    }

    const UserAlertEntry &e = userAlerts[targetIdx];
    const char *chDisplay =
        strcmp(e.channel, "*") == 0 ? "(primary)" : (strlen(e.channel) > 0 ? e.channel : "(default)");
    const char *locDisplay = strlen(e.location) > 0 ? e.location : "(none)";
    char hopsStr[8];
    if (e.hops == ALERT_HOP_LIMIT_DEFAULT) {
        strncpy(hopsStr, "default", sizeof(hopsStr));
    } else {
        snprintf(hopsStr, sizeof(hopsStr), "%d", e.hops);
    }

    char header[MAX_REPLY_LEN + 1];
    snprintf(header, sizeof(header),
             "#%d sev:%d hops:%s\nFrom: %s To: %s\nCh: %s Loc: %s",
             num, e.severity, hopsStr, e.dateFrom, e.dateTo, chDisplay, locDisplay);

    // Prefer a single packet when the combined text fits; otherwise put the
    // body first so the user sees the full message before the metadata.
    char combined[MAX_REPLY_LEN + 1];
    int combinedLen = snprintf(combined, sizeof(combined), "%s\n%s", e.body, header);
    if (combinedLen > 0 && combinedLen < (int)sizeof(combined)) {
        sendReply(toNode, combined);
    } else {
        sendReply(toNode, e.body);
        sendReply(toNode, header);
    }
}

void AlertManager::cmdCreate(const meshtastic_MeshPacket *mp, AccessLevel access)
{
    UserSession *session = allocateSession(mp->from);
    if (!session) {
        sendReply(mp->from, "Busy. Try again later.");
        return;
    }

    session->isEdit = false;
    session->isSystemEdit = false;
    session->severity = 5;
    session->hops = ALERT_HOP_LIMIT_DEFAULT;

    // Set default channel to empty (will use alert channel)
    session->channel[0] = '\0';
    session->location[0] = '\0';
    session->dateFrom[0] = '\0';
    session->dateTo[0] = '\0';

    session->state = SessionState::AWAIT_BODY;
    promptForField(session, mp->from);
}

void AlertManager::cmdEdit(const meshtastic_MeshPacket *mp, int num, AccessLevel access)
{
    uint32_t from = mp->from;

    // Find the nth alert owned by this user
    int count = 0;
    int targetIdx = -1;
    for (int i = 0; i < numUserAlerts; i++) {
        if (userAlerts[i].ownerNodeNum == from || access == AccessLevel::ADMIN) {
            count++;
            if (count == num) {
                targetIdx = i;
                break;
            }
        }
    }

    if (targetIdx < 0) {
        sendReplyFmt(from, "Alert #%d not found.", num);
        return;
    }

    // Check ownership (non-admin can only edit own)
    if (access != AccessLevel::ADMIN && userAlerts[targetIdx].ownerNodeNum != from) {
        sendReply(from, "Not authorized to edit this alert.");
        return;
    }

    UserSession *session = allocateSession(from);
    if (!session) {
        sendReply(from, "Busy. Try again later.");
        return;
    }

    // Pre-fill session with current values
    UserAlertEntry &entry = userAlerts[targetIdx];
    session->isEdit = true;
    session->isSystemEdit = false;
    session->editIndex = targetIdx;
    session->editAlertId = entry.id;
    strncpy(session->body, entry.body, sizeof(session->body) - 1);
    session->severity = entry.severity;
    session->hops = entry.hops;
    strncpy(session->dateFrom, entry.dateFrom, sizeof(session->dateFrom) - 1);
    strncpy(session->dateTo, entry.dateTo, sizeof(session->dateTo) - 1);
    strncpy(session->channel, entry.channel, sizeof(session->channel) - 1);
    strncpy(session->location, entry.location, sizeof(session->location) - 1);

    session->state = SessionState::AWAIT_BODY;
    promptForField(session, from);
}

void AlertManager::cmdDelete(const meshtastic_MeshPacket *mp, int num, AccessLevel access)
{
    uint32_t from = mp->from;

    int count = 0;
    int targetIdx = -1;
    for (int i = 0; i < numUserAlerts; i++) {
        if (userAlerts[i].ownerNodeNum == from || access == AccessLevel::ADMIN) {
            count++;
            if (count == num) {
                targetIdx = i;
                break;
            }
        }
    }

    if (targetIdx < 0) {
        sendReplyFmt(from, "Alert #%d not found.", num);
        return;
    }

    if (access != AccessLevel::ADMIN && userAlerts[targetIdx].ownerNodeNum != from) {
        sendReply(from, "Not authorized to delete this alert.");
        return;
    }

    uint32_t alertId = userAlerts[targetIdx].id;
    char preview[61];
    strncpy(preview, userAlerts[targetIdx].body, 60);
    preview[60] = '\0';

    // Remove from user_alerts array
    for (int i = targetIdx; i < numUserAlerts - 1; i++) {
        userAlerts[i] = userAlerts[i + 1];
    }
    numUserAlerts--;
    memset(&userAlerts[numUserAlerts], 0, sizeof(UserAlertEntry));
    saveUserAlerts();

    // Remove from main pipeline
    alertsModule->removeAlertById(alertId);

    sendReplyFmt(from, "Deleted: %.60s", preview);
}

void AlertManager::cmdAll(uint32_t toNode)
{
    const auto &allAlerts = alertsModule->getAlerts();
    if (allAlerts.empty()) {
        sendReply(toNode, "No alerts in system.");
        return;
    }

    char buf[MAX_REPLY_LEN + 1];
    int offset = 0;
    int shown = 0;

    for (size_t i = 0; i < allAlerts.size() && offset < MAX_REPLY_LEN - 50; i++) {
        const Alert &a = allAlerts[i];
        int written = snprintf(buf + offset, sizeof(buf) - offset, "#%d [%s|s%d] %.50s%s\n",
                               (int)(i + 1), a.source.c_str(), a.severity,
                               a.message.c_str(),
                               a.message.length() > 50 ? "..." : "");
        if (written > 0) {
            offset += written;
            shown++;
        }
    }

    if ((int)allAlerts.size() > shown) {
        snprintf(buf + offset, sizeof(buf) - offset, "... +%d more", (int)allAlerts.size() - shown);
    }

    sendReply(toNode, buf);
}

void AlertManager::cmdAllDetail(uint32_t toNode, int num)
{
    const auto &allAlerts = alertsModule->getAlerts();
    int idx = num - 1;
    if (idx < 0 || idx >= (int)allAlerts.size()) {
        sendReplyFmt(toNode, "Alert #%d not found.", num);
        return;
    }

    const Alert &a = allAlerts[idx];

    // Check if it's a user alert and include owner info
    uint32_t ownerNode = 0;
    if (a.source == "USR") {
        for (int i = 0; i < numUserAlerts; i++) {
            if (userAlerts[i].id == a.id) {
                ownerNode = userAlerts[i].ownerNodeNum;
                break;
            }
        }
    }

    const char *chDisplay = a.channel == "*" ? "(primary)" : (a.channel.length() > 0 ? a.channel.c_str() : "(default)");
    const char *locDisplay = a.location.length() > 0 ? a.location.c_str() : "(none)";
    char hopsStr[8];
    if (a.hops == ALERT_HOP_LIMIT_DEFAULT) {
        strncpy(hopsStr, "default", sizeof(hopsStr));
    } else {
        snprintf(hopsStr, sizeof(hopsStr), "%d", a.hops);
    }

    char header[MAX_REPLY_LEN + 1];
    if (ownerNode != 0) {
        snprintf(header, sizeof(header), "#%d [%s] sev:%d hops:%s\nFrom: %s To: %s\nCh: %s Loc: %s\nOwner: 0x%x",
                 num, a.source.c_str(), a.severity, hopsStr,
                 a.valid_from.c_str(), a.valid_to.c_str(),
                 chDisplay, locDisplay, ownerNode);
    } else {
        snprintf(header, sizeof(header), "#%d [%s] sev:%d hops:%s\nFrom: %s To: %s\nCh: %s Loc: %s",
                 num, a.source.c_str(), a.severity, hopsStr,
                 a.valid_from.c_str(), a.valid_to.c_str(),
                 chDisplay, locDisplay);
    }

    // Send header, then message body across multiple packets if needed
    sendReply(toNode, header);

    const char *msg = a.message.c_str();
    size_t msgLen = a.message.length();
    size_t offset = 0;
    char chunk[MAX_REPLY_LEN + 1];
    while (offset < msgLen) {
        size_t remaining = msgLen - offset;
        size_t chunkLen = (remaining > MAX_REPLY_LEN) ? MAX_REPLY_LEN : remaining;
        memcpy(chunk, msg + offset, chunkLen);
        chunk[chunkLen] = '\0';
        sendReply(toNode, chunk);
        offset += chunkLen;
    }
}

void AlertManager::cmdAllEdit(const meshtastic_MeshPacket *mp, int num)
{
    uint32_t from = mp->from;
    const auto &allAlerts = alertsModule->getAlerts();
    int idx = num - 1;
    if (idx < 0 || idx >= (int)allAlerts.size()) {
        sendReplyFmt(from, "Alert #%d not found.", num);
        return;
    }

    UserSession *session = allocateSession(from);
    if (!session) {
        sendReply(from, "Busy. Try again later.");
        return;
    }

    const Alert &a = allAlerts[idx];
    session->isEdit = true;
    session->isSystemEdit = true;
    session->editIndex = idx;
    session->editAlertId = a.id;
    strncpy(session->body, a.message.c_str(), sizeof(session->body) - 1);
    session->body[sizeof(session->body) - 1] = '\0';
    session->severity = a.severity;
    strncpy(session->dateFrom, a.valid_from.c_str(), sizeof(session->dateFrom) - 1);
    session->dateFrom[sizeof(session->dateFrom) - 1] = '\0';
    strncpy(session->dateTo, a.valid_to.c_str(), sizeof(session->dateTo) - 1);
    session->dateTo[sizeof(session->dateTo) - 1] = '\0';
    strncpy(session->channel, a.channel.c_str(), sizeof(session->channel) - 1);
    session->channel[sizeof(session->channel) - 1] = '\0';
    session->hops = a.hops;
    strncpy(session->location, a.location.c_str(), sizeof(session->location) - 1);
    session->location[sizeof(session->location) - 1] = '\0';

    session->state = SessionState::AWAIT_BODY;
    promptForField(session, from);
}

void AlertManager::cmdAllDelete(const meshtastic_MeshPacket *mp, int num)
{
    uint32_t from = mp->from;
    const auto &allAlerts = alertsModule->getAlerts();
    int idx = num - 1;
    if (idx < 0 || idx >= (int)allAlerts.size()) {
        sendReplyFmt(from, "Alert #%d not found.", num);
        return;
    }

    uint32_t alertId = allAlerts[idx].id;
    String source = allAlerts[idx].source;
    String preview = allAlerts[idx].message.substring(0, 60);

    // Also remove from user_alerts if it's a USR alert
    if (source == "USR") {
        for (int i = 0; i < numUserAlerts; i++) {
            if (userAlerts[i].id == alertId) {
                for (int j = i; j < numUserAlerts - 1; j++) {
                    userAlerts[j] = userAlerts[j + 1];
                }
                numUserAlerts--;
                memset(&userAlerts[numUserAlerts], 0, sizeof(UserAlertEntry));
                saveUserAlerts();
                break;
            }
        }
    }

    alertsModule->removeAlertById(alertId);
    sendReplyFmt(from, "Deleted [%s]: %.50s", source.c_str(), preview.c_str());
}

void AlertManager::cmdAllow(uint32_t toNode, uint32_t targetNode)
{
    if (isAllowedUser(targetNode)) {
        sendReplyFmt(toNode, "Node 0x%x already allowed.", targetNode);
        return;
    }
    if (numAllowedUsers >= MAX_ALLOWED_USERS) {
        sendReply(toNode, "User limit reached.");
        return;
    }

    allowedUsers[numAllowedUsers].nodeNum = targetNode;
    allowedUsers[numAllowedUsers].addedAt = (uint32_t)getTime();
    numAllowedUsers++;
    savePermissions();
    sendReplyFmt(toNode, "Node 0x%x allowed.", targetNode);
}

void AlertManager::cmdDeny(uint32_t toNode, uint32_t targetNode)
{
    int found = -1;
    for (int i = 0; i < numAllowedUsers; i++) {
        if (allowedUsers[i].nodeNum == targetNode) {
            found = i;
            break;
        }
    }

    if (found < 0) {
        sendReplyFmt(toNode, "Node 0x%x not in allowed list.", targetNode);
        return;
    }

    for (int i = found; i < numAllowedUsers - 1; i++) {
        allowedUsers[i] = allowedUsers[i + 1];
    }
    numAllowedUsers--;
    memset(&allowedUsers[numAllowedUsers], 0, sizeof(PermissionEntry));
    savePermissions();
    sendReplyFmt(toNode, "Node 0x%x removed.", targetNode);
}

void AlertManager::cmdUsers(uint32_t toNode)
{
    if (numAllowedUsers == 0) {
        sendReply(toNode, "No allowed users.");
        return;
    }

    char buf[MAX_REPLY_LEN + 1];
    int offset = snprintf(buf, sizeof(buf), "Allowed users (%d):\n", numAllowedUsers);
    for (int i = 0; i < numAllowedUsers && offset < MAX_REPLY_LEN - 20; i++) {
        int written = snprintf(buf + offset, sizeof(buf) - offset, "0x%x\n", allowedUsers[i].nodeNum);
        if (written > 0) {
            offset += written;
        }
    }
    sendReply(toNode, buf);
}

// ========== Messaging ==========

void AlertManager::sendReply(uint32_t toNodeNum, const char *text)
{
    if (!text || !router || !service) {
        return;
    }

    // The radio rejects PKI-encrypted DMs whose encoded Data exceeds
    // MAX_LORA_PAYLOAD_LEN (255) − header (16) − PKC overhead (12) = 227 bytes.
    // Data encoding adds ~7 bytes (portnum + payload tag+length varint + bitfield),
    // leaving ~220 bytes for the text itself. Chunk to stay under the limit
    // regardless of what the caller passes in.
    size_t totalLen = strlen(text);
    size_t offset = 0;
    do {
        size_t remaining = totalLen - offset;
        size_t chunkLen = remaining > MAX_PACKET_TEXT_LEN ? MAX_PACKET_TEXT_LEN : remaining;

        meshtastic_MeshPacket *p = router->allocForSending();
        if (!p) {
            LOG_ERROR("[AlertManager] Failed to allocate packet");
            return;
        }

        p->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
        p->to = toNodeNum;
        p->from = nodeDB->getNodeNum();
        p->channel = 0; // Primary channel for DMs
        p->want_ack = false;
        p->decoded.want_response = false;

        p->decoded.payload.size = chunkLen;
        memcpy(p->decoded.payload.bytes, text + offset, chunkLen);

        service->sendToMesh(p);
        offset += chunkLen;
    } while (offset < totalLen);
}

void AlertManager::sendReplyFmt(uint32_t toNodeNum, const char *fmt, ...)
{
    char buf[MAX_REPLY_LEN + 1];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    sendReply(toNodeNum, buf);
}

// ========== Storage ==========

bool AlertManager::loadPermissions()
{
    concurrency::LockGuard g(spiLock);
    numAllowedUsers = 0;

    if (!FSCom.exists(PERMISSIONS_FILE)) {
        return true; // No file yet is fine
    }

    File f = FSCom.open(PERMISSIONS_FILE, FILE_O_READ);
    if (!f) {
        return false;
    }

    PermissionsHeader header;
    if (f.read((uint8_t *)&header, sizeof(header)) != sizeof(header) ||
        header.magic != PERMISSIONS_MAGIC || header.version != PERMISSIONS_VERSION) {
        f.close();
        FSCom.remove(PERMISSIONS_FILE);
        return false;
    }

    int count = (header.count < MAX_ALLOWED_USERS) ? header.count : MAX_ALLOWED_USERS;
    for (int i = 0; i < count; i++) {
        PermissionEntry entry;
        if (f.read((uint8_t *)&entry, sizeof(entry)) == sizeof(entry)) {
            allowedUsers[numAllowedUsers++] = entry;
        }
    }

    f.close();
    return true;
}

bool AlertManager::savePermissions()
{
    concurrency::LockGuard g(spiLock);

    File f = FSCom.open(PERMISSIONS_FILE_TMP, FILE_O_WRITE);
    if (!f) {
        return false;
    }

    PermissionsHeader header = {};
    header.magic = PERMISSIONS_MAGIC;
    header.version = PERMISSIONS_VERSION;
    header.count = numAllowedUsers;
    f.write((const uint8_t *)&header, sizeof(header));

    for (int i = 0; i < numAllowedUsers; i++) {
        f.write((const uint8_t *)&allowedUsers[i], sizeof(PermissionEntry));
    }

    f.flush();
    f.close();

    FSCom.remove(PERMISSIONS_FILE);
    // NOTE: call FSCom.rename directly — renameFile() re-acquires spiLock on ESP32.
    return FSCom.rename(PERMISSIONS_FILE_TMP, PERMISSIONS_FILE);
}

bool AlertManager::loadUserAlerts()
{
    concurrency::LockGuard g(spiLock);
    numUserAlerts = 0;

    if (!FSCom.exists(USER_ALERTS_FILE)) {
        return true;
    }

    File f = FSCom.open(USER_ALERTS_FILE, FILE_O_READ);
    if (!f) {
        return false;
    }

    UserAlertsHeader header;
    if (f.read((uint8_t *)&header, sizeof(header)) != sizeof(header) ||
        header.magic != USER_ALERTS_MAGIC || header.version != USER_ALERTS_VERSION) {
        f.close();
        FSCom.remove(USER_ALERTS_FILE);
        return false;
    }

    int count = (header.count < MAX_USER_ALERTS) ? header.count : MAX_USER_ALERTS;
    for (int i = 0; i < count; i++) {
        UserAlertEntry entry;
        if (f.read((uint8_t *)&entry, sizeof(entry)) == sizeof(entry)) {
            userAlerts[numUserAlerts++] = entry;
        }
    }

    f.close();
    return true;
}

bool AlertManager::saveUserAlerts()
{
    concurrency::LockGuard g(spiLock);

    File f = FSCom.open(USER_ALERTS_FILE_TMP, FILE_O_WRITE);
    if (!f) {
        return false;
    }

    UserAlertsHeader header = {};
    header.magic = USER_ALERTS_MAGIC;
    header.version = USER_ALERTS_VERSION;
    header.count = numUserAlerts;
    f.write((const uint8_t *)&header, sizeof(header));

    for (int i = 0; i < numUserAlerts; i++) {
        f.write((const uint8_t *)&userAlerts[i], sizeof(UserAlertEntry));
    }

    f.flush();
    f.close();

    FSCom.remove(USER_ALERTS_FILE);
    // NOTE: call FSCom.rename directly — renameFile() re-acquires spiLock on ESP32
    // and would deadlock against the LockGuard above.
    return FSCom.rename(USER_ALERTS_FILE_TMP, USER_ALERTS_FILE);
}

uint32_t AlertManager::hashAlertId(uint32_t ownerNode, const char *body, uint32_t createdAt) const
{
    uint32_t hash = 5381;
    // Mix in owner
    hash = ((hash << 5) + hash) + (ownerNode & 0xFF);
    hash = ((hash << 5) + hash) + ((ownerNode >> 8) & 0xFF);
    hash = ((hash << 5) + hash) + ((ownerNode >> 16) & 0xFF);
    hash = ((hash << 5) + hash) + ((ownerNode >> 24) & 0xFF);
    // Mix in body
    if (body) {
        for (const char *p = body; *p; p++) {
            hash = ((hash << 5) + hash) + (uint8_t)*p;
        }
    }
    // Mix in timestamp
    hash = ((hash << 5) + hash) + (createdAt & 0xFF);
    hash = ((hash << 5) + hash) + ((createdAt >> 8) & 0xFF);
    return hash;
}

// ========== Input Helpers ==========

String AlertManager::unescapeInput(const char *text)
{
    if (!text) {
        return "";
    }
    // ".." → "." (strip leading escape dot)
    if (text[0] == '.' && text[1] == '.') {
        return String(text + 1);
    }
    // "!!" → "!" (strip leading escape bang)
    if (text[0] == '!' && text[1] == '!') {
        return String(text + 1);
    }
    return String(text);
}

bool AlertManager::isAccept(const char *text)
{
    return text && text[0] == '.' && text[1] == '\0';
}

bool AlertManager::isAbort(const char *text)
{
    return text && text[0] == '!' && text[1] == '\0';
}

#endif // HAS_ALERTING && !MESHTASTIC_EXCLUDE_ALERT_INTERACTIVE
