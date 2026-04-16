# Alert Manager — Interactive Alert Management via Mesh DM

## Overview

The Alert Manager lets mesh users create and manage alerts by sending private messages (DMs) to the node. Admins can also manage alerts from any source (RCB, IMGW, POZ, etc.) through the same interface.

It consists of two components:
- **AlertManager** — command handler, permission system, state machine
- **UserAlertSource** — minimal source stub that registers `[USR]` as an alert source

---

# User-Facing Spec

## Getting Started

Send a private message (DM) to the alert node. All interaction happens through direct messages — no special app or channel required.

### Access Levels

| Level | Who | What you can do |
|-------|-----|-----------------|
| **Everyone** | Any node | Send `info` to learn about the system |
| **Allowed user** | Nodes granted access by an admin | Create, edit, delete your own alerts |
| **Admin** | Nodes whose PKI public key matches a device admin key | Everything: manage all alerts, manage users |

To request access, send `info` and ask an admin to run `allow <your_node_id>`.

## Commands

### For Everyone

| Command | Description |
|---------|-------------|
| `info` | What this system is and how to get access |

### For Allowed Users (and Admins)

| Command | Description |
|---------|-------------|
| `help` | List your available commands |
| `help <command>` | Detailed help for a specific command |
| `stats` | How many alerts exist (yours and system-wide) |
| `list` | Show your alerts, numbered `#1`, `#2`, ... |
| `create` | Start creating a new alert (guided step-by-step) |
| `edit <n>` | Edit your alert `#n` from `list` |
| `delete <n>` | Delete your alert `#n` |

### For Admins Only

| Command | Description |
|---------|-------------|
| `all` | List ALL alerts from every source |
| `all <n>` | Full details for alert `#n` (includes author for user alerts) |
| `all edit <n>` | Edit any alert `#n` — works on RCB, IMGW, USR, etc. |
| `all delete <n>` | Delete any alert `#n` |
| `allow <nodeId>` | Grant a node alert access (hex `0x1a2b3c4d` or decimal) |
| `deny <nodeId>` | Revoke a node's alert access |
| `users` | List all allowed nodes |

**Note:** `list`/`edit`/`delete` and `all`/`all edit`/`all delete` use independent numbering. Always `list` or `all` first to see current numbers.

## Creating an Alert

Send `create` to start. The system prompts for each field one at a time:

```
You:     create
Node:    Enter alert body text ('!' to abort):
You:     Fire reported near Swarzędz, avoid area
Node:    Severity 0-10 (0=critical, 10=minor)
         Current: 5
         '.' to keep:
You:     3
Node:    Valid from (YYYY-MM-DD HH:MM:SS)
         '.' for now:
You:     .
Node:    Valid to (YYYY-MM-DD HH:MM:SS)
         '.' for +24h:
You:     .
Node:    Channel name
         '.' for default alert channel:
You:     .
Node:    Location (optional)
         '.' to skip:
You:     Swarzędz
Node:    Summary:
         Body: Fire reported near Swarzędz, avoid area
         Sev: 3 | Ch: (default)
         From: 2026-04-16 10:30:00
         To: 2026-04-17 10:30:00
         Loc: Swarzędz
         '.' to confirm, '!' to abort
You:     .
Node:    Alert created and broadcast.
```

The alert is immediately broadcast to the mesh as `[USR] Fire reported near Swarzędz, avoid area [Swarzędz]` and will be periodically resent based on severity.

## Editing an Alert

Send `edit <n>` where `<n>` is the number from `list`. Each field shows its current value. Send `.` to keep it unchanged, or type a new value.

## Special Inputs During Create/Edit

| Input | Meaning |
|-------|---------|
| `.` | Accept default value / keep current value / confirm |
| `!` | Abort the entire create or edit flow |
| `..` | Literal `.` character (if your text starts with a period) |
| `!!` | Literal `!` character (if your text starts with an exclamation) |
| `?` | Re-send the current prompt (if previous message was lost) |

## Field Defaults

When you send `.` during creation, these defaults are used:

| Field | Default |
|-------|---------|
| Body | *Required* — cannot use default |
| Severity | 5 (medium) |
| Valid from | Current time |
| Valid to | Current time + 24 hours |
| Channel | Global alert channel |
| Location | Empty (no location suffix appended) |

## Session Behavior

- Sessions time out after **5 minutes** of inactivity
- Up to **3 users** can have active sessions simultaneously
- If all sessions are busy, you'll get "Busy. Try again later."
- Sending any message during a session resets the timeout

## Author Privacy

Your node ID is **not** included in broadcast alert messages. Only admins can see who created an alert, via `all <n>`.

---

# Technical Spec

## Architecture

