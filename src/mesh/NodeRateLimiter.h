#pragma once

#include "MeshTypes.h"
#include "mesh/generated/meshtastic/mesh.pb.h"

// Destination bans stop response floods from continuing after their requester is silenced.
// Private channels are exempt because their traffic does not create the public-channel flood.
class NodeRateLimiter
{
  public:
    NodeRateLimiter();

    bool shouldDrop(const meshtastic_MeshPacket *p);

    bool lastDropWasDest() const { return lastDestDrop; }

    bool wouldDrop(const meshtastic_MeshPacket *p) const;

#if defined(UNIT_TEST)

    static NodeNum (*testResolveRelayHook)(uint8_t relayByte, int16_t rssi, float snr);
    static bool (*testNodeInGraphHook)(NodeNum nodeId);
    static uint8_t (*testGraphHopsHook)(NodeNum nodeId);
    static float testChutilOverride;
    static int8_t testOnDefaultChannelOverride;

    bool debugTracksOriginator(NodeNum nodeId) const;
    bool debugOriginatorLimited(NodeNum nodeId) const;
    bool debugTracksRelay(NodeNum nodeId) const;
    bool debugRelayLimited(NodeNum nodeId) const;
    bool debugUnresolvedRelayLimited() const;
    void debugRelayBudgets(uint32_t airMs, uint32_t &tripMs, uint32_t &clearMs) const;
    uint8_t debugOriginatorCount() const { return originatorCount; }
    uint8_t debugRelayCount() const { return relayCount; }
    bool debugTracksYoung(NodeNum nodeId) const;
    uint8_t debugYoungCount() const { return youngCount; }
    bool debugInAlumni(NodeNum nodeId) const { return inAlumni(nodeId); }
    uint8_t debugAlumniCount() const { return alumniCount; }
    bool debugIsYoung(NodeNum nodeId) const;
    bool debugYoungLimited() const { return youngBucket.limited; }
    uint32_t debugYoungCharge() const { return youngBucket.count; }

    bool debugYoungTableFull() const;
    bool debugTracksDest(NodeNum nodeId) const;
    bool debugDestLimited(NodeNum nodeId) const;
    uint32_t debugDestCharge(NodeNum nodeId) const;
    uint8_t debugDestSenders(NodeNum nodeId) const;
    uint8_t debugDestCount() const { return destCount; }
    void debugSetBootMs(uint32_t bootMs);
    void debugSeedYoung(NodeNum nodeId, uint32_t firstSeenMs);
    bool debugTakeAnnounce(NodeNum *ids, uint8_t &count);
    void debugSetAnnounceBroadcast(bool enabled) { cfgAnnounceBroadcast = enabled; }
    static int64_t testNowOverride;
#endif

    static constexpr uint8_t DEFAULT_TEXT_TRIP = 30;
    static constexpr uint8_t DEFAULT_ROUTING_TRIP = 10;
    static constexpr uint8_t DEFAULT_OTHER_TRIP = 4;
    static constexpr uint8_t DEFAULT_UNKNOWN_TRIP = 12;
    // Small originator thresholds need half-trip hysteresis: quarter-trip would require silence for OTHER.
    static constexpr uint8_t DEFAULT_CLEAR_RATIO_NUM = 1;
    static constexpr uint8_t DEFAULT_CLEAR_RATIO_DEN = 2;

    // Boot-time ambiguity is caused by an empty graph, not necessarily abusive relays.
    static bool shouldChargeUnresolvedRelay(bool srPresent, bool graphEstablished) { return !srPresent || graphEstablished; }

    bool isDroppedCoverageTarget(NodeNum nodeId) const;

    bool takeYoungAnnounce(NodeNum *ids, uint8_t &count);

    bool announceBroadcastEnabled() const { return cfgAnnounceBroadcast; }

