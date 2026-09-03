#pragma once

#include "MeshTypes.h"
#include "mesh/generated/meshtastic/mesh.pb.h"

/**
 * Per-node rate limiter for inbound packets.
 *
 * Tracks up to MAX_ENTRIES nodes using a fixed-size array (no heap allocation).
 * Each node has four independent buckets:
 *   - TEXT    : TEXT_MESSAGE_APP and TEXT_MESSAGE_COMPRESSED_APP
 *   - ROUTING : ROUTING_APP, SIGNAL_ROUTING_APP and TRACEROUTE_APP
 *   - OTHER   : every other decoded portnum
 *   - UNKNOWN : undecodable packets (no key for the channel, PKI traffic for other nodes)
 *
 * When a bucket receives >= threshold packets within WINDOW_MS, the node is
 * rate-limited on that bucket. Every subsequent packet resets the window timer,
 * so the node stays blocked until it goes quiet for a full WINDOW_MS. Buckets
 * are independent — being limited on one does not affect the others.
 *
 * When all slots are full a new node evicts the slot with the greatest hop
 * distance; ties are broken by oldest window start.
 */
class NodeRateLimiter
{
  public:
    NodeRateLimiter();

    /**
     * Returns true if the packet should be dropped (rate limited).
     * Acts on packets from any other node regardless of hop distance, except packets
     * addressed to us: those are never relayed and must always reach the ACK path and
     * the phone (admin replies, DMs).
     * Decoded packets are bucketed by portnum; undecoded (wrong key / decode
     * failure) packets count against the UNKNOWN bucket, whose threshold sits
     * between OTHER and TEXT: a relay without the key cannot tell chat from
     * telemetry, but must not let encrypted traffic for others run at chat rates.
     * DECODE_FATAL packets (already cancelled by the caller) should not be passed.
     * Calling this method also updates the internal counters.
     */
    bool shouldDrop(const meshtastic_MeshPacket *p);

  private:
    static constexpr uint8_t MAX_ENTRIES = 16;

    // Runtime-configurable values loaded from moduleConfig.node_rate_limiter at construction.
    // All default to the compile-time constants; overridden when has_node_rate_limiter is set.
    bool     cfgEnabled          = true;
    uint32_t cfgWindowMs         = 90u * 1000u;
    uint8_t  cfgTextThreshold    = 30; // packets per window before limiting
    uint8_t  cfgRoutingThreshold = 10;
    uint8_t  cfgOtherThreshold   = 4;
    // Undecodable packets; no proto override. Sized so a relayed remote-admin Channels screen
    // (channel 0, LoRa config, then channels 1..7 one at a time: nine requests) loads in one window.
    uint8_t  cfgUnknownThreshold = 12;

    enum class Bucket : uint8_t { TEXT, ROUTING, OTHER, UNKNOWN };

    struct BucketState {
        uint32_t windowStart = 0; // ms timestamp when current window started
        uint8_t  count       = 0; // packets received in current window
        bool     limited     = false; // currently rate-limited
    };

    struct RateLimitEntry {
        NodeNum     nodeId      = 0;
        BucketState text;
        BucketState routing;
        BucketState other;
        BucketState unknown;
        uint8_t     maxHopSeen  = 0; // for eviction: prefer evicting distant nodes
    };
    // sizeof(RateLimitEntry) = 4 + 4*(4+1+1+2 pad) + 1 (+3 pad) = 40 bytes
    // 40 * 16 = 640 bytes total

    RateLimitEntry entries[MAX_ENTRIES];
    uint8_t        entryCount = 0;

    static Bucket  classifyBucket(meshtastic_PortNum portnum);
    static uint8_t hopsAway(const meshtastic_MeshPacket *p);

    RateLimitEntry *findEntry(NodeNum nodeId);
    RateLimitEntry *getOrCreateEntry(NodeNum nodeId, uint8_t hops, uint32_t nowMs);
    int             findEvictionCandidate() const;

    // Returns true (drop) if the bucket is or becomes rate-limited.
    bool checkAndUpdateBucket(BucketState &b, uint8_t threshold, NodeNum nodeId, Bucket bucket, uint32_t nowMs);
};

#if !MESHTASTIC_EXCLUDE_NODE_RATE_LIMITER
extern NodeRateLimiter *nodeRateLimiter;
#endif
