#pragma once

#include "MeshTypes.h"
#include "mesh/generated/meshtastic/mesh.pb.h"

/**
 * Per-node rate limiter for inbound packets.
 *
 * Tracks up to MAX_ENTRIES nodes using a fixed-size array (no heap allocation).
 * Each node has three independent buckets:
 *   - TEXT    : TEXT_MESSAGE_APP and TEXT_MESSAGE_COMPRESSED_APP
 *   - ROUTING : ROUTING_APP and SIGNAL_ROUTING_APP
 *   - OTHER   : everything else
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
     * Acts on packets from any other node regardless of hop distance.
     * Decoded packets are bucketed by portnum; undecoded (wrong key / decode
     * failure) packets fall into the OTHER bucket — they still consume airtime.
     * DECODE_FATAL packets (already cancelled by the caller) should not be passed.
     * Calling this method also updates the internal counters.
     */
    bool shouldDrop(const meshtastic_MeshPacket *p);

  private:
    static constexpr uint8_t MAX_ENTRIES = 16;

    static constexpr uint32_t WINDOW_MS = 90u * 1000u;

    static constexpr uint8_t TEXT_THRESHOLD    = 30; // packets per window before limiting
    static constexpr uint8_t ROUTING_THRESHOLD = 10;
    static constexpr uint8_t OTHER_THRESHOLD   = 7;

    enum class Bucket : uint8_t { TEXT, ROUTING, OTHER };

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
        uint8_t     maxHopSeen  = 0; // for eviction: prefer evicting distant nodes
    };
    // sizeof(RateLimitEntry) = 4 + 3*(4+1+1) + 1 = 23 bytes
    // 23 * 16 = 368 bytes total

    RateLimitEntry entries[MAX_ENTRIES];
    uint8_t        entryCount = 0;

    static Bucket  classifyBucket(meshtastic_PortNum portnum);
    static uint8_t hopsAway(const meshtastic_MeshPacket *p);

    RateLimitEntry *findEntry(NodeNum nodeId);
    RateLimitEntry *getOrCreateEntry(NodeNum nodeId, uint8_t hops);
    int             findEvictionCandidate() const;

    // Returns true (drop) if the bucket is or becomes rate-limited.
    bool checkAndUpdateBucket(BucketState &b, uint8_t threshold, NodeNum nodeId, Bucket bucket, uint32_t nowMs);
};

extern NodeRateLimiter *nodeRateLimiter;
