#include "NodeRateLimiter.h"

#if !MESHTASTIC_EXCLUDE_SIGNALROUTING

#include "MeshTypes.h"
#include "NodeDB.h"
#include "Router.h"
#include "SignalRoutingModule.h"
#include "airtime.h"
#include "configuration.h"
#if !defined(UNIT_TEST)
#include "Channels.h"
#endif
#include "mesh/graph/NeighborGraph.h"
#if HAS_TRAFFIC_MANAGEMENT
#include "modules/TrafficManagementModule.h"
#endif
#include <Arduino.h>
#include <algorithm>
#include <cstring>

static void getNodeDisplayName(NodeNum nodeId, char *buf, size_t bufSize)
{
#ifdef DEBUG_MUTE
    (void)nodeId;
    if (bufSize > 0) {
        buf[0] = '\0';
    }
#else
    if (!nodeDB) {
        snprintf(buf, bufSize, "(%08x)", nodeId);
        return;
    }
    const meshtastic_NodeInfoLite *node = nodeDB->getMeshNode(nodeId);
    if (node && node->has_user && node->user.long_name[0]) {
        snprintf(buf, bufSize, "%s (%s, %08x)", node->user.long_name, node->user.short_name, nodeId);
    } else {
        snprintf(buf, bufSize, "Unknown (%08x)", nodeId);
    }
#endif
}

NodeRateLimiter *nodeRateLimiter = nullptr;

#if defined(UNIT_TEST)
NodeNum (*NodeRateLimiter::testResolveRelayHook)(uint8_t, int16_t, float) = nullptr;
bool (*NodeRateLimiter::testNodeInGraphHook)(NodeNum) = nullptr;
uint8_t (*NodeRateLimiter::testGraphHopsHook)(NodeNum) = nullptr;
float NodeRateLimiter::testChutilOverride = -1.0f;
int8_t NodeRateLimiter::testOnDefaultChannelOverride = -1;
int64_t NodeRateLimiter::testNowOverride = -1;

bool NodeRateLimiter::debugTracksOriginator(NodeNum nodeId) const
{
    for (uint8_t i = 0; i < originatorCount; i++) {
        if (originators[i].nodeId == nodeId) {
            return true;
        }
    }
    return false;
}

bool NodeRateLimiter::debugOriginatorLimited(NodeNum nodeId) const
{
    return isLimitedOriginator(nodeId);
}

bool NodeRateLimiter::debugTracksRelay(NodeNum nodeId) const
{
    for (uint8_t i = 0; i < relayCount; i++) {
        if (relays[i].nodeId == nodeId) {
            return true;
        }
    }
    return false;
}

bool NodeRateLimiter::debugRelayLimited(NodeNum nodeId) const
{
    const RelayEntry *e = nullptr;
    for (uint8_t i = 0; i < relayCount; i++) {
        if (relays[i].nodeId == nodeId) {
            e = &relays[i];
            break;
        }
    }
    return e && e->relay.limited;
}

bool NodeRateLimiter::debugUnresolvedRelayLimited() const
{
    return unresolvedRelay.limited;
}

void NodeRateLimiter::debugRelayBudgets(uint32_t airMs, uint32_t &tripMs, uint32_t &clearMs) const
{
    relayBudgets(airMs, tripMs, clearMs);
}

bool NodeRateLimiter::debugTracksYoung(NodeNum nodeId) const
{
    return findYoung(nodeId) >= 0;
}

bool NodeRateLimiter::debugIsYoung(NodeNum nodeId) const
{
    return isYoung(nodeId, nowMs());
}

void NodeRateLimiter::debugSetBootMs(uint32_t bootMs_)
{
    bootMs = bootMs_;
    bootKnown = true;
}

void NodeRateLimiter::debugSeedYoung(NodeNum nodeId, uint32_t firstSeenMs)
{
    noteOriginator(nodeId, firstSeenMs);
}

bool NodeRateLimiter::debugTakeAnnounce(NodeNum *ids, uint8_t &count)
{
    return takeYoungAnnounce(ids, count);
}

bool NodeRateLimiter::debugTracksDest(NodeNum nodeId) const
{
    return findDest(nodeId) != nullptr;
}

bool NodeRateLimiter::debugDestLimited(NodeNum nodeId) const
{
    return isLimitedDest(nodeId);
}

uint32_t NodeRateLimiter::debugDestCharge(NodeNum nodeId) const
{
    const DestEntry *e = findDest(nodeId);
    return e ? e->dest.count : 0;
}

uint8_t NodeRateLimiter::debugDestSenders(NodeNum nodeId) const
{
    const DestEntry *e = findDest(nodeId);
    return e ? e->senderCount : 0;
}
#endif

