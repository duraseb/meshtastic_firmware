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
 * a quiet window with count==0).
 */
class NodeRateLimiter
{
  public:
    NodeRateLimiter();

    /**
     * Returns true if the packet should be dropped (rate limited).
     * Packets addressed to us are never limited. Updates internal counters.
     */
    bool shouldDrop(const meshtastic_MeshPacket *p);

    /**
     * True if this packet's originator/RELAY buckets are already limited.
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

    bool debugTracksOriginator(NodeNum nodeId) const;
    bool debugTracksRelay(NodeNum nodeId) const;
    bool debugRelayLimited(NodeNum nodeId) const;
    bool debugUnresolvedRelayLimited() const;
    void debugRelayBudgets(uint32_t airMs, uint32_t &tripMs, uint32_t &clearMs) const;
    uint8_t debugOriginatorCount() const { return originatorCount; }
    uint8_t debugRelayCount() const { return relayCount; }
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
    static bool shouldChargeUnresolvedRelay(bool srPresent, bool graphEstablished)
    {
        return !srPresent || graphEstablished;
    }


  private:
    static constexpr uint8_t MAX_ORIGINATOR_ENTRIES = 16;
    static constexpr uint8_t MAX_RELAY_ENTRIES = 8;

    static constexpr uint32_t DEFAULT_WINDOW_MS = 90u * 1000u;
    static constexpr uint8_t DEFAULT_TEXT_TRIP = 30;
    static constexpr uint8_t DEFAULT_ROUTING_TRIP = 10;
    static constexpr uint8_t DEFAULT_OTHER_TRIP = 4;
    static constexpr uint8_t DEFAULT_UNKNOWN_TRIP = 12;
    static constexpr uint8_t DEFAULT_CLEAR = 0; // originator: quiet window required

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
};

#if !MESHTASTIC_EXCLUDE_SIGNALROUTING
extern NodeRateLimiter *nodeRateLimiter;
#endif
