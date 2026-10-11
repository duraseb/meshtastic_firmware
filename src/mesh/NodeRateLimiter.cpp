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

template <typename Entry, typename Limited, typename Age, typename InGraph, typename Hops>
static int selectEvictionCandidate(const Entry *entries, uint8_t count, Limited limited, Age age, InGraph inGraph, Hops hops)
{
    int candidate = -1;
    bool candidateInGraph = false;
    uint8_t candidateHops = 0;
    uint32_t candidateAge = 0;
    for (uint8_t i = 0; i < count; i++) {
        const auto &entry = entries[i];
        // Identity churn must not erase active limits when the table fills.
        if (limited(entry)) {
            continue;
        }
        const bool present = inGraph(entry.nodeId);
        const uint8_t distance = present ? hops(entry.nodeId) : 0;
        const uint32_t entryAge = age(entry);
        if (candidate < 0 || (!present && candidateInGraph) ||
            (present == candidateInGraph &&
             (distance > candidateHops || (distance == candidateHops && entryAge > candidateAge)))) {
            candidate = i;
            candidateInGraph = present;
            candidateHops = distance;
            candidateAge = entryAge;
        }
    }
    return candidate;
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
    bootWarmed = false;
}

void NodeRateLimiter::debugSeedYoung(NodeNum nodeId, uint32_t firstSeenMs)
{
    if (findYoung(nodeId) >= 0 || inAlumni(nodeId) || youngCount >= MAX_YOUNG_ENTRIES) {
        return;
    }
    young[youngCount] = {};
    young[youngCount].nodeId = nodeId;
    young[youngCount].firstSeenMs = firstSeenMs;
    youngCount++;
}