NodeRateLimiter::NodeRateLimiter() : originatorCount(0), relayCount(0), destCount(0)
{
    if (moduleConfig.has_node_rate_limiter) {
        const auto &cfg = moduleConfig.node_rate_limiter;
        cfgEnabled = cfg.enabled;
        if (cfg.window_secs != 0) {
            cfgWindowMs = cfg.window_secs * 1000u;
        }
        if (cfg.text_threshold != 0) {
            cfgTextTrip = (uint8_t)std::min<uint32_t>(cfg.text_threshold, 255);
        }
        if (cfg.routing_threshold != 0) {
            cfgRoutingTrip = (uint8_t)std::min<uint32_t>(cfg.routing_threshold, 255);
        }
        if (cfg.other_threshold != 0) {
            cfgOtherTrip = (uint8_t)std::min<uint32_t>(cfg.other_threshold, 255);
        }
    }

    LOG_INFO("[RateLimit] Initialized: enabled=%d originators=%u relays=%u+1 window=%us "
             "thresholds=text:%u routing:%u other:%u unknown:%u relay~%u/%u",
             cfgEnabled, MAX_ORIGINATOR_ENTRIES, MAX_RELAY_ENTRIES, cfgWindowMs / 1000u, cfgTextTrip, cfgRoutingTrip,
             cfgOtherTrip, cfgUnknownTrip, RELAY_TRIP_PACKETS, RELAY_CLEAR_PACKETS);
}

NodeRateLimiter::Bucket NodeRateLimiter::classifyBucket(meshtastic_PortNum portnum)
{
    switch (portnum) {
    case meshtastic_PortNum_TEXT_MESSAGE_APP:
    case meshtastic_PortNum_TEXT_MESSAGE_COMPRESSED_APP:
        return Bucket::TEXT;
    case meshtastic_PortNum_ROUTING_APP:
    case meshtastic_PortNum_SIGNAL_ROUTING_APP:
    case meshtastic_PortNum_TRACEROUTE_APP:
        return Bucket::ROUTING;
    default:
        return Bucket::OTHER;
    }
}

bool NodeRateLimiter::isRebroadcastCandidate(const meshtastic_MeshPacket *p)
{
    if (!p || isToUs(p) || isFromUs(p)) {
        return false;
    }
    if (config.device.role == meshtastic_Config_DeviceConfig_Role_CLIENT_MUTE) {
        return false;
    }
    if (config.device.rebroadcast_mode == meshtastic_Config_DeviceConfig_RebroadcastMode_NONE) {
        return false;
    }
    if (p->hop_limit > 0) {
        return true;
    }
    // Match NextHopRouter::perhapsRebroadcast: traffic-management exhaust still relays once.
#if HAS_TRAFFIC_MANAGEMENT
    if (trafficManagementModule && trafficManagementModule->shouldExhaustHops(*p)) {
        return true;
    }
#endif
    return false;
}

uint32_t NodeRateLimiter::packetAirtimeMs(const meshtastic_MeshPacket *p)
{
    if (router && router->getRadioInterface()) {
        uint32_t ms = router->getRadioInterface()->getPacketTime(p, true);
        if (ms > 0) {
            return ms;
        }
    }
    return RELAY_REF_AIRTIME_MS;
}

void NodeRateLimiter::relayBudgets(uint32_t airMs, uint32_t &tripMs, uint32_t &clearMs) const
{
    if (airMs == 0) {
        airMs = RELAY_REF_AIRTIME_MS;
    }
    // B_preset: ~60 packet-equivalents at this frame's airtime (modem/preset sensitive).
    uint32_t baseTrip = RELAY_TRIP_PACKETS * airMs;

    float scale = 1.0f;
    float chutil = -1.0f;
#if defined(UNIT_TEST)
    if (testChutilOverride >= 0.0f) {
        chutil = testChutilOverride;
    } else
#endif
        if (airTime) {
        chutil = airTime->channelUtilizationPercent();
    }
    if (chutil > 25.0f) {
        float t = (chutil - 25.0f) / 25.0f;
        if (t > 1.0f) {
            t = 1.0f;
        }
        scale = 1.0f - 0.5f * t;
    }

    tripMs = (uint32_t)((float)baseTrip * scale);
    if (tripMs < RELAY_TRIP_FLOOR_MS) {
        tripMs = RELAY_TRIP_FLOOR_MS;
    }
    if (tripMs > RELAY_TRIP_CEIL_MS) {
        tripMs = RELAY_TRIP_CEIL_MS;
    }
    // Preserve ~15/60 clear ratio
    clearMs = (tripMs * RELAY_CLEAR_PACKETS) / RELAY_TRIP_PACKETS;
    if (clearMs < RELAY_CLEAR_FLOOR_MS) {
        clearMs = RELAY_CLEAR_FLOOR_MS;
    }
    if (clearMs > RELAY_CLEAR_CEIL_MS) {
        clearMs = RELAY_CLEAR_CEIL_MS;
    }
}

