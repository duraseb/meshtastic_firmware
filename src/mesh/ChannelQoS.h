#pragma once

#include "mesh/generated/meshtastic/mesh.pb.h"

// Congestion shedding preserves airtime for user messages and management traffic.
class ChannelQoS
{
  public:
    bool canRelay(const meshtastic_MeshPacket *p) const;

#if defined(UNIT_TEST)
    static float testChutilOverride;
#endif

    enum class Tier : uint8_t { QOS_LOW, QOS_MEDIUM, QOS_HIGH, QOS_CRITICAL };

  private:
    static constexpr uint8_t CHUTIL_THRESHOLD_LOW = 25;
    static constexpr uint8_t CHUTIL_THRESHOLD_MEDIUM = 30;
    static constexpr uint8_t CHUTIL_THRESHOLD_HIGH = 38;

    static Tier classifyTier(const meshtastic_MeshPacket *p);
};

#if !MESHTASTIC_EXCLUDE_SIGNALROUTING
extern ChannelQoS *channelQoS;
#endif