```
                                ┌──────────────────────┐
  Incoming DM                   │   TextMessageModule   │
  (meshtastic_MeshPacket)       │   (Observable)        │
                                └──────────┬───────────┘
                                           │ onNotify()
                                           ▼
                                ┌──────────────────────┐
                                │    AlertManager       │
                                │    (Observer)         │
                                │                      │
                                │  - Permission check   │
                                │  - Command dispatch   │
                                │  - Session state      │
                                │  - DM replies         │
                                └──────────┬───────────┘
                                           │ addExternalAlert()
                                           │ updateAlertById()
                                           │ removeAlertById()
                                           ▼
                                ┌──────────────────────┐
                                │    AlertsModule       │
                                │                      │
                                │  alerts.bin storage   │
                                │  broadcast scheduling │
                                │  severity-based resend│
                                └──────────────────────┘
```

AlertManager does **not** inherit from MeshModule. It subscribes to `TextMessageModule` via the `Observer<const meshtastic_MeshPacket *>` pattern. Alerts are injected directly into AlertsModule via public API methods, bypassing the HTTP-based fetch cycle (which is WiFi-gated).

## Files

| File | Role |
|------|------|
| `AlertManager.h` | Class declaration, structs, constants |
| `AlertManager.cpp` | Full implementation (~800 lines) |
| `sources/UserAlertSource.h` | Header-only AlertSource stub for "USR" source |

## Compile-Time Guard

```
MESHTASTIC_EXCLUDE_ALERT_INTERACTIVE
```

Set `EXCLUDE_ALERT_INTERACTIVE=1` in `.env` and build with `bin/build-with-env.sh`. Guards:
- `UserAlertSource.h` — entire file
- `AlertManager.h` / `.cpp` — entire files
- `AlertsModule.cpp` — include, registration, and `alertManager` member

## Initialization

AlertManager is created during `AlertsModule::runOnce()` in the `INIT` state (not in the constructor), because `textMessageModule` must be initialized first:

```cpp
// In AlertsModule::runOnce(), ModuleState::INIT:
alertManager = new AlertManager(this);
alertManager->loadFromDisk();
alertManager->observe(textMessageModule);
```

## Permission Model

### Admin Detection

Checks `meshtastic_MeshPacket.pki_encrypted` and compares `mp->public_key.bytes` against `config.security.admin_key[0..2]` (32-byte PKI keys stored in device config). Same mechanism as `AdminModule`.

### Allowed User Storage

Node IDs stored in `/alerts/user_permissions.bin`. Loaded into a fixed-size array at startup.

### Access Resolution

```
getAccessLevel(mp):
    if pki_encrypted && public_key matches admin_key[0..2] → ADMIN
    if mp->from in allowedUsers[]                          → ALLOWED
    else                                                   → NONE
```

## Message Handling Flow

```
onNotify(mp):
    1. Filter: only DMs to this node, TEXT_MESSAGE_APP portnum
    2. Extract and trim text payload
    3. Expire stale sessions
    4. If text is `?` and session exists → re-send prompt, return 1
    5. If text is `?` and no session → send info message, return 1
    6. If active session for mp->from → handleSessionInput(), return 1
    7. Else → handleCommand(), return 1
```

Return value: `1` = handled (suppresses normal text display), `0` = pass through.

## State Machine

### States

```
NONE → AWAIT_BODY → AWAIT_SEVERITY → AWAIT_DATE_FROM →
       AWAIT_DATE_TO → AWAIT_CHANNEL → AWAIT_LOCATION → CONFIRM → NONE
```

### Input Processing Per State

Each state accepts:
- `.` (single dot) — accept current/default value, advance to next state
- `!` (single bang) — abort, clear session
- `..` — unescape to `.`, treat as user input
- `!!` — unescape to `!`, treat as user input
- Any other text — validate and store, advance to next state

Severity validation: must be 0-10, otherwise re-prompts.
Body validation: required in create mode (`.` rejected), allowed in edit mode (keeps existing).

### Session Storage

```cpp
struct UserSession {
    uint32_t nodeNum;           // 0 = slot unused
    unsigned long lastActivityMs;
    SessionState state;
    bool isEdit;
    bool isSystemEdit;          // true = admin editing system alert
    uint16_t editIndex;         // index in userAlerts[] or alerts vector
    uint32_t editAlertId;       // alert hash ID
    char body[237];
    uint8_t severity;
    char dateFrom[20];          // "YYYY-MM-DD HH:MM:SS"
    char dateTo[20];
    char channel[32];
    char location[64];
};
```

Fixed array of `MAX_SESSIONS = 3`. Linear scan by nodeNum.

## Alert Lifecycle

### Create

1. User sends `create`
2. Session allocated, state → `AWAIT_BODY`
3. Step through fields with prompts and defaults
4. On confirm:
   - `UserAlertEntry` written to `userAlerts[]` array + saved to `user_alerts.bin`
   - `Alert` struct built and passed to `alertsModule->addExternalAlert()`
   - AlertsModule stores in `alerts.bin`, broadcasts to mesh, schedules resends