bool NodeRateLimiter::nodeInGraph(NodeNum nodeId)
{
#if defined(UNIT_TEST)
    if (testNodeInGraphHook) {
        return testNodeInGraphHook(nodeId);
    }
#endif
    if (!nodeId || !signalRoutingModule) {
        return false;
    }
    return signalRoutingModule->rateLimitNodeInGraph(nodeId);
}

uint8_t NodeRateLimiter::graphHopsAway(NodeNum nodeId)
{
#if defined(UNIT_TEST)
    if (testGraphHopsHook) {
        return testGraphHopsHook(nodeId);
    }
#endif
    if (!nodeId || !signalRoutingModule) {
        return 0;
    }
    return signalRoutingModule->rateLimitGraphHops(nodeId);
}

bool NodeRateLimiter::checkAndUpdateBucket(BucketState &b, uint32_t trip, uint32_t clear, uint32_t charge, uint32_t nowMs,
                                           const char *label, NodeNum nodeId)
{
    char nodeName[48];
    getNodeDisplayName(nodeId, nodeName, sizeof(nodeName));

    // clear==0 (originator default): sticky quiet window — any activity while limited
    // resets the clock; lift only after a full quiet window (legacy behaviour).
    // clear>0 (RELAY): fixed window boundary hysteresis — do not reset on activity;
    // at roll, lift if count < clear.
    if (b.limited && clear == 0) {
        if (nowMs - b.windowStart >= cfgWindowMs) {
            LOG_INFO("[RateLimit] %s %s unlimited after quiet window", nodeName, label);
            b.limited = false;
            b.count = 0;
            b.windowStart = nowMs;
        } else {
            b.windowStart = nowMs;
            LOG_INFO("[RateLimit] %s %s still limited, window reset", nodeName, label);
            return true;
        }
    }

    if (nowMs - b.windowStart >= cfgWindowMs) {
        if (b.limited && clear > 0) {
            if (b.count < clear) {
                LOG_INFO("[RateLimit] %s %s unlimited after window (count=%u clear=%u)", nodeName, label, b.count, clear);
                b.limited = false;
            } else {
                LOG_INFO("[RateLimit] %s %s still limited at window roll (count=%u clear=%u)", nodeName, label, b.count,
                         clear);
            }
        }
        b.count = 0;
        b.windowStart = nowMs;
    }

    b.count += charge;
    if (b.count < charge) {
        b.count = UINT32_MAX;
    }

    if (!b.limited && b.count >= trip) {
        b.limited = true;
        b.windowStart = nowMs;
        LOG_WARN("[RateLimit] %s %s limited at count=%u trip=%u", nodeName, label, b.count, trip);
        return true;
    }

    return b.limited;
}

NodeRateLimiter::OriginatorEntry *NodeRateLimiter::findOriginator(NodeNum nodeId)
{
    for (uint8_t i = 0; i < originatorCount; i++) {
        if (originators[i].nodeId == nodeId) {
            return &originators[i];
        }
    }
    return nullptr;
}

int NodeRateLimiter::findOriginatorEvictionCandidate() const
{
    auto isLimited = [](const OriginatorEntry &e) {
        return e.text.limited || e.routing.limited || e.other.limited || e.unknown.limited;
    };
    auto oldestWindow = [](const OriginatorEntry &e) {
        return std::min(std::min(e.text.windowStart, e.routing.windowStart),
                        std::min(e.other.windowStart, e.unknown.windowStart));
    };

    // Prefer not-in-graph among non-limited
    int candidate = -1;
    for (int i = 0; i < originatorCount; i++) {
        if (isLimited(originators[i])) {
            continue;
        }
        bool inGraph = nodeInGraph(originators[i].nodeId);
        if (candidate == -1) {
            candidate = i;
            continue;
        }
        bool candInGraph = nodeInGraph(originators[candidate].nodeId);
        if (!inGraph && candInGraph) {
            candidate = i;
            continue;
        }
        if (inGraph == candInGraph) {
            uint8_t hops = graphHopsAway(originators[i].nodeId);
            uint8_t candHops = graphHopsAway(originators[candidate].nodeId);
            if (hops > candHops) {
                candidate = i;
            } else if (hops == candHops && oldestWindow(originators[i]) < oldestWindow(originators[candidate])) {
                candidate = i;
            }
        }
    }
    if (candidate != -1) {
        return candidate;
    }

    candidate = 0;
    for (int i = 1; i < originatorCount; i++) {
        if (oldestWindow(originators[i]) < oldestWindow(originators[candidate])) {
            candidate = i;
        }
    }
    return candidate;
}

