#pragma once

#include "MeshTypes.h"
#include "mesh/generated/meshtastic/mesh.pb.h"

/**
 * Per-node rate limiter for inbound packets.
 *
 * Tracks up to MAX_ENTRIES nodes using a fixed-size array (no heap allocation).
 * Each node has three independent token buckets:
 *   - TEXT    : TEXT_MESSAGE_APP and TEXT_MESSAGE_COMPRESSED_APP
 *   - ROUTING : ROUTING_APP and SIGNAL_ROUTING_APP
 *   - OTHER   : everything else
 *
 * When a bucket exceeds its threshold within WINDOW_MS, the node is banned on
 * that bucket for BAN_FIRST_MS (5 min). A subsequent ban doubles to BAN_REPEAT_MS
 * (10 min). Bans are independent per bucket.
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

    static constexpr uint32_t WINDOW_MS = 60u * 1000u;       // 1 minute counting window

    static constexpr uint8_t TEXT_THRESHOLD    = 20;  // texts per window before ban
    static constexpr uint8_t ROUTING_THRESHOLD = 5;   // routing pkts per window before ban
    static constexpr uint8_t OTHER_THRESHOLD   = 8;   // other pkts per window before ban

    static constexpr uint32_t BAN_FIRST_MS  = 5u * 60u * 1000u;  // 5 minutes
    static constexpr uint32_t BAN_REPEAT_MS = 10u * 60u * 1000u; // 10 minutes (second+ ban)

    // Bit flags for bannedBefore field — one bit per bucket
    static constexpr uint8_t BANNED_BEFORE_TEXT    = (1u << 0u);
    static constexpr uint8_t BANNED_BEFORE_ROUTING = (1u << 1u);
    static constexpr uint8_t BANNED_BEFORE_OTHER   = (1u << 2u);

    enum class Bucket : uint8_t { TEXT, ROUTING, OTHER };

    struct RateLimitEntry {
        NodeNum  nodeId         = 0;
        uint32_t windowStart    = 0; // ms timestamp when current window started
        uint8_t  textCount      = 0;
        uint8_t  routingCount   = 0;
        uint8_t  otherCount     = 0;
        uint8_t  bannedBefore   = 0; // bit flags: which buckets have ever been banned
        uint8_t  maxHopSeen     = 0; // largest hop distance observed (eviction priority)
        uint8_t  _pad[3]        = {}; // explicit padding to avoid surprises
        uint32_t textBanExpiry    = 0; // ms expiry; 0 = not banned
        uint32_t routingBanExpiry = 0;
        uint32_t otherBanExpiry   = 0;
    };
    // sizeof(RateLimitEntry) = 4+4+1+1+1+1+1+3+4+4+4 = 28 bytes
    // 28 * 16 = 448 bytes total

    RateLimitEntry entries[MAX_ENTRIES];
    uint8_t        entryCount = 0;

    static Bucket  classifyBucket(meshtastic_PortNum portnum);
    static uint8_t hopsAway(const meshtastic_MeshPacket *p);

    RateLimitEntry *findEntry(NodeNum nodeId);
    RateLimitEntry *getOrCreateEntry(NodeNum nodeId, uint8_t hops);
    int             findEvictionCandidate() const;

    // Returns true (drop) if bucket is banned or becomes banned after this packet.
    bool checkAndUpdateBucket(RateLimitEntry &e, Bucket bucket, uint32_t nowMs);
};

extern NodeRateLimiter *nodeRateLimiter;