### Edit (User Alert)

1. User sends `edit <n>`, looked up in `userAlerts[]` filtered by owner
2. Session pre-filled with current values, `isEdit = true`
3. On confirm:
   - `userAlerts[editIndex]` updated + saved to `user_alerts.bin`
   - `alertsModule->updateAlertById()` updates `alerts.bin` and resets send timing

### Edit (System Alert — Admin)

1. Admin sends `all edit <n>`, looked up in `alertsModule->getAlerts()[n-1]`
2. Session pre-filled, `isSystemEdit = true`
3. On confirm:
   - `alertsModule->updateAlertById()` only (no `user_alerts.bin` entry)

### Delete

- User `delete <n>`: removes from `userAlerts[]` + calls `alertsModule->removeAlertById()`
- Admin `all delete <n>`: calls `alertsModule->removeAlertById()`, and if source is "USR", also removes from `userAlerts[]`

## AlertsModule Public API

Added to support AlertManager operations:

```cpp
const std::vector<Alert> &getAlerts() const;
bool addExternalAlert(const Alert &alert);
bool removeAlertById(uint32_t id);
bool updateAlertById(uint32_t id, const Alert &updatedAlert);
```

- `addExternalAlert` — adds to `alerts` vector, caches ID, saves to disk, broadcasts immediately
- `removeAlertById` — removes from vector + processed ID cache, saves to disk
- `updateAlertById` — updates in-place, resets send timing for immediate re-broadcast, saves to disk

## Disk Storage

### `/alerts/user_permissions.bin`

| Field | Type | Size |
|-------|------|------|
| **Header** | | **8 bytes** |
| magic | uint32_t | 4 (0x55505253 "UPRS") |
| version | uint16_t | 2 |
| count | uint16_t | 2 |
| **Entry** (x count) | | **8 bytes each** |
| nodeNum | uint32_t | 4 |
| addedAt | uint32_t | 4 |

Max 32 entries. Max file size: 264 bytes.

### `/alerts/user_alerts.bin`

| Field | Type | Size |
|-------|------|------|
| **Header** | | **8 bytes** |
| magic | uint32_t | 4 (0x55414C54 "UALT") |
| version | uint16_t | 2 |
| count | uint16_t | 2 |
| **Entry** (x count) | | **388 bytes each** |
| id | uint32_t | 4 |
| ownerNodeNum | uint32_t | 4 |
| body | char[237] | 237 |
| location | char[64] | 64 |
| channel | char[32] | 32 |
| dateFrom | char[20] | 20 |
| dateTo | char[20] | 20 |
| severity | uint8_t | 1 |
| createdAt | uint32_t | 4 |
| padding | uint8_t[2] | 2 |

Max 50 entries. Max file size: ~19 KB.

Both files use atomic writes (write to `.tmp`, rename) with SPI lock for thread safety. On version mismatch or corruption, the file is deleted and starts fresh.

## Alert ID Generation

User alert IDs are computed as a djb2 hash of `ownerNodeNum + body + createdAt`:

```cpp
uint32_t hash = 5381;
// Mix in ownerNodeNum bytes
// Mix in body characters
// Mix in createdAt bytes
```

This provides deterministic, collision-resistant 32-bit IDs that link `user_alerts.bin` entries to their counterparts in the main `alerts.bin`.

## Reply Mechanism

Uses `router->allocForSending()` (not `allocDataPacket()` which is MeshModule-only):

```cpp
p->decoded.portnum = TEXT_MESSAGE_APP
p->to = targetNodeNum       // DM to sender
p->channel = 0              // Primary channel
p->want_ack = false
```

Replies are capped at 230 bytes (safe mesh text payload). Long list output is truncated with `... +N more`.

## Constants

| Constant | Value | Purpose |
|----------|-------|---------|
| `MAX_SESSIONS` | 3 | Concurrent create/edit sessions |
| `SESSION_TIMEOUT_MS` | 300,000 (5 min) | Inactivity timeout |
| `MAX_ALLOWED_USERS` | 32 | Permission entries |
| `MAX_USER_ALERTS` | 50 | User-created alert entries |
| `MAX_REPLY_LEN` | 230 | Safe mesh text payload |
| `PERMISSIONS_MAGIC` | 0x55505253 | "UPRS" file header |
| `USER_ALERTS_MAGIC` | 0x55414C54 | "UALT" file header |
| `STORAGE_VERSION` | 1 | Binary format version |

## RAM Usage

- 3 sessions x ~394 bytes = ~1.2 KB
- 32 permission entries x 8 bytes = 256 bytes
- 50 user alert entries x 388 bytes = ~19 KB
- **Total: ~20.5 KB fixed allocation**

All arrays are statically sized (no heap allocation beyond the initial `new AlertManager`).
