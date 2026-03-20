#include "NodeRateLimiter.h"
#include "MeshTypes.h"
#include "NodeDB.h"
#include "configuration.h"
#include <Arduino.h>
#include <cstring>

static void getNodeDisplayName(NodeNum nodeId, char *buf, size_t bufSize)
{
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
}

NodeRateLimiter *nodeRateLimiter = nullptr;

NodeRateLimiter::NodeRateLimiter() : entryCount(0)
{
    memset(entries, 0, sizeof(entries));
    LOG_INFO("[RateLimit] Initialized: slots=%u window=%us thresholds=text:%u routing:%u other:%u",
             MAX_ENTRIES, WINDOW_MS / 1000u,
             TEXT_THRESHOLD, ROUTING_THRESHOLD, OTHER_THRESHOLD);
}

NodeRateLimiter::Bucket NodeRateLimiter::classifyBucket(meshtastic_PortNum portnum)
{
    switch (portnum) {
        case meshtastic_PortNum_TEXT_MESSAGE_APP:
        case meshtastic_PortNum_TEXT_MESSAGE_COMPRESSED_APP:
            return Bucket::TEXT;
        case meshtastic_PortNum_ROUTING_APP:
        case meshtastic_PortNum_SIGNAL_ROUTING_APP:
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
    // Evict the entry with the greatest hop distance.
    // Ties broken by oldest window start across all buckets.
    int candidate = 0;
    for (int i = 1; i < entryCount; i++) {
        if (entries[i].maxHopSeen > entries[candidate].maxHopSeen) {
            candidate = i;
        } else if (entries[i].maxHopSeen == entries[candidate].maxHopSeen) {
            uint32_t iOldest = std::min(entries[i].text.windowStart,
                                std::min(entries[i].routing.windowStart, entries[i].other.windowStart));
            uint32_t cOldest = std::min(entries[candidate].text.windowStart,
                                std::min(entries[candidate].routing.windowStart, entries[candidate].other.windowStart));
            if (iOldest < cOldest) {
                candidate = i;
            }
        }
    }
    return candidate;
}

NodeRateLimiter::RateLimitEntry *NodeRateLimiter::getOrCreateEntry(NodeNum nodeId, uint8_t hops)
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
    slot->nodeId     = nodeId;
    slot->maxHopSeen = hops;
    return slot;
}

bool NodeRateLimiter::checkAndUpdateBucket(BucketState &b, uint8_t threshold, NodeNum nodeId, Bucket bucket, uint32_t nowMs)
{
    const char *bucketName = (bucket == Bucket::TEXT) ? "text" : (bucket == Bucket::ROUTING) ? "routing" : "other";
    char nodeName[48];
    getNodeDisplayName(nodeId, nodeName, sizeof(nodeName));

    if (b.limited) {
        if (nowMs - b.windowStart >= WINDOW_MS) {
            // Node went quiet for a full window — lift the limit
            LOG_INFO("[RateLimit] %s %s bucket unlimited after quiet window", nodeName, bucketName);
            b.limited     = false;
            b.count       = 0;
            b.windowStart = nowMs;
            // Fall through to count this packet normally
        } else {
            // Still active — reset window so the node must go quiet for WINDOW_MS
            b.windowStart = nowMs;
            LOG_DEBUG("[RateLimit] %s %s bucket still limited, window reset", nodeName, bucketName);
            return true;
        }
    }

    // Window expired naturally — start fresh
    if (nowMs - b.windowStart >= WINDOW_MS) {
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

    uint32_t        nowMs = millis();
    RateLimitEntry *entry = getOrCreateEntry(p->from, hops);

    BucketState *b;
    uint8_t      threshold;
    switch (bucket) {
        case Bucket::TEXT:    b = &entry->text;    threshold = TEXT_THRESHOLD;    break;
        case Bucket::ROUTING: b = &entry->routing; threshold = ROUTING_THRESHOLD; break;
        default:              b = &entry->other;   threshold = OTHER_THRESHOLD;   break;
    }

    return checkAndUpdateBucket(*b, threshold, entry->nodeId, bucket, nowMs);
}