NodeRateLimiter::OriginatorEntry *NodeRateLimiter::getOrCreateOriginator(NodeNum nodeId, uint32_t nowMs)
{
    OriginatorEntry *existing = findOriginator(nodeId);
    if (existing) {
        return existing;
    }

    OriginatorEntry *slot;
    if (originatorCount < MAX_ORIGINATOR_ENTRIES) {
        slot = &originators[originatorCount++];
    } else {
        slot = &originators[findOriginatorEvictionCandidate()];
    }

    *slot = OriginatorEntry{};
    slot->nodeId = nodeId;
    slot->text.windowStart = nowMs;
    slot->routing.windowStart = nowMs;
    slot->other.windowStart = nowMs;
    slot->unknown.windowStart = nowMs;
    return slot;
}

NodeRateLimiter::RelayEntry *NodeRateLimiter::findRelay(NodeNum nodeId)
{
    for (uint8_t i = 0; i < relayCount; i++) {
        if (relays[i].nodeId == nodeId) {
            return &relays[i];
        }
    }
    return nullptr;
}

int NodeRateLimiter::findRelayEvictionCandidate() const
{
    auto oldestWindow = [](const RelayEntry &e) { return e.relay.windowStart; };

    int candidate = -1;
    for (int i = 0; i < relayCount; i++) {
        if (relays[i].relay.limited) {
            continue;
        }
        bool inGraph = nodeInGraph(relays[i].nodeId);
        if (candidate == -1) {
            candidate = i;
            continue;
        }
        bool candInGraph = nodeInGraph(relays[candidate].nodeId);
        if (!inGraph && candInGraph) {
            candidate = i;
            continue;
        }
        if (inGraph == candInGraph) {
            uint8_t hops = graphHopsAway(relays[i].nodeId);
            uint8_t candHops = graphHopsAway(relays[candidate].nodeId);
            if (hops > candHops) {
                candidate = i;
            } else if (hops == candHops && oldestWindow(relays[i]) < oldestWindow(relays[candidate])) {
                candidate = i;
            }
        }
    }
    if (candidate != -1) {
        return candidate;
    }

    candidate = 0;
    for (int i = 1; i < relayCount; i++) {
        if (oldestWindow(relays[i]) < oldestWindow(relays[candidate])) {
            candidate = i;
        }
    }
    return candidate;
}

NodeRateLimiter::RelayEntry *NodeRateLimiter::getOrCreateRelay(NodeNum nodeId, uint32_t nowMs)
{
    RelayEntry *existing = findRelay(nodeId);
    if (existing) {
        return existing;
    }

    RelayEntry *slot;
    if (relayCount < MAX_RELAY_ENTRIES) {
        slot = &relays[relayCount++];
    } else {
        slot = &relays[findRelayEvictionCandidate()];
    }

    *slot = RelayEntry{};
    slot->nodeId = nodeId;
    slot->relay.windowStart = nowMs;
    return slot;
}

NodeRateLimiter::DestEntry *NodeRateLimiter::findDest(NodeNum nodeId)
{
    for (uint8_t i = 0; i < destCount; i++) {
        if (dests[i].nodeId == nodeId) {
            return &dests[i];
        }
    }
    return nullptr;
}

const NodeRateLimiter::DestEntry *NodeRateLimiter::findDest(NodeNum nodeId) const
{
    for (uint8_t i = 0; i < destCount; i++) {
        if (dests[i].nodeId == nodeId) {
            return &dests[i];
        }
    }
    return nullptr;
}

int NodeRateLimiter::findDestEvictionCandidate() const
{
    auto oldestWindow = [](const DestEntry &e) { return e.dest.windowStart; };

    int candidate = -1;
    for (int i = 0; i < destCount; i++) {
        if (dests[i].dest.limited) {
            continue;
        }
        bool inGraph = nodeInGraph(dests[i].nodeId);
        if (candidate == -1) {
            candidate = i;
            continue;
        }
        bool candInGraph = nodeInGraph(dests[candidate].nodeId);
        if (!inGraph && candInGraph) {
            candidate = i;
            continue;
        }
        if (inGraph == candInGraph) {
            uint8_t hops = graphHopsAway(dests[i].nodeId);
            uint8_t candHops = graphHopsAway(dests[candidate].nodeId);
            if (hops > candHops) {
                candidate = i;
            } else if (hops == candHops && oldestWindow(dests[i]) < oldestWindow(dests[candidate])) {
                candidate = i;
            }
        }
    }
    return candidate;
}

