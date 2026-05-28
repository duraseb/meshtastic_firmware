#include "NodeRateLimiter.h"
#include "MeshTypes.h"
#include "NodeDB.h"
#include "configuration.h"
#include <Arduino.h>
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

NodeRateLimiter::NodeRateLimiter() : entryCount(0)
{
    memset(entries, 0, sizeof(entries));

    // Load config overrides. Numeric fields: 0 means "use firmware default". Bool fields: if the
    // message exists, the value is used as-is (proto3 default false = disabled).
    if (moduleConfig.has_node_rate_limiter) {
        const auto &cfg = moduleConfig.node_rate_limiter;
        cfgEnabled = cfg.enabled;
        if (cfg.window_secs != 0) {
            cfgWindowMs = cfg.window_secs * 1000u;
        }
        if (cfg.text_threshold != 0) {
            cfgTextThreshold = (uint8_t)std::min<uint32_t>(cfg.text_threshold, 255);
        }
        if (cfg.routing_threshold != 0) {
            cfgRoutingThreshold = (uint8_t)std::min<uint32_t>(cfg.routing_threshold, 255);
        }
        if (cfg.other_threshold != 0) {
            cfgOtherThreshold = (uint8_t)std::min<uint32_t>(cfg.other_threshold, 255);
        }
    }

    LOG_INFO("[RateLimit] Init: enabled=%d slots=%u window=%us thresholds=text:%u routing:%u other:%u",
             cfgEnabled, MAX_ENTRIES, cfgWindowMs / 1000u,
             cfgTextThreshold, cfgRoutingThreshold, cfgOtherThreshold);
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

uint8_t NodeRateLimiter::hopsAway(const meshtastic_MeshPacket *p)
{
    if (p->hop_start == 0 || p->hop_limit >= p->hop_start) {
        return 0;
    }
    return p->hop_start - p->hop_limit;
}

NodeRateLimiter::RateLimitEntry *NodeRateLimiter::findEntry(NodeNum nodeId)
{
    for (uint8_t i = 0; i < entryCount; i++) {
        if (entries[i].nodeId == nodeId) {
            return &entries[i];
        }
    }
    return nullptr;
}

int NodeRateLimiter::findEvictionCandidate() const
{
    // Never evict an entry that is currently rate-limited — doing so would reset its ban and let the
    // node bypass the limit.  Among non-limited entries evict the farthest (highest maxHopSeen),
    // breaking ties by oldest window start.  If every entry is limited, fall back to oldest window
    // start across all entries so we at least evict the stalest one.
    auto isLimited = [](const RateLimitEntry &e) {
        return e.text.limited || e.routing.limited || e.other.limited;
    };
    auto oldestWindowStart = [](const RateLimitEntry &e) {
        return std::min(e.text.windowStart, std::min(e.routing.windowStart, e.other.windowStart));
    };

    // Check if any non-limited entry exists.
    int candidate = -1;
    for (int i = 0; i < entryCount; i++) {
        if (isLimited(entries[i])) {
            continue;
        }
        if (candidate == -1) {
            candidate = i;
            continue;
        }
        if (entries[i].maxHopSeen > entries[candidate].maxHopSeen) {
            candidate = i;
        } else if (entries[i].maxHopSeen == entries[candidate].maxHopSeen &&
                   oldestWindowStart(entries[i]) < oldestWindowStart(entries[candidate])) {
            candidate = i;
        }
    }
    if (candidate != -1) {
        return candidate;
    }

    // All entries are limited — evict the one with the oldest window start.
    candidate = 0;
    for (int i = 1; i < entryCount; i++) {
        if (oldestWindowStart(entries[i]) < oldestWindowStart(entries[candidate])) {
            candidate = i;
        }
    }
    return candidate;
}

NodeRateLimiter::RateLimitEntry *NodeRateLimiter::getOrCreateEntry(NodeNum nodeId, uint8_t hops, uint32_t nowMs)
{
    RateLimitEntry *existing = findEntry(nodeId);
    if (existing) {
        if (hops > existing->maxHopSeen) {
            existing->maxHopSeen = hops;
        }
        return existing;
    }

    RateLimitEntry *slot;
    if (entryCount < MAX_ENTRIES) {
        slot = &entries[entryCount++];
    } else {
        slot = &entries[findEvictionCandidate()];
    }

    memset(slot, 0, sizeof(RateLimitEntry));
    slot->nodeId = nodeId;
    slot->maxHopSeen = hops;
    slot->text.windowStart = nowMs;
    slot->routing.windowStart = nowMs;
    slot->other.windowStart = nowMs;
    return slot;
}

bool NodeRateLimiter::checkAndUpdateBucket(BucketState &b, uint8_t threshold, NodeNum nodeId, Bucket bucket, uint32_t nowMs)
{
    const char *bucketName = (bucket == Bucket::TEXT) ? "text" : (bucket == Bucket::ROUTING) ? "routing" : "other";
    char nodeName[48];
    getNodeDisplayName(nodeId, nodeName, sizeof(nodeName));

    uint32_t windowAge = nowMs - b.windowStart;
    LOG_INFO("[RateLimit] %s %s bucket: count=%u/%u limited=%d windowAge=%ums/%ums",
             nodeName, bucketName, b.count, threshold, (int)b.limited, windowAge, cfgWindowMs);

    if (b.limited) {
        if (windowAge >= cfgWindowMs) {
            // Node went quiet for a full window — lift the limit
            LOG_INFO("[RateLimit] %s %s bucket unlimited after quiet window", nodeName, bucketName);
            b.limited     = false;
            b.count       = 0;
            b.windowStart = nowMs;
            // Fall through to count this packet normally
        } else {
            // Still active — reset window so the node must go quiet for WINDOW_MS
            b.windowStart = nowMs;
            LOG_INFO("[RateLimit] %s %s bucket still limited, window reset", nodeName, bucketName);
            return true;
        }
    }

    // Window expired naturally — start fresh
    if (nowMs - b.windowStart >= cfgWindowMs) {
        b.windowStart = nowMs;
        b.count       = 0;
    }

    b.count++;

    if (b.count >= threshold) {
        b.limited     = true;
        b.windowStart = nowMs;
        LOG_WARN("[RateLimit] %s %s bucket limited at count=%u", nodeName, bucketName, b.count);
        return true;
    }

    return false;
}

bool NodeRateLimiter::shouldDrop(const meshtastic_MeshPacket *p)
{
    if (!cfgEnabled) {
        return false;
    }
    if (p->from == 0 || isFromUs(p)) {
        return false;
    }

    if (nodeDB) {
        const meshtastic_NodeInfoLite *node = nodeDB->getMeshNode(p->from);
        if (node && node->is_favorite) {
            return false;
        }
    }

    uint8_t hops = hopsAway(p);

    Bucket bucket;
    if (p->which_payload_variant == meshtastic_MeshPacket_decoded_tag) {
        bucket = classifyBucket(p->decoded.portnum);
    } else {
        bucket = Bucket::OTHER;
    }

    uint32_t nowMs = millis();
    RateLimitEntry *entry = getOrCreateEntry(p->from, hops, nowMs);

    BucketState *b;
    uint8_t threshold;
    switch (bucket) {
        case Bucket::TEXT:    b = &entry->text;    threshold = cfgTextThreshold;    break;
        case Bucket::ROUTING: b = &entry->routing; threshold = cfgRoutingThreshold; break;
        default:              b = &entry->other;   threshold = cfgOtherThreshold;   break;
    }

    return checkAndUpdateBucket(*b, threshold, entry->nodeId, bucket, nowMs);
}
