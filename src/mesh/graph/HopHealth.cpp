#include "HopHealth.h"

void HopHealth::clear()
{
    for (uint8_t i = 0; i < HOP_HEALTH_MAX_ENTRIES; i++) {
        entries[i] = Entry();
    }
}

int8_t HopHealth::find(NodeNum destination, NodeNum nextHop) const
{
    if (destination == 0 || nextHop == 0) {
        return -1;
    }
    for (uint8_t i = 0; i < HOP_HEALTH_MAX_ENTRIES; i++) {
        if (entries[i].destination == destination && entries[i].nextHop == nextHop) {
            return (int8_t)i;
        }
    }
    return -1;
}

int8_t HopHealth::findLive(NodeNum destination, NodeNum nextHop, uint32_t nowMs)
{
    int8_t idx = find(destination, nextHop);
    if (idx < 0) {
        return -1;
    }
    if ((uint32_t)(nowMs - entries[idx].lastMissMs) >= HOP_HEALTH_SUSPECT_TTL_MS) {
        entries[idx] = Entry();
        return -1;
    }
    return idx;
}

uint8_t HopHealth::allocSlot(uint32_t nowMs)
{
    for (uint8_t i = 0; i < HOP_HEALTH_MAX_ENTRIES; i++) {
        if (entries[i].destination == 0 || entries[i].nextHop == 0) {
            return i;
        }
    }
    uint8_t best = 0;
    uint32_t bestAge = 0;
    for (uint8_t i = 0; i < HOP_HEALTH_MAX_ENTRIES; i++) {
        uint32_t age = nowMs - entries[i].lastMissMs;
        if (age >= bestAge) {
            bestAge = age;
            best = i;
        }
    }
    return best;
}

bool HopHealth::isSuspect(NodeNum destination, NodeNum nextHop, uint32_t nowMs)
{
    int8_t idx = findLive(destination, nextHop, nowMs);
    return idx >= 0 && entries[idx].consecutiveMisses >= HOP_HEALTH_SUSPECT_MISSES;
}

bool HopHealth::recordMiss(NodeNum destination, NodeNum nextHop, uint32_t nowMs)
{
    if (destination == 0 || nextHop == 0) {
        return false;
    }
    int8_t idx = findLive(destination, nextHop, nowMs);
    if (idx < 0) {
        uint8_t slot = allocSlot(nowMs);
        entries[slot] = Entry{destination, nextHop, 1, nowMs};
        return false;
    }
    Entry &e = entries[idx];
    if (e.consecutiveMisses < 255) {
        e.consecutiveMisses++;
    }
    e.lastMissMs = nowMs;
    return e.consecutiveMisses == HOP_HEALTH_SUSPECT_MISSES;
}

bool HopHealth::recordSuccess(NodeNum destination, NodeNum nextHop)
{
    int8_t idx = find(destination, nextHop);
    if (idx < 0) {
        return false;
    }
    bool wasSuspect = entries[idx].consecutiveMisses >= HOP_HEALTH_SUSPECT_MISSES;
    entries[idx] = Entry();
    return wasSuspect;
}

uint8_t HopHealth::missCount(NodeNum destination, NodeNum nextHop) const
{
    int8_t idx = find(destination, nextHop);
    if (idx < 0) {
        return 0;
    }
    return entries[idx].consecutiveMisses;
}
