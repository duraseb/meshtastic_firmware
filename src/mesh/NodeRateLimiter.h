#pragma once

#include "MeshTypes.h"
#include "mesh/generated/meshtastic/mesh.pb.h"

/**
 * Per-node inbound rate limiter (SignalRouting only).
 *
 * Originator path: up to MAX_ORIGINATOR_ENTRIES sources, each with TEXT / ROUTING /
 * OTHER / UNKNOWN buckets (packet counts). Favorites bypass originator charging only.
 *
 * RELAY path: rebroadcast candidates charge the last hop — resolved NodeID slots
 * (MAX_RELAY_ENTRIES) plus one shared bucket for all unresolved relay_node bytes.
 * RELAY uses an airtime budget (ms) with hybrid AirUtil tightening.
 *
 * Uniform hysteresis: trip_threshold / clear_threshold / fixed window; while limited,
 * drop matching traffic; lift at window end when count is below clear (clear==0 means
 * a quiet window with count==0). RELAY and YOUNG use a clear threshold below trip.
 *
 * Dest drop: while any originator bucket for a node is limited, unicast frames
 * addressed to that node on the default public channel are logged and dropped without
 * charging the sender. WantResponse replies from other nodes would otherwise keep
 * flooding after the originator itself is already silenced. Unicast on a non-default
 * channel (private PSK / PKI) is not dest-dropped. RELAY and YOUNG limits do not
 * dest-drop; only an originator ban does.
 *
 * Young path: one shared packet-count bucket for originators first heard less than
 * 30 minutes ago, so a flood of minted identities buys one slot, not one each.
 */
class NodeRateLimiter
{
  public:
    NodeRateLimiter();

    /**
     * Returns true if the packet should be dropped (rate limited).
     * Packets addressed to us are never limited. Updates internal counters.
     * Default-channel unicast to a limited originator is dest-dropped without charging
     * the sender.
     */
    bool shouldDrop(const meshtastic_MeshPacket *p);

    /**
     * True if the last shouldDrop() returned true because the destination is a
     * limited originator (default-channel unicast). False after any other outcome.
     */
    bool lastDropWasDest() const { return lastDestDrop; }

    /**
     * True if this packet's originator/RELAY buckets are already limited, or if it is
     * default-channel unicast to a limited originator.
     * Does not update counters — for dupe/upgrade rebroadcast gates so we do not
     * amplify after a prior shouldDrop, and do not double-charge airtime.
     */
    bool wouldDrop(const meshtastic_MeshPacket *p) const;

#if defined(UNIT_TEST)
    /// pio-test hooks only (not in device or meshtasticd production builds).
    static NodeNum (*testResolveRelayHook)(uint8_t relayByte, int16_t rssi, float snr);
    static bool (*testNodeInGraphHook)(NodeNum nodeId);
    static uint8_t (*testGraphHopsHook)(NodeNum nodeId);
    static float testChutilOverride; // < 0 => use airTime / default
    static int8_t testOnDefaultChannelOverride; // < 0 => treat as default channel

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
    bool debugIsYoung(NodeNum nodeId) const;
    bool debugYoungLimited() const { return youngBucket.limited; }
    uint32_t debugYoungCharge() const { return youngBucket.count; }
    void debugSetBootMs(uint32_t bootMs);
    void debugSeedYoung(NodeNum nodeId, uint32_t firstSeenMs);
    bool debugTakeAnnounce(NodeNum *ids, uint8_t &count);
    void debugSetAnnounceBroadcast(bool enabled) { cfgAnnounceBroadcast = enabled; }
    static int64_t testNowOverride; // < 0 => millis()
#endif

    /**
     * Whether an unresolvable relay byte should be charged to the shared bucket.
     *
     * Only when resolution could have worked. With SignalRouting present but no direct neighbours
     * in the graph yet — the first minutes after a reboot — every byte is unresolvable because
     * there is nothing to resolve against, and charging then trips the shared bucket on ordinary
     * traffic. With no SignalRouting at all there is no resolution mechanism, so the shared bucket
     * is the only defence and must still be charged.
     */
    // Unchanged, and measured to be right: counted the way the limiter actually sees traffic —
    // at one node, with duplicate copies of a frame already filtered out — the busiest originator
    // in any 90 s window over 20 h reached TEXT 3, ROUTING 7, OTHER 5, UNKNOWN 18. Only a couple
    // of originators ever brush a threshold, which matches the hub's operational experience that
    // the limiter seldom triggers.
    static constexpr uint8_t DEFAULT_TEXT_TRIP = 30;
    static constexpr uint8_t DEFAULT_ROUTING_TRIP = 10;
    static constexpr uint8_t DEFAULT_OTHER_TRIP = 4;
    static constexpr uint8_t DEFAULT_UNKNOWN_TRIP = 12;
    // Hysteresis: a limited originator clears once a whole window comes in under half the trip
    // level, i.e. once it has demonstrably slowed down. Zero meant "sticky until a fully silent
    // window", and because every packet arriving while limited reset that window, an originator
    // that kept talking could never clear. Brushing a threshold once is common and harmless;
    // being limited forever after is what stopped a gateway relaying anything for forty minutes.
    //
    // Half, not the RELAY bucket's quarter: these thresholds are small. A quarter of OTHER's 4 is
    // 1, which would demand a completely silent window and reinstate the behaviour being fixed.
    static constexpr uint8_t DEFAULT_CLEAR_RATIO_NUM = 1;
    static constexpr uint8_t DEFAULT_CLEAR_RATIO_DEN = 2;