NodeRateLimiter::DestEntry *NodeRateLimiter::getOrCreateDest(NodeNum nodeId, uint32_t nowMs)
{
    DestEntry *existing = findDest(nodeId);
    if (existing) {
        return existing;
    }

    DestEntry *slot;
    if (destCount < MAX_DEST_ENTRIES) {
        slot = &dests[destCount++];
    } else {
        int evict = findDestEvictionCandidate();
        if (evict < 0) {
            return nullptr;
        }
        slot = &dests[evict];
    }

    *slot = DestEntry{};
    slot->nodeId = nodeId;
    slot->dest.windowStart = nowMs;
    return slot;
}

uint32_t NodeRateLimiter::nowMs() const
{
#if defined(UNIT_TEST)
    if (testNowOverride >= 0) {
        return (uint32_t)testNowOverride;
    }
#endif
    return millis();
}

void NodeRateLimiter::noteBoot(uint32_t now)
{
    if (!bootKnown) {
        bootMs = now;
        bootKnown = true;
    }
}

bool NodeRateLimiter::warmedUp(uint32_t now) const
{
    return bootKnown && (now - bootMs) >= WARMUP_MS;
}

int NodeRateLimiter::findYoung(NodeNum nodeId) const
{
    for (uint8_t i = 0; i < youngCount; i++) {
        if (young[i].nodeId == nodeId) {
            return i;
        }
    }
    return -1;
}

bool NodeRateLimiter::inAlumni(NodeNum nodeId) const
{
    for (uint8_t i = 0; i < alumniCount; i++) {
        if (alumni[i] == nodeId) {
            return true;
        }
    }
    return false;
}

void NodeRateLimiter::addAlumni(NodeNum nodeId)
{
    if (inAlumni(nodeId)) {
        return;
    }
    if (alumniCount < MAX_YOUNG_ENTRIES) {
        alumni[alumniCount++] = nodeId;
        return;
    }
    memmove(&alumni[0], &alumni[1], (MAX_YOUNG_ENTRIES - 1) * sizeof(alumni[0]));
    alumni[MAX_YOUNG_ENTRIES - 1] = nodeId;
}

void NodeRateLimiter::removeYoungAt(uint8_t idx)
{
    uint8_t last = youngCount - 1;
    if (idx < last) {
        young[idx] = young[last];
    }
    young[last] = YoungSighting{};
    youngCount--;
}

void NodeRateLimiter::expireIfOld(NodeNum nodeId, uint32_t now)
{
    int idx = findYoung(nodeId);
    if (idx < 0) {
        return;
    }
    uint32_t age = now - young[idx].firstSeenMs;
    if (age >= YOUNG_AGE_MS) {
        addAlumni(nodeId);
        removeYoungAt((uint8_t)idx);
    }
}

void NodeRateLimiter::noteOriginator(NodeNum nodeId, uint32_t now)
{
    expireIfOld(nodeId, now);
    if (findYoung(nodeId) >= 0 || inAlumni(nodeId)) {
        return;
    }
    if (youngCount < MAX_YOUNG_ENTRIES) {
        young[youngCount].nodeId = nodeId;
        young[youngCount].firstSeenMs = now;
        youngCount++;
    }
}

bool NodeRateLimiter::isYoung(NodeNum nodeId, uint32_t now) const
{
    int idx = findYoung(nodeId);
    if (idx >= 0) {
        return (now - young[idx].firstSeenMs) < YOUNG_AGE_MS;
    }
    if (inAlumni(nodeId)) {
        return false;
    }
    return youngCount >= MAX_YOUNG_ENTRIES;
}

bool NodeRateLimiter::relayAnyLimited() const
{
    if (unresolvedRelay.limited) {
        return true;
    }
    for (uint8_t i = 0; i < relayCount; i++) {
        if (relays[i].relay.limited) {
            return true;
        }
    }
    return false;
}

