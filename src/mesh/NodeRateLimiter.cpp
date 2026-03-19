#include "NodeRateLimiter.h"
#include "MeshTypes.h"
#include "configuration.h"
#include <Arduino.h>
#include <cstring>

NodeRateLimiter *nodeRateLimiter = nullptr;

NodeRateLimiter::NodeRateLimiter() : entryCount(0)
{
    memset(entries, 0, sizeof(entries));
    LOG_INFO("[RateLimit] Initialized: slots=%u window=%us thresholds=text:%u routing:%u other:%u bans=%u/%umin",
             MAX_ENTRIES, WINDOW_MS / 1000u,
             TEXT_THRESHOLD, ROUTING_THRESHOLD, OTHER_THRESHOLD,
             BAN_FIRST_MS / 60000u, BAN_REPEAT_MS / 60000u);
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
    // Ties broken by oldest windowStart (least recently active).
    int candidate = 0;
    for (int i = 1; i < entryCount; i++) {
        if (entries[i].maxHopSeen > entries[candidate].maxHopSeen) {
            candidate = i;
        } else if (entries[i].maxHopSeen == entries[candidate].maxHopSeen &&
                   entries[i].windowStart < entries[candidate].windowStart) {
            candidate = i;
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

bool NodeRateLimiter::checkAndUpdateBucket(RateLimitEntry &e, Bucket bucket, uint32_t nowMs)
{
    uint32_t *banExpiry;
    uint8_t  *count;
    uint8_t   threshold;
    uint8_t   bannedBeforeFlag;

    switch (bucket) {
        case Bucket::TEXT:
            banExpiry       = &e.textBanExpiry;
            count           = &e.textCount;
            threshold       = TEXT_THRESHOLD;
            bannedBeforeFlag = BANNED_BEFORE_TEXT;
            break;
        case Bucket::ROUTING:
            banExpiry       = &e.routingBanExpiry;
            count           = &e.routingCount;
            threshold       = ROUTING_THRESHOLD;
            bannedBeforeFlag = BANNED_BEFORE_ROUTING;
            break;
        default: // OTHER
            banExpiry       = &e.otherBanExpiry;
            count           = &e.otherCount;
            threshold       = OTHER_THRESHOLD;
            bannedBeforeFlag = BANNED_BEFORE_OTHER;
            break;
    }

    // Currently banned?
    if (*banExpiry != 0 && nowMs < *banExpiry) {
        const char *bucketName = (bucket == Bucket::TEXT) ? "text" : (bucket == Bucket::ROUTING) ? "routing" : "other";
        uint32_t remainingSecs = (*banExpiry - nowMs) / 1000u;
        LOG_DEBUG("[RateLimit] Node 0x%08x still banned on %s bucket, %us remaining",
                  e.nodeId, bucketName, remainingSecs);
        return true;
    }

    // Ban just expired — clear it
    if (*banExpiry != 0 && nowMs >= *banExpiry) {
        *banExpiry = 0;
    }

    // Window expired — reset all counts and start a new window
    if (nowMs - e.windowStart >= WINDOW_MS) {
        e.windowStart  = nowMs;
        e.textCount    = 0;
        e.routingCount = 0;
        e.otherCount   = 0;
    }

    (*count)++;

    if (*count > threshold) {
        bool isRepeat = (e.bannedBefore & bannedBeforeFlag) != 0;
        uint32_t banDuration = isRepeat ? BAN_REPEAT_MS : BAN_FIRST_MS;
        e.bannedBefore |= bannedBeforeFlag;
        *banExpiry = nowMs + banDuration;
        const char *bucketName = (bucket == Bucket::TEXT) ? "text" : (bucket == Bucket::ROUTING) ? "routing" : "other";
        LOG_WARN("[RateLimit] Node 0x%08x banned on %s bucket for %u min (%s ban), count=%u in window",
                 e.nodeId, bucketName, banDuration / 60000u, isRepeat ? "repeat" : "first", *count);
        return true;
    }

    return false;
}

bool NodeRateLimiter::shouldDrop(const meshtastic_MeshPacket *p)
{
    if (p->from == 0 || isFromUs(p)) {
        return false;
    }

    uint8_t hops = hopsAway(p);

    // Classify bucket: decoded packets use portnum; undecoded fall into OTHER
    Bucket bucket;
    if (p->which_payload_variant == meshtastic_MeshPacket_decoded_tag) {
        bucket = classifyBucket(p->decoded.portnum);
    } else {
        bucket = Bucket::OTHER;
    }

    uint32_t nowMs = millis();
    RateLimitEntry *entry = getOrCreateEntry(p->from, hops);
    return checkAndUpdateBucket(*entry, bucket, nowMs);
}
