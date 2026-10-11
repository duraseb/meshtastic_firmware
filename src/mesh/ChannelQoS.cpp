#include "configuration.h"

#if !MESHTASTIC_EXCLUDE_SIGNALROUTING

#include "ChannelQoS.h"
#include "Channels.h"
#include "NodeDB.h"
#include "airtime.h"
#include "mesh/generated/meshtastic/portnums.pb.h"

ChannelQoS *channelQoS = nullptr;

#if defined(UNIT_TEST)
float ChannelQoS::testChutilOverride = -1.0f;
#endif

#ifndef DEBUG_MUTE
static const char *getTierName(ChannelQoS::Tier t)
{
    switch (t) {
    case ChannelQoS::Tier::QOS_LOW:
        return "LOW";
    case ChannelQoS::Tier::QOS_MEDIUM:
        return "MEDIUM";
    case ChannelQoS::Tier::QOS_HIGH:
        return "HIGH";
    case ChannelQoS::Tier::QOS_CRITICAL:
        return "CRITICAL";
    default:
        return "?";
    }
}

static const char *getPortnumShortName(meshtastic_PortNum portnum)
{
    switch (portnum) {
    case meshtastic_PortNum_TEXT_MESSAGE_APP:
        return "text";
    case meshtastic_PortNum_TEXT_MESSAGE_COMPRESSED_APP:
        return "text_compressed";
    case meshtastic_PortNum_POSITION_APP:
        return "position";
    case meshtastic_PortNum_NODEINFO_APP:
        return "nodeinfo";
    case meshtastic_PortNum_ROUTING_APP:
        return "routing";
    case meshtastic_PortNum_ADMIN_APP:
        return "admin";
    case meshtastic_PortNum_TELEMETRY_APP:
        return "telemetry";
    case meshtastic_PortNum_TRACEROUTE_APP:
        return "traceroute";
    case meshtastic_PortNum_NEIGHBORINFO_APP:
        return "neighborinfo";
    case meshtastic_PortNum_SIGNAL_ROUTING_APP:
        return "signal_routing";
    case meshtastic_PortNum_MAP_REPORT_APP:
        return "map_report";
    default:
        return nullptr;
    }
}
#endif

bool ChannelQoS::canRelay(const meshtastic_MeshPacket *p) const
{
    if (!p) {
        return false;
    }
    Tier tier = classifyTier(p);

    if (tier == Tier::QOS_CRITICAL) {
        return true;
    }

    float chutil = airTime ? airTime->channelUtilizationPercent() : 0.0f;
#if defined(UNIT_TEST)
    if (testChutilOverride >= 0.0f) {
        chutil = testChutilOverride;
    }
#endif

    uint8_t threshold = 0;
    switch (tier) {
    case Tier::QOS_LOW:
        threshold = CHUTIL_THRESHOLD_LOW;
        break;
    case Tier::QOS_MEDIUM:
        threshold = CHUTIL_THRESHOLD_MEDIUM;
        break;
    case Tier::QOS_HIGH:
        threshold = CHUTIL_THRESHOLD_HIGH;
        break;
    default:
        return true;
    }

    if (chutil >= threshold) {
#ifndef DEBUG_MUTE
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
        const auto portnum =
            p->which_payload_variant == meshtastic_MeshPacket_decoded_tag ? p->decoded.portnum : meshtastic_PortNum_UNKNOWN_APP;
        const char *portnumName = getPortnumShortName(portnum);
        if (portnumName) {
            LOG_INFO("[QoS] Drop relay 0x%08x from %s (%s, %08x): %s, tier %s, chutil %.1f%% >= %d%%", p->id, longName, shortName,
                     p->from, portnumName, getTierName(tier), chutil, threshold);
        } else {
            LOG_INFO("[QoS] Drop relay 0x%08x from %s (%s, %08x): port %d, tier %s, chutil %.1f%% >= %d%%", p->id, longName,
                     shortName, p->from, portnum, getTierName(tier), chutil, threshold);
        }
#endif
        return false;
    }

    return true;
}

auto ChannelQoS::classifyTier(const meshtastic_MeshPacket *p) -> Tier
{
    if (p->which_payload_variant != meshtastic_MeshPacket_decoded_tag) {
        return Tier::QOS_LOW;
    }

    switch (p->decoded.portnum) {
    case meshtastic_PortNum_TEXT_MESSAGE_APP:
    case meshtastic_PortNum_TEXT_MESSAGE_COMPRESSED_APP:
        return (p->channel == channels.getPrimaryIndex()) ? Tier::QOS_CRITICAL : Tier::QOS_MEDIUM;

    case meshtastic_PortNum_ADMIN_APP:
        return Tier::QOS_CRITICAL;

    case meshtastic_PortNum_ROUTING_APP:
    case meshtastic_PortNum_SIGNAL_ROUTING_APP:
    case meshtastic_PortNum_TRACEROUTE_APP:
        return Tier::QOS_HIGH;

    default:
        return Tier::QOS_LOW;
    }
}

#endif
