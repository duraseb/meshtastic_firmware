#include "configuration.h"

#if !MESHTASTIC_EXCLUDE_CHANNEL_QOS

#include "ChannelQoS.h"
#include "airtime.h"
#include "mesh/generated/meshtastic/portnums.pb.h"

ChannelQoS *channelQoS = nullptr;

static const char *getTierName(ChannelQoS::Tier t)
{
    switch (t) {
        case ChannelQoS::Tier::QOS_LOW:      return "LOW";
        case ChannelQoS::Tier::QOS_MEDIUM:   return "MEDIUM";
        case ChannelQoS::Tier::QOS_HIGH:     return "HIGH";
        case ChannelQoS::Tier::QOS_CRITICAL: return "CRITICAL";
        default:                             return "?";
    }
}

bool ChannelQoS::canRelay(const meshtastic_MeshPacket *p)
{
    Tier tier = classifyTier(p);

    // CRITICAL traffic is never dropped
    if (tier == Tier::QOS_CRITICAL) {
        return true;
    }

    float chutil = airTime->channelUtilizationPercent();

    uint8_t threshold = 0;
    switch (tier) {
        case Tier::QOS_LOW:    threshold = CHUTIL_THRESHOLD_LOW;    break;
        case Tier::QOS_MEDIUM: threshold = CHUTIL_THRESHOLD_MEDIUM; break;
        case Tier::QOS_HIGH:   threshold = CHUTIL_THRESHOLD_HIGH;   break;
        default:               return true; // unreachable, but safe
    }

    if (chutil >= threshold) {
        LOG_INFO("[QoS] Drop relay 0x%08x from 0x%08x: tier %s, chutil %.1f%% >= %d%%",
                 p->id, p->from, getTierName(tier), chutil, threshold);
        return false;
    }

    return true;
}

auto ChannelQoS::classifyTier(const meshtastic_MeshPacket *p) -> Tier
{
    // Undecoded packets (wrong key, decode failure) are low priority
    if (!p->decoded.portnum) {
        return Tier::QOS_LOW;
    }

    switch (p->decoded.portnum) {
        // CRITICAL: text messages on primary channel
        case meshtastic_PortNum_TEXT_MESSAGE_APP:
        case meshtastic_PortNum_TEXT_MESSAGE_COMPRESSED_APP:
            return (p->channel == 0) ? Tier::QOS_CRITICAL : Tier::QOS_MEDIUM;

        // CRITICAL: admin / remote management
        case meshtastic_PortNum_ADMIN_APP:
            return Tier::QOS_CRITICAL;

        // HIGH: routing and control plane
        case meshtastic_PortNum_ROUTING_APP:
        case meshtastic_PortNum_SIGNAL_ROUTING_APP:
        case meshtastic_PortNum_TRACEROUTE_APP:
            return Tier::QOS_HIGH;

        // LOW: telemetry, position, nodeinfo, and everything else
        default:
            return Tier::QOS_LOW;
    }
}

#endif // !MESHTASTIC_EXCLUDE_CHANNEL_QOS