    static constexpr uint8_t MAX_YOUNG_ENTRIES = 32;
    static constexpr uint8_t MAX_ALUMNI_ENTRIES = 128;
    static constexpr uint32_t YOUNG_AGE_MS = 30u * 60u * 1000u;
    static constexpr uint32_t WARMUP_MS = 30u * 60u * 1000u;
    // The trip budget is twice the measured legitimate peak of 24 young packets per 90 seconds.
    static constexpr uint8_t YOUNG_TRIP = 48;
    static constexpr uint8_t YOUNG_CLEAR = 12;
    static constexpr uint32_t ANNOUNCE_REFRACTORY_MS = 30u * 60u * 1000u;
    static constexpr float ANNOUNCE_CHUTIL_HIGH = 25.0f;
    static constexpr uint8_t ANNOUNCE_MAX_IDS = 4;

    static constexpr uint8_t MAX_DEST_ENTRIES = 8;
    static constexpr uint8_t DEST_TRIP = 20;
    static constexpr uint8_t DEST_CLEAR = 5;
    static constexpr uint8_t DEST_MIN_SENDERS = 3;
    static constexpr uint8_t DEST_SENDER_SLOTS = 8;

  private:
    static constexpr uint8_t MAX_ORIGINATOR_ENTRIES = 16;
    static constexpr uint8_t MAX_RELAY_ENTRIES = 8;

    static constexpr uint32_t DEFAULT_WINDOW_MS = 90u * 1000u;

    static constexpr uint8_t RELAY_TRIP_PACKETS = 60;
    static constexpr uint8_t RELAY_CLEAR_PACKETS = 15;
    static constexpr uint32_t RELAY_REF_AIRTIME_MS = 100;
    static constexpr uint32_t RELAY_TRIP_FLOOR_MS = 20u * RELAY_REF_AIRTIME_MS;
    static constexpr uint32_t RELAY_TRIP_CEIL_MS = 120u * RELAY_REF_AIRTIME_MS;
    static constexpr uint32_t RELAY_CLEAR_FLOOR_MS = 5u * RELAY_REF_AIRTIME_MS;
    static constexpr uint32_t RELAY_CLEAR_CEIL_MS = 40u * RELAY_REF_AIRTIME_MS;

    bool cfgEnabled = true;
    uint32_t cfgWindowMs = DEFAULT_WINDOW_MS;
    uint8_t cfgTextTrip = DEFAULT_TEXT_TRIP;
    uint8_t cfgRoutingTrip = DEFAULT_ROUTING_TRIP;
    uint8_t cfgOtherTrip = DEFAULT_OTHER_TRIP;
    uint8_t cfgUnknownTrip = DEFAULT_UNKNOWN_TRIP;

    enum class Bucket : uint8_t { TEXT, ROUTING, OTHER, UNKNOWN };

    struct BucketState {
        uint32_t windowStart = 0;
        uint32_t count = 0;
        bool limited = false;
    };

    struct OriginatorEntry {
        NodeNum nodeId = 0;
        BucketState text;
        BucketState routing;
        BucketState other;
        BucketState unknown;
    };

    struct RelayEntry {
        NodeNum nodeId = 0;
        BucketState relay;
    };

    struct DestEntry {
        NodeNum nodeId = 0;
        BucketState dest;
        NodeNum senders[DEST_SENDER_SLOTS] = {};
        uint8_t senderCount = 0;
    };

    OriginatorEntry originators[MAX_ORIGINATOR_ENTRIES];
    uint8_t originatorCount = 0;

    RelayEntry relays[MAX_RELAY_ENTRIES];
    uint8_t relayCount = 0;
    BucketState unresolvedRelay;