void NodeRateLimiter::maybeAnnounce(uint32_t now, float chutil)
{
    if (relayAnyLimited() || chutil > ANNOUNCE_CHUTIL_HIGH) {
        return;
    }
    if (announceEver && (now - lastAnnounceMs) < ANNOUNCE_REFRACTORY_MS) {
        return;
    }
    pendingAnnounceCount = 0;
    uint8_t n = youngCount < ANNOUNCE_MAX_IDS ? youngCount : ANNOUNCE_MAX_IDS;
    for (uint8_t i = 0; i < n; i++) {
        pendingAnnounceIds[i] = young[i].nodeId;
        pendingAnnounceCount++;
    }
    lastAnnounceMs = now;
    announceEver = true;
    pendingAnnounce = true;
}

bool NodeRateLimiter::takeYoungAnnounce(NodeNum *ids, uint8_t &count)
{
    if (!pendingAnnounce) {
        count = 0;
        return false;
    }
    count = pendingAnnounceCount;
    if (ids) {
        for (uint8_t i = 0; i < count; i++) {
            ids[i] = pendingAnnounceIds[i];
        }
    }
    pendingAnnounce = false;
    pendingAnnounceCount = 0;
    return true;
}

bool NodeRateLimiter::isDroppedCoverageTarget(NodeNum nodeId) const
{
    if (!nodeId || !warmedUp(nowMs()) || !youngBucket.limited) {
        return false;
    }
    return isYoung(nodeId, nowMs());
}

bool NodeRateLimiter::isLimitedOriginator(NodeNum nodeId) const
{
    if (!nodeId) {
        return false;
    }
    for (uint8_t i = 0; i < originatorCount; i++) {
        if (originators[i].nodeId != nodeId) {
            continue;
        }
        return originators[i].text.limited || originators[i].routing.limited || originators[i].other.limited ||
               originators[i].unknown.limited;
    }
    return false;
}

bool NodeRateLimiter::isOnDefaultChannel(const meshtastic_MeshPacket *p) const
{
    if (!p || p->pki_encrypted) {
        return false;
    }
#if defined(UNIT_TEST)
    if (testOnDefaultChannelOverride >= 0) {
        return testOnDefaultChannelOverride != 0;
    }
    return true;
#else
    return channels.isDefaultChannel(p->channel);
#endif
}

bool NodeRateLimiter::shouldDropToLimitedDest(const meshtastic_MeshPacket *p) const
{
    if (!p || isBroadcast(p->to) || p->to == 0) {
        return false;
    }
    if (!isOnDefaultChannel(p)) {
        return false;
    }
    return isLimitedOriginator(p->to) || isLimitedDest(p->to);
}

bool NodeRateLimiter::isLimitedDest(NodeNum nodeId) const
{
    const DestEntry *e = findDest(nodeId);
    return e && e->dest.limited;
}

bool NodeRateLimiter::isDestVolumePort(meshtastic_PortNum portnum)
{
    return portnum == meshtastic_PortNum_POSITION_APP || portnum == meshtastic_PortNum_NODEINFO_APP ||
           portnum == meshtastic_PortNum_TELEMETRY_APP;
}

bool NodeRateLimiter::chargesDestVolume(const meshtastic_MeshPacket *p) const
{
    if (!p || isBroadcast(p->to) || p->to == 0) {
        return false;
    }
    if (!isOnDefaultChannel(p)) {
        return false;
    }
    if (!isRebroadcastCandidate(p)) {
        return false;
    }
    if (p->which_payload_variant != meshtastic_MeshPacket_decoded_tag) {
        return false;
    }
    return isDestVolumePort(p->decoded.portnum);
}

bool NodeRateLimiter::destWindowWillRoll(const BucketState &b, uint32_t nowMs, uint32_t windowMs)
{
    return b.windowStart != 0 && (nowMs - b.windowStart) >= windowMs;
}

void NodeRateLimiter::rollDestSenders(DestEntry &entry, uint32_t nowMs, uint32_t windowMs)
{
    if (destWindowWillRoll(entry.dest, nowMs, windowMs)) {
        memset(entry.senders, 0, sizeof(entry.senders));
        entry.senderCount = 0;
    }
}

void NodeRateLimiter::noteDestSender(DestEntry &entry, NodeNum from)
{
    if (!from) {
        return;
    }
    for (uint8_t i = 0; i < entry.senderCount; i++) {
        if (entry.senders[i] == from) {
            return;
        }
    }
    if (entry.senderCount < DEST_SENDER_SLOTS) {
        entry.senders[entry.senderCount++] = from;
    }
}

