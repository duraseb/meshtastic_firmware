#pragma once

#include "mesh/generated/meshtastic/mesh.pb.h"

/**
 * Channel-utilization-based QoS filter for relay decisions.
 *
 * When the channel is congested, gradually drop lower priority traffic first
 * to preserve bandwidth for user messages and critical control traffic.
 *
 * Priority tiers (lowest dropped first):
 *   LOW      – telemetry, position, nodeinfo, unknown/undecoded  (drop above 25%)
 *   MEDIUM   – text on non-primary channel                       (drop above 30%)
 *   HIGH     – routing, SR routing, traceroute, ACKs             (drop above 38%)
 *   CRITICAL – text on primary channel, admin                    (never dropped)
 */
class ChannelQoS
{
  public:
    /**
     * Returns true if current channel utilization permits relaying this packet.
     * Only call this for packets we are considering relaying (not our own, not to us).
     */
    bool canRelay(const meshtastic_MeshPacket *p);

    enum class Tier : uint8_t { QOS_LOW, QOS_MEDIUM, QOS_HIGH, QOS_CRITICAL };

  private:
    // Channel utilization thresholds (percent) — relay is suppressed when chutil exceeds these
    static constexpr uint8_t CHUTIL_THRESHOLD_LOW = 25;
    static constexpr uint8_t CHUTIL_THRESHOLD_MEDIUM = 30;
    static constexpr uint8_t CHUTIL_THRESHOLD_HIGH = 38;
    // CRITICAL tier is never dropped

    Tier classifyTier(const meshtastic_MeshPacket *p);
};

#if !MESHTASTIC_EXCLUDE_CHANNEL_QOS
extern ChannelQoS *channelQoS;
#endif