    struct YoungSighting {
        NodeNum nodeId = 0;
        uint32_t firstSeenMs = 0;
        mutable bool aged = false;
    };
    YoungSighting young[MAX_YOUNG_ENTRIES];
    uint8_t youngCount = 0;
    NodeNum alumni[MAX_ALUMNI_ENTRIES] = {};
    uint8_t alumniCount = 0;
    BucketState youngBucket;
    DestEntry dests[MAX_DEST_ENTRIES];
    uint8_t destCount = 0;
    uint32_t bootMs = 0;
    bool bootKnown = false;
    mutable bool bootWarmed = false;
    bool cfgAnnounceBroadcast = false;
    uint32_t lastAnnounceMs = 0;
    bool announceEver = false;
    NodeNum pendingAnnounceIds[ANNOUNCE_MAX_IDS] = {};
    uint8_t pendingAnnounceCount = 0;
    bool pendingAnnounce = false;
    bool lastDestDrop = false;

    static uint32_t originatorClear(uint32_t trip);
    bool bucketLimited(const BucketState &b, uint32_t clear, uint32_t now) const;
    static NodeNum resolveRelay(const meshtastic_MeshPacket *p);
    static Bucket classifyBucket(meshtastic_PortNum portnum);
    static bool isRebroadcastCandidate(const meshtastic_MeshPacket *p);
    static uint32_t packetAirtimeMs(const meshtastic_MeshPacket *p);
    void relayBudgets(uint32_t airMs, uint32_t &tripMs, uint32_t &clearMs) const;

    bool checkAndUpdateBucket(BucketState &b, uint32_t trip, uint32_t clear, uint32_t charge, uint32_t nowMs, const char *label,
                              NodeNum nodeId);

    OriginatorEntry *findOriginator(NodeNum nodeId);
    const OriginatorEntry *findOriginator(NodeNum nodeId) const;
    OriginatorEntry *getOrCreateOriginator(NodeNum nodeId, uint32_t nowMs);
    int findOriginatorEvictionCandidate() const;

    RelayEntry *findRelay(NodeNum nodeId);
    RelayEntry *getOrCreateRelay(NodeNum nodeId, uint32_t nowMs);
    int findRelayEvictionCandidate() const;

    DestEntry *findDest(NodeNum nodeId);
    const DestEntry *findDest(NodeNum nodeId) const;
    DestEntry *getOrCreateDest(NodeNum nodeId, uint32_t nowMs);
    int findDestEvictionCandidate() const;
    static bool destWindowWillRoll(const BucketState &b, uint32_t nowMs, uint32_t windowMs);
    static void rollDestSenders(DestEntry &entry, uint32_t nowMs, uint32_t windowMs);
    static void noteDestSender(DestEntry &entry, NodeNum from);
    static bool isDestVolumePort(meshtastic_PortNum portnum);
    bool chargesDestVolume(const meshtastic_MeshPacket *p) const;
    bool isLimitedDest(NodeNum nodeId) const;
    bool chargeDestVolume(const meshtastic_MeshPacket *p);

    static bool nodeInGraph(NodeNum nodeId);
    static uint8_t graphHopsAway(NodeNum nodeId);

    uint32_t nowMs() const;
    void noteBoot(uint32_t now);
    bool warmedUp(uint32_t now) const;
    int findYoung(NodeNum nodeId) const;
    bool inAlumni(NodeNum nodeId) const;
    bool addAlumni(NodeNum nodeId);
    void removeYoungAt(uint8_t idx);
    static bool youngAgeActive(const YoungSighting &sighting, uint32_t now);
    uint8_t activeYoungCount(uint32_t now) const;
    int findAgedYoung(uint32_t now) const;
    void expireIfOld(NodeNum nodeId, uint32_t now);
    void insertYoung(NodeNum nodeId, uint32_t now);
    void noteOriginator(NodeNum nodeId, uint32_t now);
    bool isYoung(NodeNum nodeId, uint32_t now) const;
    bool relayAnyLimited() const;
    void maybeAnnounce(uint32_t now, float chutil);
    bool isLimitedOriginator(NodeNum nodeId) const;
    bool isOnDefaultChannel(const meshtastic_MeshPacket *p) const;
    bool shouldDropToLimitedDest(const meshtastic_MeshPacket *p) const;
};

#if !MESHTASTIC_EXCLUDE_SIGNALROUTING
extern NodeRateLimiter *nodeRateLimiter;
#endif