bool NodeRateLimiter::chargeDestVolume(const meshtastic_MeshPacket *p)
{
    if (!p || isBroadcast(p->to) || p->to == 0 || !isOnDefaultChannel(p)) {
        return false;
    }

    const uint32_t now = nowMs();
    if (chargesDestVolume(p)) {
        DestEntry *entry = getOrCreateDest(p->to, now);
        if (!entry) {
            return isLimitedDest(p->to);
        }
        rollDestSenders(*entry, now, cfgWindowMs);
        noteDestSender(*entry, p->from);
        const uint32_t trip = (entry->dest.limited || entry->senderCount >= DEST_MIN_SENDERS) ? DEST_TRIP : UINT32_MAX;
        return checkAndUpdateBucket(entry->dest, trip, DEST_CLEAR, 1, now, "dest", entry->nodeId);
    }

    DestEntry *entry = findDest(p->to);
    if (entry) {
        rollDestSenders(*entry, now, cfgWindowMs);
        checkAndUpdateBucket(entry->dest, DEST_TRIP, DEST_CLEAR, 0, now, "dest", entry->nodeId);
    }
    return isLimitedDest(p->to);
}

bool NodeRateLimiter::shouldDrop(const meshtastic_MeshPacket *p)
{
    lastDestDrop = false;
    if (!cfgEnabled || !p) {
        return false;
    }
    if (p->from == 0 || isFromUs(p)) {
        return false;
    }
    if (isToUs(p)) {
        return false;
    }

    uint32_t nowMs = this->nowMs();
    noteBoot(nowMs);

    const bool destVolumeDrop = chargeDestVolume(p);
    if (destVolumeDrop || shouldDropToLimitedDest(p)) {
        lastDestDrop = true;
        return true;
    }
    pendingAnnounce = false;
    bool drop = false;
    const bool decoded = p->which_payload_variant == meshtastic_MeshPacket_decoded_tag;
    if (decoded) {
        noteOriginator(p->from, nowMs);
    }

    // --- Originator buckets (favorites bypass originator only) ---
    bool favoriteOriginator = false;
    if (nodeDB) {
        const meshtastic_NodeInfoLite *node = nodeDB->getMeshNode(p->from);
        favoriteOriginator = node && node->is_favorite;
    }

    if (!favoriteOriginator) {
        Bucket bucket;
        uint32_t trip;
        if (p->which_payload_variant == meshtastic_MeshPacket_decoded_tag) {
            bucket = classifyBucket(p->decoded.portnum);
        } else {
            bucket = Bucket::UNKNOWN;
        }
        switch (bucket) {
        case Bucket::TEXT:
            trip = cfgTextTrip;
            break;
        case Bucket::ROUTING:
            trip = cfgRoutingTrip;
            break;
        case Bucket::UNKNOWN:
            trip = cfgUnknownTrip;
            break;
        default:
            trip = cfgOtherTrip;
            break;
        }

        OriginatorEntry *entry = getOrCreateOriginator(p->from, nowMs);
        BucketState *b;
        const char *label;
        switch (bucket) {
        case Bucket::TEXT:
            b = &entry->text;
            label = "text";
            break;
        case Bucket::ROUTING:
            b = &entry->routing;
            label = "routing";
            break;
        case Bucket::UNKNOWN:
            b = &entry->unknown;
            label = "unknown";
            break;
        default:
            b = &entry->other;
            label = "other";
            break;
        }
        // A limited originator clears when a whole window comes in under the clear level,
        // instead of needing a fully silent one it can never get while it keeps talking.
        const uint32_t clear = std::max<uint32_t>(1, (trip * DEFAULT_CLEAR_RATIO_NUM) / DEFAULT_CLEAR_RATIO_DEN);
        if (checkAndUpdateBucket(*b, trip, clear, 1, nowMs, label, entry->nodeId)) {
            drop = true;
        }
    }

    if (decoded && warmedUp(nowMs) && isYoung(p->from, nowMs)) {
        if (checkAndUpdateBucket(youngBucket, YOUNG_TRIP, YOUNG_CLEAR, 1, nowMs, "young", 0)) {
            drop = true;
        }
        if (youngBucket.limited && youngBucket.count == YOUNG_TRIP) {
            float chutil = 0.0f;
#if defined(UNIT_TEST)
            if (testChutilOverride >= 0.0f) {
                chutil = testChutilOverride;
            } else
#endif
                if (airTime) {
                chutil = airTime->channelUtilizationPercent();
            }
            maybeAnnounce(nowMs, chutil);
        }
    }

    // --- RELAY (rebroadcast candidates only; never favorite-bypassed) ---
    if (isRebroadcastCandidate(p) && !drop) {
        // If originator already limited we would drop above; B7 N/A. Here !drop means still a candidate.
        NodeNum resolved = 0;
        bool resolvedOk = false;
#if defined(UNIT_TEST)
        if (testResolveRelayHook && p->relay_node != 0) {
            resolved = testResolveRelayHook(p->relay_node, p->rx_rssi, p->rx_snr);
            if (resolved != 0 && !SignalRoutingModule::isPlaceholderNodeId(resolved) &&
                (!nodeDB || resolved != nodeDB->getNodeNum())) {
                resolvedOk = true;
            }
        } else
#endif
            if (p->relay_node != 0 && signalRoutingModule) {
            resolved = signalRoutingModule->resolveRelayIdentity(p->relay_node, p->rx_rssi, p->rx_snr);
            if (resolved != 0 && !signalRoutingModule->isPlaceholderNode(resolved) &&
                (!nodeDB || resolved != nodeDB->getNodeNum())) {
                resolvedOk = true;
            }
        }

        uint32_t airMs = packetAirtimeMs(p);
        uint32_t tripMs = 0;
        uint32_t clearMs = 0;
        relayBudgets(airMs, tripMs, clearMs);

        if (unresolvedRelay.windowStart == 0) {
            unresolvedRelay.windowStart = nowMs;
        }

        if (resolvedOk) {
            RelayEntry *re = getOrCreateRelay(resolved, nowMs);
            if (checkAndUpdateBucket(re->relay, tripMs, clearMs, airMs, nowMs, "relay", re->nodeId)) {
                drop = true;
            }
        } else if (shouldChargeUnresolvedRelay(signalRoutingModule != nullptr,
                                               signalRoutingModule && signalRoutingModule->hasEstablishedGraph())) {
            if (checkAndUpdateBucket(unresolvedRelay, tripMs, clearMs, airMs, nowMs, "relay-unresolved", 0)) {
                drop = true;
            }
        }
    }

    return drop;
}

