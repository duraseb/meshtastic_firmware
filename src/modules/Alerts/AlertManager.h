#pragma once

#if defined(HAS_ALERTING) && HAS_ALERTING && !MESHTASTIC_EXCLUDE_ALERT_INTERACTIVE

#include "AlertsModule.h"
#include "Observer.h"
#include "mesh/generated/meshtastic/mesh.pb.h"
#include <Arduino.h>

/**
 * AlertManager — handles interactive alert management via private mesh messages.
 *
 * Observes TextMessageModule for incoming DMs and provides:
 * - User alert creation/editing/deletion with a step-by-step state machine
 * - Admin commands for managing ALL alerts (any source) and user permissions
 * - Permission tiers: admin (PKI) > allowed user (stored) > everyone (info only)
 *
 * Storage files (separate from main alerts.bin):
 * - /alerts/user_permissions.bin — allowed user node IDs
 * - /alerts/user_alerts.bin     — user-created alert metadata (ownership tracking)
 */
class AlertManager : public Observer<const meshtastic_MeshPacket *> {
public:
    explicit AlertManager(AlertsModule *module);

    /// Load permission and user alert data from disk
    void loadFromDisk();

protected:
    int onNotify(const meshtastic_MeshPacket *mp) override;

private:
    // ===== Constants =====
    static constexpr int MAX_SESSIONS = 3;
    static constexpr unsigned long SESSION_TIMEOUT_MS = 5UL * 60 * 1000;
    static constexpr int MAX_ALLOWED_USERS = 32;
    static constexpr int MAX_USER_ALERTS = 50;
    static constexpr int MAX_REPLY_LEN = 230; // Safe mesh text payload

    // Storage paths
    static constexpr const char *PERMISSIONS_FILE = "/alerts/user_permissions.bin";
    static constexpr const char *PERMISSIONS_FILE_TMP = "/alerts/user_permissions.bin.tmp";
    static constexpr const char *USER_ALERTS_FILE = "/alerts/user_alerts.bin";
    static constexpr const char *USER_ALERTS_FILE_TMP = "/alerts/user_alerts.bin.tmp";

    // Storage magic numbers
    static constexpr uint32_t PERMISSIONS_MAGIC = 0x55505253; // "UPRS"
    static constexpr uint32_t USER_ALERTS_MAGIC = 0x55414C54; // "UALT"
    static constexpr uint16_t PERMISSIONS_VERSION = 1;
    static constexpr uint16_t USER_ALERTS_VERSION = 2; // v2: added hops field to UserAlertEntry

    // ===== Session State Machine =====
    enum class SessionState {
        NONE,
        AWAIT_BODY,
        AWAIT_SEVERITY,
        AWAIT_DATE_FROM,
        AWAIT_DATE_TO,
        AWAIT_CHANNEL,
        AWAIT_HOPS,
        AWAIT_LOCATION,
        CONFIRM
    };

    struct UserSession {
        uint32_t nodeNum;           // 0 = slot unused
        unsigned long lastActivityMs;
        SessionState state;
        bool isEdit;
        bool isSystemEdit;          // true if editing a system alert (admin 'all edit')
        uint16_t editIndex;         // index in user_alerts or alerts vector
        uint32_t editAlertId;       // alert ID being edited
        char body[237];
        uint8_t severity;
        char dateFrom[20];
        char dateTo[20];
        char channel[32];
        uint8_t hops;
        char location[64];
    };

    // ===== Storage Structures =====
    struct PermissionsHeader {
        uint32_t magic;
        uint16_t version;
        uint16_t count;
    };

    struct PermissionEntry {
        uint32_t nodeNum;
        uint32_t addedAt;
    };

    struct UserAlertsHeader {
        uint32_t magic;
        uint16_t version;
        uint16_t count;
    };

    struct UserAlertEntry {
        uint32_t id;
        uint32_t ownerNodeNum;
        char body[237];
        char location[64];
        char channel[32];
        char dateFrom[20];
        char dateTo[20];
        uint8_t severity;
        uint8_t hops;         // 0-7 = explicit, ALERT_HOP_LIMIT_DEFAULT = device default
        uint32_t createdAt;
        uint8_t padding[1];
    };

    // ===== Permission Levels =====
    enum class AccessLevel {
        NONE,       // Unauthenticated — info only
        ALLOWED,    // Allowed user — manage own alerts
        ADMIN       // Admin — manage everything
    };

    // ===== Members =====
    AlertsModule *alertsModule;
    UserSession sessions[MAX_SESSIONS];

    PermissionEntry allowedUsers[MAX_ALLOWED_USERS];
    int numAllowedUsers;

    UserAlertEntry userAlerts[MAX_USER_ALERTS];
    int numUserAlerts;

    // ===== Permission Methods =====
    AccessLevel getAccessLevel(const meshtastic_MeshPacket *mp) const;
    bool isAdmin(const meshtastic_MeshPacket *mp) const;
    bool isAllowedUser(uint32_t nodeNum) const;

    // ===== Session Methods =====
    UserSession *findSession(uint32_t nodeNum);
    UserSession *allocateSession(uint32_t nodeNum);
    void clearSession(UserSession *session);
    void expireStaleSessions();

    // ===== Command Handlers =====
    void handleCommand(const meshtastic_MeshPacket *mp, const char *text, AccessLevel access);
    void handleSessionInput(const meshtastic_MeshPacket *mp, const char *text, UserSession *session);

    void cmdInfo(uint32_t toNode);
    void cmdHelp(uint32_t toNode, AccessLevel access);
    void cmdHelpCommand(uint32_t toNode, const char *cmd, AccessLevel access);
    void cmdStats(uint32_t toNode, AccessLevel access);
    void cmdList(uint32_t toNode, uint32_t callerNode);
    void cmdCreate(const meshtastic_MeshPacket *mp, AccessLevel access);
    void cmdEdit(const meshtastic_MeshPacket *mp, int num, AccessLevel access);
    void cmdDelete(const meshtastic_MeshPacket *mp, int num, AccessLevel access);
    void cmdAll(uint32_t toNode);
    void cmdAllDetail(uint32_t toNode, int num);
    void cmdAllEdit(const meshtastic_MeshPacket *mp, int num);
    void cmdAllDelete(const meshtastic_MeshPacket *mp, int num);
    void cmdAllow(uint32_t toNode, uint32_t targetNode);
    void cmdDeny(uint32_t toNode, uint32_t targetNode);
    void cmdUsers(uint32_t toNode);

    // ===== State Machine =====
    void advanceSession(UserSession *session, uint32_t toNode);
    void promptForField(UserSession *session, uint32_t toNode);
    void finalizeAlert(UserSession *session, uint32_t toNode);

    // ===== Messaging =====
    void sendReply(uint32_t toNodeNum, const char *text);
    void sendReplyFmt(uint32_t toNodeNum, const char *fmt, ...);

    // ===== Storage =====
    bool loadPermissions();
    bool savePermissions();
    bool loadUserAlerts();
    bool saveUserAlerts();
    int findUserAlertByOwnerIndex(uint32_t ownerNode, int userIndex) const;
    uint32_t hashAlertId(uint32_t ownerNode, const char *body, uint32_t createdAt) const;

    // ===== Input Helpers =====
    static String unescapeInput(const char *text);
    static bool isAccept(const char *text);
    static bool isAbort(const char *text);
};

#endif // HAS_ALERTING && !MESHTASTIC_EXCLUDE_ALERT_INTERACTIVE
