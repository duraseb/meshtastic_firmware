#pragma once

#include "mesh/MeshTypes.h"
#include <stdint.h>

/// Consecutive misses before a hop is treated as suspect.
static constexpr uint8_t HOP_HEALTH_SUSPECT_MISSES = 2;
/// An entry, suspect or not, expires this many milliseconds after its last miss.
static constexpr uint32_t HOP_HEALTH_SUSPECT_TTL_MS = 10 * 60 * 1000;
static constexpr uint8_t HOP_HEALTH_MAX_ENTRIES = 8;

/// Small RAM table keyed by (destination, next hop). Nothing is persisted.
class HopHealth
{
  public:
    static constexpr uint8_t SUSPECT_MISSES = HOP_HEALTH_SUSPECT_MISSES;
    static constexpr uint32_t SUSPECT_TTL_MS = HOP_HEALTH_SUSPECT_TTL_MS;
    static constexpr uint8_t MAX_ENTRIES = HOP_HEALTH_MAX_ENTRIES;

    HopHealth() { clear(); }

    void clear();

    bool isSuspect(NodeNum destination, NodeNum nextHop, uint32_t nowMs);

    /// @return true when this miss is the one that made the pair suspect.
    bool recordMiss(NodeNum destination, NodeNum nextHop, uint32_t nowMs);

    /// @return true when the pair had been suspect and is now healthy again.
    bool recordSuccess(NodeNum destination, NodeNum nextHop);

    uint8_t missCount(NodeNum destination, NodeNum nextHop) const;

  private:
    struct Entry {
        NodeNum destination = 0;
        NodeNum nextHop = 0;
        uint8_t consecutiveMisses = 0;
        uint32_t lastMissMs = 0;
    };

    Entry entries[HOP_HEALTH_MAX_ENTRIES];

    int8_t find(NodeNum destination, NodeNum nextHop) const;
    /// The live entry for the pair, after dropping it when its last miss is older than the TTL.
    int8_t findLive(NodeNum destination, NodeNum nextHop, uint32_t nowMs);
    uint8_t allocSlot(uint32_t nowMs);
};
