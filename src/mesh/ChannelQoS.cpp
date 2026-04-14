#include "configuration.h"

#if !MESHTASTIC_EXCLUDE_CHANNEL_QOS

#include "ChannelQoS.h"
#include "NodeDB.h"
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

static const char *getPortnumShortName(meshtastic_PortNum portnum)
{
    switch (portnum) {
        case meshtastic_PortNum_TEXT_MESSAGE_APP:            return "text";
        case meshtastic_PortNum_TEXT_MESSAGE_COMPRESSED_APP: return "text_compressed";
        case meshtastic_PortNum_POSITION_APP:                return "position";
        case meshtastic_PortNum_NODEINFO_APP:                return "nodeinfo";
        case meshtastic_PortNum_ROUTING_APP:                 return "routing";
        case meshtastic_PortNum_ADMIN_APP:                   return "admin";
        case meshtastic_PortNum_TELEMETRY_APP:               return "telemetry";
        case meshtastic_PortNum_TRACEROUTE_APP:              return "traceroute";
        case meshtastic_PortNum_NEIGHBORINFO_APP:            return "neighborinfo";
        case meshtastic_PortNum_SIGNAL_ROUTING_APP:          return "signal_routing";
        case meshtastic_PortNum_MAP_REPORT_APP:              return "map_report";
        default:                                             return nullptr;
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
        const char *longName = "";
        const char *shortName = "?";
        if (nodeDB) {
            const meshtastic_NodeInfoLite *node = nodeDB->getMeshNode(p->from);
            if (node && node->has_user) {
                if (node->user.long_name[0]) {
                    longName = node->user.long_name;
                }
                if (node->user.short_name[0]) {
                    shortName = node->user.short_name;
                }
            }
        }
        const char *portnumName = getPortnumShortName(static_cast<meshtastic_PortNum>(p->decoded.portnum));
        if (portnumName) {
            LOG_INFO("[QoS] Drop relay 0x%08x from %s (%s, %08x): %s, tier %s, chutil %.1f%% >= %d%%",
                     p->id, longName, shortName, p->from, portnumName, getTierName(tier), chutil, threshold);
        } else {
            LOG_INFO("[QoS] Drop relay 0x%08x from %s (%s, %08x): port %d, tier %s, chutil %.1f%% >= %d%%",
                     p->id, longName, shortName, p->from, p->decoded.portnum, getTierName(tier), chutil, threshold);
        }
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