bool NodeRateLimiter::debugYoungTableFull() const
{
    return activeYoungCount(nowMs()) >= MAX_YOUNG_ENTRIES;
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

NodeRateLimiter::NodeRateLimiter()
{
    if (moduleConfig.has_node_rate_limiter) {
        const auto &cfg = moduleConfig.node_rate_limiter;
        cfgEnabled = cfg.enabled;
        if (cfg.window_secs != 0) {
            cfgWindowMs = std::min<uint32_t>(cfg.window_secs, INT32_MAX / 1000u) * 1000u;
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

    uint32_t baseTrip = RELAY_TRIP_PACKETS * std::min<uint32_t>(airMs, RELAY_TRIP_CEIL_MS);

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

uint32_t NodeRateLimiter::originatorClear(uint32_t trip)
{
    return std::max<uint32_t>(1, (trip * DEFAULT_CLEAR_RATIO_NUM) / DEFAULT_CLEAR_RATIO_DEN);
}

bool NodeRateLimiter::bucketLimited(const BucketState &b, uint32_t clear, uint32_t now) const
{
    if (!b.limited) {
        return false;
    }
    // Read-only relay and destination gates must recover even when no packet updates the bucket.
    const uint32_t windows = (now - b.windowStart) / cfgWindowMs;
    return windows == 0 || (windows == 1 && b.count >= clear);
}

bool NodeRateLimiter::checkAndUpdateBucket(BucketState &b, uint32_t trip, uint32_t clear, uint32_t charge, uint32_t now,
                                           const char *label, NodeNum nodeId)
{
    const bool wasLimited = b.limited;
    if (now - b.windowStart >= cfgWindowMs) {
        b.limited = bucketLimited(b, clear, now);
        const uint32_t windows = (now - b.windowStart) / cfgWindowMs;
        b.windowStart += windows * cfgWindowMs;
        b.count = 0;
    }
    // Counter wrap would turn heavy traffic into an apparently empty budget.
    b.count = charge > UINT32_MAX - b.count ? UINT32_MAX : b.count + charge;
    if (!b.limited && b.count >= trip) {
        b.limited = true;
        b.windowStart = now;
    }
    if (wasLimited != b.limited) {
        char nodeName[48];
        getNodeDisplayName(nodeId, nodeName, sizeof(nodeName));
        LOG_INFO("[RateLimit] %s %s %s (count=%u trip=%u clear=%u)", nodeName, label, b.limited ? "limited" : "unlimited",
                 b.count, trip, clear);
    }
    return b.limited;
}

NodeRateLimiter::OriginatorEntry *NodeRateLimiter::findOriginator(NodeNum nodeId)
{
    return const_cast<OriginatorEntry *>(static_cast<const NodeRateLimiter *>(this)->findOriginator(nodeId));
}

const NodeRateLimiter::OriginatorEntry *NodeRateLimiter::findOriginator(NodeNum nodeId) const
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
    const uint32_t now = nowMs();
    return selectEvictionCandidate(
        originators, originatorCount, [this, now](const OriginatorEntry &e) { return isLimitedOriginator(e.nodeId); },
        [now](const OriginatorEntry &e) {
            return std::max(std::max(now - e.text.windowStart, now - e.routing.windowStart),
                            std::max(now - e.other.windowStart, now - e.unknown.windowStart));
        },
        nodeInGraph, graphHopsAway);
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
        const int evict = findOriginatorEvictionCandidate();
        if (evict < 0) {
            return nullptr;
        }
        slot = &originators[evict];
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
    const uint32_t now = nowMs();
    return selectEvictionCandidate(
        relays, relayCount, [this, now](const RelayEntry &e) { return bucketLimited(e.relay, RELAY_CLEAR_FLOOR_MS, now); },
        [now](const RelayEntry &e) { return now - e.relay.windowStart; }, nodeInGraph, graphHopsAway);
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
        const int evict = findRelayEvictionCandidate();
        if (evict < 0) {
            return nullptr;
        }
        slot = &relays[evict];
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
    const uint32_t now = nowMs();
    return selectEvictionCandidate(
        dests, destCount, [this, now](const DestEntry &e) { return bucketLimited(e.dest, DEST_CLEAR, now); },
        [now](const DestEntry &e) { return now - e.dest.windowStart; }, nodeInGraph, graphHopsAway);
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
    // Established identities must not get another warm-up exemption at clock rollover.
    bootWarmed |= bootKnown && (now - bootMs) >= WARMUP_MS;
    return bootWarmed;
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

bool NodeRateLimiter::addAlumni(NodeNum nodeId)
{
    if (inAlumni(nodeId)) {
        return true;
    }
    if (alumniCount >= MAX_ALUMNI_ENTRIES) {
        // Forgetting an established identity would enroll it as young again.
        return false;
    }
    alumni[alumniCount++] = nodeId;
    return true;
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

bool NodeRateLimiter::youngAgeActive(const YoungSighting &sighting, uint32_t now)
{
    // Graduation must not reverse after the millisecond clock wraps.
    sighting.aged |= (now - sighting.firstSeenMs) >= YOUNG_AGE_MS;
    return !sighting.aged;
}

uint8_t NodeRateLimiter::activeYoungCount(uint32_t now) const
{
    uint8_t n = 0;
    for (uint8_t i = 0; i < youngCount; i++) {
        if (youngAgeActive(young[i], now)) {
            n++;
        }
    }
    return n;
}

int NodeRateLimiter::findAgedYoung(uint32_t now) const
{
    for (uint8_t i = 0; i < youngCount; i++) {
        if (!youngAgeActive(young[i], now)) {
            return i;
        }
    }
    return -1;
}

void NodeRateLimiter::expireIfOld(NodeNum nodeId, uint32_t now)
{
    int idx = findYoung(nodeId);
    if (idx < 0) {
        return;
    }
    if (youngAgeActive(young[idx], now)) {
        return;
    }
    // Parking preserves graduation when the alumni table has no room.
    if (addAlumni(nodeId)) {
        removeYoungAt((uint8_t)idx);
    }
}

void NodeRateLimiter::insertYoung(NodeNum nodeId, uint32_t now)
{
    if (youngCount >= MAX_YOUNG_ENTRIES) {
        return;
    }
    young[youngCount] = {};
    young[youngCount].nodeId = nodeId;
    young[youngCount].firstSeenMs = now;
    youngCount++;
}

void NodeRateLimiter::noteOriginator(NodeNum nodeId, uint32_t now)
{
    expireIfOld(nodeId, now);
    if (findYoung(nodeId) >= 0 || inAlumni(nodeId)) {
        return;
    }
    // A reboot makes every established peer look new.
    if (!warmedUp(now)) {
        (void)addAlumni(nodeId);
        return;
    }
    if (youngCount < MAX_YOUNG_ENTRIES) {
        insertYoung(nodeId, now);
        return;
    }
    // Reclaiming parked rows avoids charging the entire established mesh when alumni storage is exhausted.
    int idx = findAgedYoung(now);
    if (idx < 0) {
        return;
    }
    NodeNum old = young[idx].nodeId;
    (void)addAlumni(old);
    removeYoungAt((uint8_t)idx);
    insertYoung(nodeId, now);
}

bool NodeRateLimiter::isYoung(NodeNum nodeId, uint32_t now) const
{
    int idx = findYoung(nodeId);
    if (idx >= 0) {
        return youngAgeActive(young[idx], now);
    }
    if (inAlumni(nodeId)) {
        return false;
    }
    // Identity churn must not bypass the shared budget by filling the active-young table.
    return activeYoungCount(now) >= MAX_YOUNG_ENTRIES;
}

bool NodeRateLimiter::relayAnyLimited() const
{
    if (bucketLimited(unresolvedRelay, RELAY_CLEAR_FLOOR_MS, nowMs())) {
        return true;
    }
    for (uint8_t i = 0; i < relayCount; i++) {
        if (bucketLimited(relays[i].relay, RELAY_CLEAR_FLOOR_MS, nowMs())) {
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
    for (uint8_t i = 0; i < youngCount && pendingAnnounceCount < ANNOUNCE_MAX_IDS; i++) {
        if (!youngAgeActive(young[i], now)) {
            continue;
        }
        pendingAnnounceIds[pendingAnnounceCount++] = young[i].nodeId;
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
    if (!cfgEnabled || !nodeId || !warmedUp(nowMs()) || !bucketLimited(youngBucket, YOUNG_CLEAR, nowMs())) {
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
        const auto &e = originators[i];
        const uint32_t now = nowMs();
        return bucketLimited(e.text, originatorClear(cfgTextTrip), now) ||
               bucketLimited(e.routing, originatorClear(cfgRoutingTrip), now) ||
               bucketLimited(e.other, originatorClear(cfgOtherTrip), now) ||
               bucketLimited(e.unknown, originatorClear(cfgUnknownTrip), now);
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
    return e && bucketLimited(e->dest, DEST_CLEAR, nowMs());
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
    return (nowMs - b.windowStart) >= windowMs;
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
        const uint32_t trip = (entry->dest.limited || entry->senderCount >= DEST_MIN_SENDERS) ? DEST_TRIP : UINT32_MAX;
        checkAndUpdateBucket(entry->dest, trip, DEST_CLEAR, 0, now, "dest", entry->nodeId);
    }
    return isLimitedDest(p->to);
}

NodeNum NodeRateLimiter::resolveRelay(const meshtastic_MeshPacket *p)
{
    if (p->relay_node == 0) {
        return 0;
    }
    NodeNum resolved = 0;
#if defined(UNIT_TEST)
    if (testResolveRelayHook) {
        resolved = testResolveRelayHook(p->relay_node, p->rx_rssi, p->rx_snr);
    } else
#endif
        if (signalRoutingModule) {
        resolved = signalRoutingModule->resolveRelayIdentity(p->relay_node, p->rx_rssi, p->rx_snr);
    }
    return resolved != 0 && !SignalRoutingModule::isPlaceholderNodeId(resolved) && (!nodeDB || resolved != nodeDB->getNodeNum())
               ? resolved
               : 0;
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
    bool drop = false;
    const bool decoded = p->which_payload_variant == meshtastic_MeshPacket_decoded_tag;
    if (decoded) {
        noteOriginator(p->from, nowMs);
    }

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
        if (!entry) {
            return true;
        }
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
        // Traffic that has slowed below hysteresis should recover without needing complete silence.
        const uint32_t clear = originatorClear(trip);
        if (checkAndUpdateBucket(*b, trip, clear, 1, nowMs, label, entry->nodeId)) {
            drop = true;
        }
    }

    if (decoded && warmedUp(nowMs) && isYoung(p->from, nowMs)) {
        const bool wasLimited = bucketLimited(youngBucket, YOUNG_CLEAR, nowMs);
        if (checkAndUpdateBucket(youngBucket, YOUNG_TRIP, YOUNG_CLEAR, 1, nowMs, "young", 0)) {
            drop = true;
        }
        if (!wasLimited && youngBucket.limited) {
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

    if (isRebroadcastCandidate(p) && !drop) {
        const NodeNum resolved = resolveRelay(p);
        uint32_t airMs = packetAirtimeMs(p);
        uint32_t tripMs = 0;
        uint32_t clearMs = 0;
        relayBudgets(airMs, tripMs, clearMs);

        if (unresolvedRelay.count == 0 && !unresolvedRelay.limited) {
            unresolvedRelay.windowStart = nowMs;
        }

        if (resolved != 0) {
            RelayEntry *re = getOrCreateRelay(resolved, nowMs);
            if (!re || checkAndUpdateBucket(re->relay, tripMs, clearMs, airMs, nowMs, "relay", re->nodeId)) {
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
            uint32_t trip = bucket == Bucket::TEXT      ? cfgTextTrip
                            : bucket == Bucket::ROUTING ? cfgRoutingTrip
                            : bucket == Bucket::UNKNOWN ? cfgUnknownTrip
                                                        : cfgOtherTrip;
            if (b && bucketLimited(*b, originatorClear(trip), nowMs())) {
                return true;
            }
            break;
        }
        if (!findOriginator(p->from) && originatorCount == MAX_ORIGINATOR_ENTRIES && findOriginatorEvictionCandidate() < 0) {
            return true;
        }
    }

    if (p->which_payload_variant == meshtastic_MeshPacket_decoded_tag && bucketLimited(youngBucket, YOUNG_CLEAR, nowMs()) &&
        warmedUp(nowMs()) && isYoung(p->from, nowMs())) {
        return true;
    }

    if (!isRebroadcastCandidate(p)) {
        return false;
    }

    const NodeNum resolved = resolveRelay(p);

    if (resolved != 0) {
        for (uint8_t i = 0; i < relayCount; i++) {
            if (relays[i].nodeId == resolved) {
                uint32_t tripMs, clearMs;
                relayBudgets(packetAirtimeMs(p), tripMs, clearMs);
                return bucketLimited(relays[i].relay, clearMs, nowMs());
            }
        }
        return relayCount == MAX_RELAY_ENTRIES && findRelayEvictionCandidate() < 0;
    }
    if (!shouldChargeUnresolvedRelay(signalRoutingModule != nullptr,
                                     signalRoutingModule && signalRoutingModule->hasEstablishedGraph())) {
        return false;
    }
    uint32_t tripMs, clearMs;
    relayBudgets(packetAirtimeMs(p), tripMs, clearMs);
    return bucketLimited(unresolvedRelay, clearMs, nowMs());
}

#endif