bool NodeRateLimiter::wouldDrop(const meshtastic_MeshPacket *p) const
{
    if (!cfgEnabled || !p) {
        return false;
    }
    if (p->from == 0 || isFromUs(p)) {
        return false;
    }
    if (isToUs(p)) {
        return false;
    }
    if (shouldDropToLimitedDest(p)) {
        return true;
    }

    bool favoriteOriginator = false;
    if (nodeDB) {
        const meshtastic_NodeInfoLite *node = nodeDB->getMeshNode(p->from);
        favoriteOriginator = node && node->is_favorite;
    }

    if (!favoriteOriginator) {
        Bucket bucket;
        if (p->which_payload_variant == meshtastic_MeshPacket_decoded_tag) {
            bucket = classifyBucket(p->decoded.portnum);
        } else {
            bucket = Bucket::UNKNOWN;
        }
        for (uint8_t i = 0; i < originatorCount; i++) {
            if (originators[i].nodeId != p->from) {
                continue;
            }
            const BucketState *b = nullptr;
            switch (bucket) {
            case Bucket::TEXT:
                b = &originators[i].text;
                break;
            case Bucket::ROUTING:
                b = &originators[i].routing;
                break;
            case Bucket::UNKNOWN:
                b = &originators[i].unknown;
                break;
            default:
                b = &originators[i].other;
                break;
            }
            if (b && b->limited) {
                return true;
            }
            break;
        }
    }

    if (p->which_payload_variant == meshtastic_MeshPacket_decoded_tag && youngBucket.limited &&
        warmedUp(nowMs()) && isYoung(p->from, nowMs())) {
        return true;
    }

    if (!isRebroadcastCandidate(p)) {
        return false;
    }

    NodeNum resolved = 0;
    bool resolvedOk = false;
#if defined(UNIT_TEST)
    if (testResolveRelayHook && p->relay_node != 0) {
        resolved = testResolveRelayHook(p->relay_node, p->rx_rssi, p->rx_snr);
        if (resolved != 0 && !SignalRoutingModule::isPlaceholderNodeId(resolved) &&
            (!nodeDB || resolved != nodeDB->getNodeNum())) {
            resolvedOk = true;
        }
    } else
#endif
        if (p->relay_node != 0 && signalRoutingModule) {
        resolved = signalRoutingModule->resolveRelayIdentity(p->relay_node, p->rx_rssi, p->rx_snr);
        if (resolved != 0 && !signalRoutingModule->isPlaceholderNode(resolved) &&
            (!nodeDB || resolved != nodeDB->getNodeNum())) {
            resolvedOk = true;
        }
    }

    if (resolvedOk) {
        for (uint8_t i = 0; i < relayCount; i++) {
            if (relays[i].nodeId == resolved && relays[i].relay.limited) {
                return true;
            }
        }
        return false;
    }
    return unresolvedRelay.limited;
}

#endif // !MESHTASTIC_EXCLUDE_SIGNALROUTING