    static bool shouldChargeUnresolvedRelay(bool srPresent, bool graphEstablished)
    {
        return !srPresent || graphEstablished;
    }

    /// A node whose traffic the young bucket is currently dropping is not a coverage target.
    bool isDroppedCoverageTarget(NodeNum nodeId) const;

    /// Diagnostic IDs from the last young-bucket trip that was allowed to announce.
    bool takeYoungAnnounce(NodeNum *ids, uint8_t &count);

    /// Mesh broadcast of the diagnostic. Stays false until a config bit exists.
    bool announceBroadcastEnabled() const { return cfgAnnounceBroadcast; }

    static constexpr uint8_t MAX_YOUNG_ENTRIES = 32;
    static constexpr uint32_t YOUNG_AGE_MS = 30u * 60u * 1000u;
    static constexpr uint32_t WARMUP_MS = 30u * 60u * 1000u;
    // 48 is 2× the measured 24-packet / 90 s legitimate peak of young traffic; 12 is
    // the RELAY-style quarter-of-trip clear. Thirty minutes is a commitment, not a knob.
    static constexpr uint8_t YOUNG_TRIP = 48;
    static constexpr uint8_t YOUNG_CLEAR = 12;
    static constexpr uint32_t ANNOUNCE_REFRACTORY_MS = 30u * 60u * 1000u;
    static constexpr float ANNOUNCE_CHUTIL_HIGH = 25.0f;
    static constexpr uint8_t ANNOUNCE_MAX_IDS = 4;


  private:
    static constexpr uint8_t MAX_ORIGINATOR_ENTRIES = 16;
    static constexpr uint8_t MAX_RELAY_ENTRIES = 8;

    static constexpr uint32_t DEFAULT_WINDOW_MS = 90u * 1000u;

    // RELAY packet-equivalent defaults at reference calibration (LONG_FAST-ish)
    static constexpr uint8_t RELAY_TRIP_PACKETS = 60;
    static constexpr uint8_t RELAY_CLEAR_PACKETS = 15;
    static constexpr uint32_t RELAY_REF_AIRTIME_MS = 100; // ms per "packet" for budget scale
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
        uint32_t count = 0; // packets (originator) or airtime ms (RELAY)
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
        NodeNum nodeId = 0; // resolved last hop; never 0 in the array slots
        BucketState relay;
    };

    OriginatorEntry originators[MAX_ORIGINATOR_ENTRIES];
    uint8_t originatorCount = 0;

    RelayEntry relays[MAX_RELAY_ENTRIES];
    uint8_t relayCount = 0;
    BucketState unresolvedRelay; // shared "+1" for forgeable/unresolved relay bytes

    struct YoungSighting {
        NodeNum nodeId = 0;
        uint32_t firstSeenMs = 0;
    };
    YoungSighting young[MAX_YOUNG_ENTRIES];
    uint8_t youngCount = 0;
    NodeNum alumni[MAX_YOUNG_ENTRIES] = {};
    uint8_t alumniCount = 0;
    BucketState youngBucket;
    uint32_t bootMs = 0;
    bool bootKnown = false;
    bool cfgAnnounceBroadcast = false;
    uint32_t lastAnnounceMs = 0;
    bool announceEver = false;
    NodeNum pendingAnnounceIds[ANNOUNCE_MAX_IDS] = {};
    uint8_t pendingAnnounceCount = 0;
    bool pendingAnnounce = false;
    bool lastDestDrop = false;

    static Bucket classifyBucket(meshtastic_PortNum portnum);
    static bool isRebroadcastCandidate(const meshtastic_MeshPacket *p);
    static uint32_t packetAirtimeMs(const meshtastic_MeshPacket *p);
    void relayBudgets(uint32_t airMs, uint32_t &tripMs, uint32_t &clearMs) const;

    // Returns true (drop) if limited after update. charge is 1 for packets or airtime ms.
    bool checkAndUpdateBucket(BucketState &b, uint32_t trip, uint32_t clear, uint32_t charge, uint32_t nowMs,
                              const char *label, NodeNum nodeId);

    OriginatorEntry *findOriginator(NodeNum nodeId);
    OriginatorEntry *getOrCreateOriginator(NodeNum nodeId, uint32_t nowMs);
    int findOriginatorEvictionCandidate() const;

    RelayEntry *findRelay(NodeNum nodeId);
    RelayEntry *getOrCreateRelay(NodeNum nodeId, uint32_t nowMs);
    int findRelayEvictionCandidate() const;

    // Eviction helpers (observed graph proximity; never frame hop fields)
    static bool nodeInGraph(NodeNum nodeId);
    static uint8_t graphHopsAway(NodeNum nodeId); // 0 = unknown / not in graph

    uint32_t nowMs() const;
    void noteBoot(uint32_t now);
    bool warmedUp(uint32_t now) const;
    int findYoung(NodeNum nodeId) const;
    bool inAlumni(NodeNum nodeId) const;
    void addAlumni(NodeNum nodeId);
    void removeYoungAt(uint8_t idx);
    void expireIfOld(NodeNum nodeId, uint32_t now);
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
