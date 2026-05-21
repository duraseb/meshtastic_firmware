#include "SignalRoutingModule.h"

#if !MESHTASTIC_EXCLUDE_SIGNALROUTING

#include "graph/NeighborGraph.h"
#include "MeshService.h"
#include "MeshTypes.h"
#include "NodeDB.h"
#include "RTC.h"
#include "Router.h"
#include "configuration.h"
#include "memGet.h"
#include "pb_decode.h"
#include <Arduino.h>
#include <algorithm>

SignalRoutingModule *signalRoutingModule;

// --- Fixed-size topology version table helpers ---
uint8_t SignalRoutingModule::getTopologyVersion(const TopologyVersionEntry *table, uint8_t count, NodeNum nodeId) const
{
    for (uint8_t i = 0; i < count; i++) {
        if (table[i].nodeId == nodeId) return table[i].version;
    }
    return 0;
}

void SignalRoutingModule::setTopologyVersion(TopologyVersionEntry *table, uint8_t &count, NodeNum nodeId, uint8_t version)
{
    for (uint8_t i = 0; i < count; i++) {
        if (table[i].nodeId == nodeId) {
            table[i].version = version;
            return;
        }
    }
    if (count < MAX_TOPOLOGY_VERSION_ENTRIES) {
        table[count].nodeId = nodeId;
        table[count].version = version;
        count++;
    } else {
        // Overwrite oldest (slot 0) and shift isn't worth it — just overwrite first slot
        table[0].nodeId = nodeId;
        table[0].version = version;
    }
}

// Helper to get node display name for logging
static void getNodeDisplayName(NodeNum nodeId, char *buf, size_t bufSize) {
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

// Helper to compute age in seconds; returns -1 if unknown/invalid
static int32_t computeAgeSecs(uint32_t last, uint32_t now)
{
    static constexpr uint32_t MAX_AGE_DISPLAY_SEC = 30 * 24 * 60 * 60; // 30 days
    if (!last) return -1;
    // Guard against bogus future timestamps (e.g., legacy nodes that send 0/invalid)
    if (last > now + 86400) return -1;
    int32_t age = static_cast<int32_t>(now - last);
    if (age < 0) age = 0;
    if (static_cast<uint32_t>(age) > MAX_AGE_DISPLAY_SEC) return -1;
    return age;
}



bool SignalRoutingModule::isDirectPacket(const meshtastic_MeshPacket &mp)
{
    if (mp.hop_start != mp.hop_limit)
        return false;
    // If relay_node is set and doesn't match the sender's last byte,
    // a different node relayed this (stock nodes may not decrement hop_limit)
    if (mp.relay_node != 0 && mp.relay_node != (mp.from & 0xFF))
        return false;
    return true;
}

SignalRoutingModule::SignalRoutingModule()
    : ProtobufModule("SignalRouting", meshtastic_PortNum_SIGNAL_ROUTING_APP, &meshtastic_SignalRoutingInfo_msg),
      concurrency::OSThread("SignalRouting")
{
#ifdef ARCH_STM32WL
    // STM32WL only has 64KB RAM total - disable signal routing entirely
    LOG_INFO("[SR] Disabled on STM32WL (insufficient RAM)");
    routingGraph = nullptr;
    disable();
    return;
#endif

#ifdef ARCH_RP2040
    // RP2040 RAM guard: Graph uses ~25-35KB worst case (100 nodes, 6 edges each)
    // 30KB threshold leaves headroom for graph + Dijkstra temp allocations
    uint32_t freeHeap = memGet.getFreeHeap();
    if (freeHeap < 30 * 1024) {
        LOG_WARN("[SR] Insufficient RAM on RP2040 (%u bytes free), disabling signal-based routing", freeHeap);
        routingGraph = nullptr;
        disable();
        return;
    }
#endif
    LOG_INFO("[SR] Using NeighborGraph");
    routingGraph = new NeighborGraph();

    if (!routingGraph) {
        LOG_WARN("[SR] Failed to allocate Graph, disabling signal-based routing");
        disable();
        return;
    }

    if (!nodeDB) {
        LOG_WARN("[SR] NodeDB not available, disabling signal-based routing");
        delete routingGraph;
        routingGraph = nullptr;
        disable();
        return;
    }

    // Load config overrides. Numeric fields: 0 means "use firmware default". Bool fields: if the
    // message exists, the value is used as-is (proto3 default false = disabled).
    if (moduleConfig.has_signal_routing) {
        const auto &srCfg = moduleConfig.signal_routing;
        signalBasedRoutingEnabled = srCfg.enabled;
        t1RetransmitEnabled = srCfg.t1_retransmit_enabled;
        if (srCfg.topology_broadcast_secs != 0) {
            cfgBroadcastSecs = srCfg.topology_broadcast_secs;
        }
        if (srCfg.dirty_broadcast_secs != 0) {
            cfgDirtyBroadcastSecs = srCfg.dirty_broadcast_secs;
        }
        if (srCfg.node_ttl_secs != 0) {
            cfgNodeTtlSecs = srCfg.node_ttl_secs;
        }
        if (srCfg.broadcast_max_hops != 0) {
            cfgBroadcastMaxHops = srCfg.broadcast_max_hops;
        }
        if (srCfg.poor_link_etx_threshold != 0.0f) {
            cfgPoorLinkEtxThreshold = srCfg.poor_link_etx_threshold;
        }
        if (srCfg.etx_change_threshold != 0.0f) {
            routingGraph->setEtxChangeThreshold(srCfg.etx_change_threshold);
        }
        LOG_INFO("[SR] Config: enabled=%d t1=%d broadcastSecs=%u dirtyBroadcastSecs=%u nodeTtlSecs=%u maxHops=%u poorLinkEtx=%.1f etxChange=%.2f",
                 signalBasedRoutingEnabled, t1RetransmitEnabled, cfgBroadcastSecs, cfgDirtyBroadcastSecs,
                 cfgNodeTtlSecs, cfgBroadcastMaxHops, cfgPoorLinkEtxThreshold,
                 routingGraph->getEtxChangeThreshold());
    }

    trackNodeCapability(nodeDB->getNodeNum(), CapabilityStatus::SRactive);

    // We want to see all packets for signal quality updates
    isPromiscuous = true;

    // Send empty SR broadcast shortly after boot to announce our presence.
    // SR neighbors that receive this will trigger an early broadcast of their
    // own topology, helping us bootstrap our graph quickly.
    needsBootBroadcast = true;
    setIntervalFromNow(5 * 1000);

    LOG_INFO("[SR] Module initialized (version %d)", SIGNAL_ROUTING_VERSION);
}

void SignalRoutingModule::markTopologyDirty()
{
    if (!topologyDirty) {
        topologyDirty = true;
        // Wake runOnce() to fire the early broadcast as soon as the minimum inter-broadcast
        // delay has elapsed since the last broadcast. If it already has, wake immediately.
        uint32_t sinceLastBroadcast = millis() - lastBroadcast;
        uint32_t wakeIn = (sinceLastBroadcast >= cfgDirtyBroadcastSecs * 1000) ? 0 : (cfgDirtyBroadcastSecs * 1000 - sinceLastBroadcast);
        setIntervalFromNow(wakeIn);
    }
}

// Write the 5-byte packed header into buf.
static inline void writePackedHeader(uint8_t *buf, uint8_t topologyVersion, bool signalRoutingActive)
{
    buf[0] = PACKED_NEIGHBOR_FORMAT_VERSION;
    buf[1] = PACKED_NEIGHBOR_ENTRY_SIZE;
    buf[2] = SIGNAL_ROUTING_VERSION;
    buf[3] = topologyVersion;
    buf[4] = signalRoutingActive ? PACKED_HEADER_FLAG_SR_ACTIVE : 0;
}

int32_t SignalRoutingModule::runOnce()
{
    uint32_t nowMs = millis();
    uint32_t nowSecs = millis() / 1000;  // Use monotonic time for aging

    pruneCapabilityCache(nowSecs);
    pruneRelayIdentityCache(nowMs);

    // --- Fire pending T1 broadcast retransmits ---
    for (uint8_t i = 0; i < MAX_PENDING_RETRANSMITS; i++) {
        PendingRetransmit &pr = pendingRetransmits[i];
        if (pr.canceled || pr.packet == nullptr || pr.packetId == 0) {
            continue;
        }
        if ((int32_t)(nowMs - pr.fireAfterMs) >= 0) {
            // All hearsUs neighbors already transmitted this packet — they had it before
            // our relay and will treat our copy as a dupe. T1 is unnecessary.
            if (allHearsUsNeighborsHeardPacket(pr.packetId)) {
                LOG_INFO("[SR] T1 canceled for 0x%08x — all hearsUs neighbors already heard packet", pr.packetId);
                if (pr.packet) {
                    packetPool.release(pr.packet);
                    pr.packet = nullptr;
                }
                pr.canceled = true;
                continue;
            }
            LOG_INFO("[SR] T1 firing for 0x%08x — no relay heard in window; retransmitting now", pr.packetId);
            meshtastic_MeshPacket *toSend = pr.packet;
            pr.packet = nullptr;
            pr.canceled = true; // Mark done before send to block T2 scheduling
            // Clear stale tx_after from the original packet's scheduling — the copy was made
            // seconds ago and its tx_after is far in the past. Without this, setTransmitDelay()
            // sees an expired tx_after and draws an immediate short delay, causing busyRx spam
            // if the radio happens to be receiving.
            toSend->tx_after = 0;
            isRetransmitting = true;
            router->send(toSend); // dispatches through FloodingRouter::send()
            isRetransmitting = false;
        }
    }

    if (routingGraph && signalBasedRoutingEnabled) {
        // Send empty SR broadcast at boot to announce presence and trigger
        // neighbor topology responses, regardless of whether we have neighbors yet
        if (needsBootBroadcast) {
            needsBootBroadcast = false;
            LOG_INFO("[SR] Sending empty boot broadcast to bootstrap topology");
            uint8_t bootBuf[PACKED_NEIGHBOR_HEADER_SIZE];
            writePackedHeader(bootBuf, 0, isActiveRoutingRole());
            sendTopologyPacket(NODENUM_BROADCAST, bootBuf, PACKED_NEIGHBOR_HEADER_SIZE);
        } else if (nowMs - lastBroadcast >= cfgBroadcastSecs * 1000) {
            sendSignalRoutingInfo();
            topologyDirty = false;
        } else if (topologyDirty && nowMs - lastBroadcast >= cfgDirtyBroadcastSecs * 1000) {
            // Topology changed and the minimum inter-broadcast interval has elapsed
            // since the last broadcast — send the early broadcast now.
            LOG_INFO("[SR] Topology dirty — sending early broadcast");
            sendSignalRoutingInfo();
            topologyDirty = false;
        }

        // Topology logging: log on every dirty wakeup and at least every GRAPH_MAINTENANCE_INTERVAL_SECS seconds.
        // topologyDirty is intentionally NOT cleared before this point so that if the broadcast
        // was deferred (min interval not yet elapsed), we still log the pending change.
        static uint32_t lastTopologyLog = 0;
        if (topologyDirty || nowMs - lastTopologyLog >= GRAPH_MAINTENANCE_INTERVAL_SECS * 1000) {
            logNetworkTopology();
            lastTopologyLog = nowMs;
        }
    }

    uint32_t broadcastCycle = topologyDirty ? cfgDirtyBroadcastSecs * 1000 : cfgBroadcastSecs * 1000;
    uint32_t elapsed = nowMs - lastBroadcast;
    uint32_t timeToBroadcast = (elapsed < broadcastCycle) ? (broadcastCycle - elapsed) : 0;

    uint32_t nextDelay = timeToBroadcast;

    // Wake up early enough to fire any pending T1 retransmits on time
    for (uint8_t i = 0; i < MAX_PENDING_RETRANSMITS; i++) {
        const PendingRetransmit &pr = pendingRetransmits[i];
        if (pr.canceled || pr.packet == nullptr || pr.packetId == 0) {
            continue;
        }
        int32_t remaining = (int32_t)(pr.fireAfterMs - nowMs);
        if (remaining > 0 && (uint32_t)remaining < nextDelay) {
            nextDelay = (uint32_t)remaining;
        }
    }

    if (nextDelay < 20) {
        nextDelay = 20;
    }
    return static_cast<int32_t>(nextDelay);
}

void SignalRoutingModule::sendSignalRoutingInfo(NodeNum dest)
{
    // Allow mute nodes to broadcast their direct neighbors to help active SR nodes
    // make better unicast routing decisions, even though mute nodes don't participate in relaying
    if (!canSendTopology()) {
        return;
    }

    // Pack all neighbors into a large buffer (header placeholder + entries)
    static constexpr size_t MAX_TOTAL_PACKED = MAX_SIGNAL_ROUTING_NEIGHBORS * 6 * PACKED_NEIGHBOR_ENTRY_SIZE + PACKED_NEIGHBOR_HEADER_SIZE;
    uint8_t allPacked[MAX_TOTAL_PACKED];
    uint8_t totalNeighbors = packNeighborsForBroadcast(allPacked, sizeof(allPacked));

    uint8_t topologyVersion = currentTopologyVersion++;
    bool srActive = isActiveRoutingRole();

    if (totalNeighbors == 0) {
        // Send empty broadcast to announce SR capability.
        uint8_t emptyBuf[PACKED_NEIGHBOR_HEADER_SIZE];
        writePackedHeader(emptyBuf, topologyVersion, srActive);
        sendTopologyPacket(dest, emptyBuf, PACKED_NEIGHBOR_HEADER_SIZE);
        return;
    }

    // Split into chunks of MAX_SIGNAL_ROUTING_NEIGHBORS and send multiple packets
    uint8_t packetsNeeded = (totalNeighbors + MAX_SIGNAL_ROUTING_NEIGHBORS - 1) / MAX_SIGNAL_ROUTING_NEIGHBORS;

    char ourName[48];
    getNodeDisplayName(nodeDB->getNodeNum(), ourName, sizeof(ourName));

    LOG_INFO("[SR] SENDING: Broadcasting %u neighbors in %u packet(s) from %s (version %u)",
             totalNeighbors, packetsNeeded, ourName, topologyVersion);

    // Space multi-packet broadcasts by 2× the packet airtime so relaying nodes
    // finish transmitting packet N before packet N+1 arrives.
    uint32_t packetSpacingMs = 300; // conservative fallback
    if (router && router->getRadioInterface()) {
        meshtastic_SignalRoutingInfo dummyInfo = meshtastic_SignalRoutingInfo_init_zero;
        size_t chunkLen = PACKED_NEIGHBOR_HEADER_SIZE +
                          std::min((uint8_t)MAX_SIGNAL_ROUTING_NEIGHBORS, totalNeighbors) * PACKED_NEIGHBOR_ENTRY_SIZE;
        memcpy(dummyInfo.packed_neighbors.bytes, allPacked, std::min(chunkLen, sizeof(dummyInfo.packed_neighbors.bytes)));
        dummyInfo.packed_neighbors.size = chunkLen;
        meshtastic_MeshPacket *dummy = allocDataProtobuf(dummyInfo);
        packetSpacingMs = 2 * router->getRadioInterface()->getPacketTime(dummy);
        packetPool.release(dummy);
    }

    for (uint8_t packetIndex = 0; packetIndex < packetsNeeded; packetIndex++) {
        uint8_t startNeighbor = packetIndex * MAX_SIGNAL_ROUTING_NEIGHBORS;
        uint8_t count = std::min((uint8_t)MAX_SIGNAL_ROUTING_NEIGHBORS, (uint8_t)(totalNeighbors - startNeighbor));

        // Build per-packet packed buffer: 5-byte header + this chunk's entries
        uint8_t chunkBuf[PACKED_NEIGHBOR_HEADER_SIZE + MAX_SIGNAL_ROUTING_NEIGHBORS * PACKED_NEIGHBOR_ENTRY_SIZE];
        writePackedHeader(chunkBuf, topologyVersion, srActive);
        size_t dataOffset = PACKED_NEIGHBOR_HEADER_SIZE + startNeighbor * PACKED_NEIGHBOR_ENTRY_SIZE;
        memcpy(&chunkBuf[PACKED_NEIGHBOR_HEADER_SIZE], &allPacked[dataOffset], count * PACKED_NEIGHBOR_ENTRY_SIZE);
        size_t chunkLen = PACKED_NEIGHBOR_HEADER_SIZE + count * PACKED_NEIGHBOR_ENTRY_SIZE;

        uint32_t txAfter = packetIndex > 0 ? millis() + packetIndex * packetSpacingMs : 0;
        sendTopologyPacket(dest, chunkBuf, chunkLen);
    }

    // Update our own capability after sending
    trackNodeCapability(nodeDB->getNodeNum(), isActiveRoutingRole() ? CapabilityStatus::SRactive : CapabilityStatus::Passive);
}

void SignalRoutingModule::notifyOriginatedPacketSent()
{
    LOG_INFO("[SR] Originated packet sent — resetting topology broadcast timer");
    lastBroadcast = millis();
}

uint8_t SignalRoutingModule::packNeighborsForBroadcast(uint8_t *outBuf, size_t bufSize)
{
    if (!routingGraph || !nodeDB) {
        return 0;
    }

    const NodeEdges *nodeEdges = routingGraph->getEdgesFrom(nodeDB->getNodeNum());
    if (!nodeEdges || nodeEdges->edgeCount == 0) {
        return 0;
    }

    // Collect non-placeholder edge pointers into a fixed-size array for sorting
    const Edge *edgePtrs[NEIGHBOR_GRAPH_MAX_EDGES_PER_NODE];
    uint8_t edgeCount = 0;
    for (uint8_t i = 0; i < nodeEdges->edgeCount && edgeCount < NEIGHBOR_GRAPH_MAX_EDGES_PER_NODE; i++) {
        if (!isPlaceholderNode(nodeEdges->edges[i].to)) {
            edgePtrs[edgeCount++] = &nodeEdges->edges[i];
        }
    }

    // Sort by quality (reported first, then by ETX)
    std::sort(edgePtrs, edgePtrs + edgeCount, [](const Edge *a, const Edge *b) {
        if (a->source != b->source) {
            return a->source == Edge::Source::Reported;
        }
        return a->getEtx() < b->getEtx();
    });

    // Reserve header space (caller fills in the full 5-byte header per chunk via writePackedHeader)
    memset(outBuf, 0, PACKED_NEIGHBOR_HEADER_SIZE);

    // Pack each neighbor as 8 bytes: [4B node_id LE][1B rssi][1B snr][1B flags][1B etx_variance]
    uint8_t count = 0;
    size_t maxEntries = (bufSize - PACKED_NEIGHBOR_HEADER_SIZE) / PACKED_NEIGHBOR_ENTRY_SIZE;
    for (uint8_t i = 0; i < edgeCount && count < maxEntries; i++) {
        const Edge *edge = edgePtrs[i];
        uint8_t *entry = &outBuf[PACKED_NEIGHBOR_HEADER_SIZE + count * PACKED_NEIGHBOR_ENTRY_SIZE];

        // node_id in little-endian
        uint32_t nodeId = edge->to;
        entry[0] = (nodeId >> 0) & 0xFF;
        entry[1] = (nodeId >> 8) & 0xFF;
        entry[2] = (nodeId >> 16) & 0xFF;
        entry[3] = (nodeId >> 24) & 0xFF;

        int32_t rssi32, snr32;
        NeighborGraph::etxToSignal(edge->getEtx(), rssi32, snr32);
        entry[4] = static_cast<uint8_t>(static_cast<int8_t>(std::max((int32_t)-128, std::min((int32_t)127, rssi32))));
        entry[5] = static_cast<uint8_t>(static_cast<int8_t>(std::max((int32_t)-128, std::min((int32_t)127, snr32))));

        uint8_t flags = 0;
        CapabilityStatus neighborStatus = getCapabilityStatus(edge->to);
        if (neighborStatus == CapabilityStatus::SRactive) {
            flags |= PACKED_NEIGHBOR_FLAG_SR_ACTIVE;
        }
        if (edge->hearsUs) {
            flags |= PACKED_NEIGHBOR_FLAG_HEARS_US;
        }
        entry[6] = flags;
        entry[7] = edge->etxVariance;

        count++;
    }

    return count;
}

void SignalRoutingModule::sendTopologyPacket(NodeNum dest, const uint8_t *packedData, size_t packedLen, uint8_t /*topologyVersion*/, uint32_t txAfterMs)
{
    meshtastic_SignalRoutingInfo info = meshtastic_SignalRoutingInfo_init_zero;

    if (packedData && packedLen > 0) {
        size_t copyLen = std::min(packedLen, sizeof(info.packed_neighbors.bytes));
        memcpy(info.packed_neighbors.bytes, packedData, copyLen);
        info.packed_neighbors.size = copyLen;
    }

    meshtastic_MeshPacket *p = allocDataProtobuf(info);
    p->to = dest;
    p->hop_limit = std::min(p->hop_limit, (uint8_t)cfgBroadcastMaxHops);
    p->priority = meshtastic_MeshPacket_Priority_BACKGROUND;
    if (txAfterMs) {
        p->tx_after = txAfterMs;
    }

    service->sendToMesh(p);

    // Record our transmission for contention window tracking
    if (routingGraph) {
        uint32_t currentTime = millis() / 1000;  // Use monotonic time
        routingGraph->recordNodeTransmission(nodeDB->getNodeNum(), p->id, currentTime);
    }
}


void SignalRoutingModule::updateGraphWithNeighbor(NodeNum sender, NodeNum neighborId, int8_t rssi, int8_t snr, bool hearsUs)
{
    // Add/update edge from sender to this neighbor
    if (routingGraph) {
        float etx = (rssi != 0 || snr != 0)
                        ? NeighborGraph::calculateETX(rssi, snr)
                        : 1.0f;
        uint32_t currentTime = millis() / 1000;

        // Do NOT refresh the edge timestamp from topology broadcasts: the timestamp must reflect
        // when the edge was last directly observed, not when we received someone's topology report.
        // Refreshing it here would create phantom references — a dead node stays in the graph
        // as long as any neighbor keeps mentioning it, potentially chaining across multiple hops.
        // New edges (first creation) still receive currentTime from the new-edge path in updateEdge.
        routingGraph->updateEdge(sender, neighborId, etx, currentTime,
                                 Edge::Source::Mirrored, /*updateTimestamp=*/false);

        // Propagate the bidirectional link flag from the authoritative sender
        routingGraph->setEdgeHearsUs(sender, neighborId, hearsUs);
    }
}

void SignalRoutingModule::preProcessSignalRoutingPacket(const meshtastic_MeshPacket *p, uint32_t packetReceivedTimestamp)
{
    if (!routingGraph || !p) return;

    // Skip processing entirely for packets we sent (detected as rebroadcasts)
    // This prevents topology pollution from our own rebroadcasted packets
    NodeNum ourNode = nodeDB ? nodeDB->getNodeNum() : 0;
    uint32_t currentTime = millis() / 1000;
    if (routingGraph->hasNodeTransmitted(ourNode, p->id, currentTime)) {
        LOG_INFO("[SR] Skip topo proc (own rebcast %08x)", p->id);
        return;
    }

    // Only process SignalRoutingInfo packets
    if (p->decoded.portnum != meshtastic_PortNum_SIGNAL_ROUTING_APP) {
        LOG_INFO("[SR] Skip non-SR pkt portnum=%d", p->decoded.portnum);
        return;
    }
    // Reject packets from invalid node IDs (0 is invalid)
    if (p->from == 0) {
        LOG_WARN("[SR] SR bcast: invalid id 0");
        return;
    }
    
    // For passive nodes: only process SR broadcasts from direct neighbors
    // Active nodes: process all SR broadcasts for full topology
    if (!isActiveRoutingRole()) {
        // Passive node: check if SR broadcast is from direct sender
        if (!isDirectPacket(*p)) {
            LOG_INFO("[SR] Passive: ignore SR bcast 0x%08x (hs=%d hl=%d)",
                     p->from, p->hop_start, p->hop_limit);
            return;
        }
    }

    // Decode the protobuf to get the packed binary blob
    meshtastic_SignalRoutingInfo info = meshtastic_SignalRoutingInfo_init_zero;
    if (!pb_decode_from_bytes(p->decoded.payload.bytes, p->decoded.payload.size,
                              &meshtastic_SignalRoutingInfo_msg, &info)) {
        LOG_WARN("[SR] SRInfo decode failed from %08x (sz=%u)",
                 p->from, p->decoded.payload.size);
        return;
    }

    // Decode packed neighbors and header
    PackedHeader hdr = {};
    PackedNeighborEntry neighbors[MAX_SIGNAL_ROUTING_NEIGHBORS];
    uint8_t neighborCount = decodePackedNeighbors(info.packed_neighbors.bytes, info.packed_neighbors.size,
                                                   neighbors, MAX_SIGNAL_ROUTING_NEIGHBORS, &hdr);

    // V2 packets land on field 3 but fail the format_version check — treat as stock node, skip
    if (hdr.formatVersion != PACKED_NEIGHBOR_FORMAT_VERSION) {
        return;
    }

    // Check version validity with wraparound logic
    uint8_t receivedVersion = hdr.topologyVersion;
    uint8_t lastProcessedVersion = getTopologyVersion(lastTopologyVersion, lastTopologyVersionCount, p->from);

    bool accept = false;
    if (receivedVersion > lastProcessedVersion) {
        accept = true;
    } else if (receivedVersion < lastProcessedVersion) {
        uint8_t threshold = (lastProcessedVersion + 256 - 100) % 256;
        if (receivedVersion >= threshold || receivedVersion < 100) {
            accept = true;
        }
    } else if (receivedVersion == lastProcessedVersion) {
        accept = true;
    }

    if (!accept) {
        LOG_INFO("[SR] Stale topo bcast %08x v%u (last %u)",
                 p->from, receivedVersion, lastProcessedVersion);
        return;
    }

    bool isNewVersion = (receivedVersion != lastProcessedVersion);
    setTopologyVersion(lastTopologyVersion, lastTopologyVersionCount, p->from, receivedVersion);

    // Update capability status for the sender
    CapabilityStatus newStatus = hdr.signalRoutingActive ? CapabilityStatus::SRactive : CapabilityStatus::Passive;
    CapabilityStatus oldStatus = getCapabilityStatus(p->from);
    trackNodeCapability(p->from, newStatus);

    if (oldStatus != newStatus) {
        char senderName[64];
        getNodeDisplayName(p->from, senderName, sizeof(senderName));
        LOG_INFO("[SR] Cap: %s %d->%d",
                senderName, (int)oldStatus, (int)newStatus);
    }

    char senderNameForTopo[48];
    getNodeDisplayName(p->from, senderNameForTopo, sizeof(senderNameForTopo));
    LOG_INFO("[SR] Topo from %s: %dn v%u %s r=0x%02x",
              senderNameForTopo, neighborCount, receivedVersion,
              isNewVersion ? "new version" : "continuation", p->relay_node);

    // Empty SR broadcast from a direct SR neighbor = bootstrap request.
    if (neighborCount == 0 && isDirectPacket(*p) && hdr.signalRoutingActive) {
        LOG_INFO("[SR] Empty bcast from %s, topo dirty",
                 senderNameForTopo);
        markTopologyDirty();
    }

    // Mirrored edges are not cleared on new topology versions — they age out naturally.

    // Process each neighbor from the decoded packed data
    for (uint8_t i = 0; i < neighborCount; i++) {
        const PackedNeighborEntry &neighbor = neighbors[i];

        // Reject neighbors with invalid node IDs (0 or placeholders)
        if (neighbor.nodeId == 0 || isPlaceholderNode(neighbor.nodeId)) {
            LOG_WARN("[SR] Invalid neighbor id: %08x", neighbor.nodeId);
            continue;
        }

        // Process this neighbor directly - capability status was already handled for the main sender
        updateGraphWithNeighbor(p->from, neighbor.nodeId, neighbor.rssi, neighbor.snr, neighbor.hearsUs);

        // Create gateway relationship ONLY for nodes we cannot hear directly
        bool hasDirectConnection = false;
        NodeNum ourNode = nodeDB ? nodeDB->getNodeNum() : 0;

        // Never mark ourselves as downstream of anyone
        if (neighbor.nodeId == ourNode) {
            hasDirectConnection = true;
        } else if (routingGraph) {
            // A direct connection is confirmed if the neighbor has a Reported edge TO us
            const NodeEdges *neighborEdges = routingGraph->getEdgesFrom(neighbor.nodeId);
            if (neighborEdges) {
                for (uint8_t j = 0; j < neighborEdges->edgeCount; j++) {
                    if (neighborEdges->edges[j].to == ourNode &&
                        neighborEdges->edges[j].source == Edge::Source::Reported) {
                        hasDirectConnection = true;
                        break;
                    }
                }
            }
        }

        char neighborName[48];
        getNodeDisplayName(neighbor.nodeId, neighborName, sizeof(neighborName));

        if (!hasDirectConnection && neighbor.hearsUs) {
            // Only mark as downstream if the link is bidirectional — the neighbor must be able
            // to hear the topology source, otherwise the source cannot actually deliver to it.
            LOG_INFO("[SR]   -> %s: no direct, downstream of %s",
                    neighborName, senderNameForTopo);
            float etxForDownstream = NeighborGraph::calculateETX(neighbor.rssi, neighbor.snr);
            routingGraph->updateDownstream(neighbor.nodeId, p->from, etxForDownstream, millis() / 1000);
        } else if (!hasDirectConnection && !neighbor.hearsUs) {
            LOG_INFO("[SR]   -> %s: no direct (asym, skip ds of %s)",
                    neighborName, senderNameForTopo);
        } else {
            LOG_INFO("[SR]   -> %s: direct (confirmed)",
                    neighborName);
        }
    }

    // Authoritative hearsUs override: the topology source is authoritative about who it can hear.
    // If another node claims hearsUs=true on its edge to the source, but the source didn't list
    // that node as a neighbor, clear the flag — the source can't actually hear that node.
    if (routingGraph && neighborCount > 0) {
        NodeNum allNodes[NEIGHBOR_GRAPH_MAX_NEIGHBORS];
        size_t nodeCount = routingGraph->getAllNodeIds(allNodes, NEIGHBOR_GRAPH_MAX_NEIGHBORS);
        for (size_t n = 0; n < nodeCount; n++) {
            NodeNum nodeId = allNodes[n];
            if (nodeId == p->from) {
                continue;
            }
            const NodeEdges *nodeEdges = routingGraph->getEdgesFrom(nodeId);
            if (!nodeEdges) {
                continue;
            }
            for (uint8_t e = 0; e < nodeEdges->edgeCount; e++) {
                if (nodeEdges->edges[e].to == p->from && nodeEdges->edges[e].hearsUs) {
                    // Check if the source listed this node as a neighbor
                    bool foundInNeighborList = false;
                    for (uint8_t i = 0; i < neighborCount; i++) {
                        if (neighbors[i].nodeId == nodeId) {
                            foundInNeighborList = true;
                            break;
                        }
                    }
                    if (!foundInNeighborList) {
                        char nodeName[48];
                        getNodeDisplayName(nodeId, nodeName, sizeof(nodeName));
                        LOG_INFO("[SR] Clear hearsUs %s->%s (src topo missing %s)",
                                 nodeName, senderNameForTopo, nodeName);
                        routingGraph->setEdgeHearsUs(nodeId, p->from, false);
                    }
                }
            }
        }
    }

    // Update last processed version (minimal state tracking)
    setTopologyVersion(lastTopologyVersion, lastTopologyVersionCount, p->from, receivedVersion);

    // Record that this version was pre-processed so handleReceivedProtobuf can skip redundant work
    setTopologyVersion(lastPreProcessedVersion, lastPreProcessedVersionCount, p->from, receivedVersion);

}
meshtastic_MeshPacket *SignalRoutingModule::allocReply()
{
    // Never send unicast replies to SR broadcasts — they cause stock nodes
    // to generate NO_RESPONSE NAKs (portnum=5) that flood the mesh.
    // Instead, SR nodes respond to empty broadcasts by triggering an early
    // broadcast of their own topology.
    ignoreRequest = true;
    return nullptr;
}

bool SignalRoutingModule::handleReceivedProtobuf(const meshtastic_MeshPacket &mp, meshtastic_SignalRoutingInfo *p)
{
    // Reject packets from invalid node IDs (0 is invalid)
    if (mp.from == 0) {
        LOG_INFO("[SR] Ignoring SR broadcast from invalid node ID 0 in handleReceivedProtobuf");
        return false;
    }

    // Note: Graph may have already been updated by preProcessSignalRoutingPacket()
    // This is intentional - we want up-to-date data for relay decisions
    if (!routingGraph || !p) return false;

    char senderName[64];
    getNodeDisplayName(mp.from, senderName, sizeof(senderName));

    // Decode packed neighbors and header
    PackedHeader hdr = {};
    PackedNeighborEntry neighbors[MAX_SIGNAL_ROUTING_NEIGHBORS];
    uint8_t neighborCount = decodePackedNeighbors(p->packed_neighbors.bytes, p->packed_neighbors.size,
                                                   neighbors, MAX_SIGNAL_ROUTING_NEIGHBORS, &hdr);

    // V2 packets land on field 3 but fail the format_version check — treat as stock node, skip
    if (hdr.formatVersion != PACKED_NEIGHBOR_FORMAT_VERSION) {
        return false;
    }

    // Mark sender based on their claimed SR capability
    CapabilityStatus newStatus = hdr.signalRoutingActive ? CapabilityStatus::SRactive : CapabilityStatus::Passive;
    CapabilityStatus oldStatus = getCapabilityStatus(mp.from);
    trackNodeCapability(mp.from, newStatus);

    if (oldStatus != newStatus) {
        LOG_INFO("[SR] Cap: %s %d->%d",
                senderName, (int)oldStatus, (int)newStatus);
    }

    // Inactive SR roles don't participate in routing decisions - skip topology learning from broadcasts
    if (!isActiveRoutingRole()) {
        LOG_INFO("[SR] Passive role: Tracking capability from %s but not processing topology (node count %d)",
                  senderName, neighborCount);
        return false;
    }

    if (neighborCount == 0) {
        LOG_INFO("[SR] %s is online (SR v%d, %s) - no neighbors detected yet",
                 senderName, hdr.routingVersion,
                 hdr.signalRoutingActive ? "SR-active" : "passive");

        if (hdr.signalRoutingActive) {
            routingGraph->clearDownstreamForRelay(mp.from);
        }

        if (hdr.signalRoutingActive && isDirectPacket(mp)) {
            LOG_INFO("[SR] Empty bcast from %s, topo dirty",
                     senderName);
            markTopologyDirty();
        }

        return false;
    }

    LOG_INFO("[SR] RECEIVED: %s reports %d neighbors (SR v%d, %s)",
             senderName, neighborCount, hdr.routingVersion,
             hdr.signalRoutingActive ? "SR-active" : "passive");

    if (!hdr.signalRoutingActive) {
        LOG_INFO("[SR] Received topology from passive SR node %08x - storing edges for direct connection detection", mp.from);
    }

    // Check if preProcessSignalRoutingPacket already handled edge clearing and rebuilding
    uint8_t preProcessedVer = getTopologyVersion(lastPreProcessedVersion, lastPreProcessedVersionCount, mp.from);
    bool alreadyPreProcessed = (preProcessedVer != 0 && preProcessedVer == hdr.routingVersion);

    if (!alreadyPreProcessed) {
        // Clear inferred edges pointing TO this node that were created before we knew it was SR-capable
        routingGraph->clearInferredEdgesToNode(mp.from);

        uint32_t rxTime = millis() / 1000;
        for (uint8_t i = 0; i < neighborCount; i++) {
            const PackedNeighborEntry &neighbor = neighbors[i];

            if (neighbor.nodeId == 0 || isPlaceholderNode(neighbor.nodeId)) {
                continue;
            }

            float etx = NeighborGraph::calculateETX(neighbor.rssi, neighbor.snr);

            routingGraph->updateEdge(neighbor.nodeId, mp.from, etx, rxTime,
                                     Edge::Source::Reported);
            routingGraph->updateEdge(mp.from, neighbor.nodeId, etx, rxTime,
                                     Edge::Source::Mirrored);

            routingGraph->setEdgeHearsUs(mp.from, neighbor.nodeId, neighbor.hearsUs);
        }
    } else {
        LOG_INFO("[SR] Skipping redundant edge rebuild for %s (already pre-processed version %u)",
                 senderName, hdr.routingVersion);
    }

    // Always process gateway relations and logging (even if edges were already built)
    for (uint8_t i = 0; i < neighborCount; i++) {
        const PackedNeighborEntry &neighbor = neighbors[i];

        if (neighbor.nodeId == 0 || isPlaceholderNode(neighbor.nodeId)) {
            continue;
        }

        char neighborName[64];
        getNodeDisplayName(neighbor.nodeId, neighborName, sizeof(neighborName));

        float etx = NeighborGraph::calculateETX(neighbor.rssi, neighbor.snr);

        const char *quality;
        if (etx < 2.0f) quality = "excellent";
        else if (etx < 4.0f) quality = "good";
        else if (etx < 8.0f) quality = "fair";
        else quality = "poor";

        LOG_INFO("  ├── %s: %s link (%s, ETX=%.1f, var=%u)",
                 neighborName,
                 neighbor.signalRoutingActive ? "SR-active" : "SR-inactive",
                 quality, etx,
                 neighbor.etxVariance);

        // If the sender is SR-capable and reports this neighbor as directly reachable,
        // clear downstream entries for this neighbor
        if (hdr.signalRoutingActive) {
            NodeNum relayForNeighbor = routingGraph->getDownstreamRelay(neighbor.nodeId);
            if (relayForNeighbor != 0 && relayForNeighbor != mp.from) {
                char gwName[64];
                getNodeDisplayName(relayForNeighbor, gwName, sizeof(gwName));
                LOG_INFO("[SR] Clearing downstream for %s (now directly reachable via %s, was via %s)",
                         neighborName, senderName, gwName);
                routingGraph->clearDownstreamForDestination(neighbor.nodeId);
            }
        }
    }

    // Log network topology summary
    LOG_INFO("[SR] Network topology updated - %s now connected to %d neighbors",
             senderName, neighborCount);

    // Allow others to see this packet too
    return false;
}

/**
 * Log the current network topology graph in a readable format
 */
// Placeholder node system for unknown relays
// Use high NodeNum values that won't conflict with real nodes
#define PLACEHOLDER_BASE 0xFF000000

bool SignalRoutingModule::isPlaceholderNode(NodeNum nodeId) const
{
    return (nodeId & PLACEHOLDER_BASE) == PLACEHOLDER_BASE;
}

NodeNum SignalRoutingModule::createPlaceholderNode(uint8_t relayId)
{
    return PLACEHOLDER_BASE | relayId;
}

bool SignalRoutingModule::resolvePlaceholder(NodeNum placeholderId, NodeNum realNodeId)
{
    if (!isPlaceholderNode(placeholderId)) {
        return false; // Not a placeholder
    }

    if (isPlaceholderNode(realNodeId)) {
        return false; // Can't resolve to another placeholder
    }

    // Check if this placeholder is already resolved
    uint8_t relayId = placeholderId & 0xFF;
    NodeNum alreadyResolved = resolveRelayIdentity(relayId);
    if (alreadyResolved != 0) {
        if (alreadyResolved == realNodeId) {
            return false; // Already resolved to this node, nothing to do
        }
        LOG_WARN("[SR] Placeholder %08x already resolved to %08x, refusing to resolve to %08x",
                placeholderId, alreadyResolved, realNodeId);
        return false; // Already resolved to a different node
    }


    // Update relay identity cache - this ensures future relay resolutions work
    rememberRelayIdentity(realNodeId, relayId);

    // Update gateway relationships
    replaceGatewayNode(placeholderId, realNodeId);

    // Transfer ONLY legitimate graph edges from placeholder to real node
    // Placeholders are used for inferred connectivity and should not have edges that represent
    // actual neighbor relationships. Only transfer reverse edges from our node to the placeholder,
    // which represent actual radio connectivity that should be preserved.
    if (routingGraph) {
        NodeNum ourNode = nodeDB->getNodeNum();

        // Only transfer reverse edges (where our node had a direct link to the placeholder)
        // These represent actual radio connectivity that should be preserved
        const NodeEdges* ourEdges = routingGraph->getEdgesFrom(ourNode);
        if (ourEdges) {
            for (uint8_t i = 0; i < ourEdges->edgeCount; i++) {
                if (ourEdges->edges[i].to == placeholderId) {
                    float etx = ourEdges->edges[i].getEtx();
                    // Create equivalent edge from our node to real node
                    routingGraph->updateEdge(ourNode, realNodeId, etx, millis() / 1000);
                }
            }
        }

        // Remove the placeholder node from the graph
        // (downstream entries already transferred by replaceGatewayNode above)
        routingGraph->removeNode(placeholderId);

        // Also explicitly remove any orphaned edges pointing to the placeholder
        // (removeNode may not find the placeholder if it was already aged out,
        // but edges from our node to it can still linger in our edge list)
        routingGraph->removeEdgesTo(placeholderId);

        LOG_INFO("[SR] Resolved placeholder %08x -> real node %08x", placeholderId, realNodeId);
        LOG_INFO("[SR] Removed placeholder node %08x from graph", placeholderId);
    }

    return true;
}

NodeNum SignalRoutingModule::getPlaceholderForRelay(uint8_t relayId) const
{
    return PLACEHOLDER_BASE | relayId;
}

void SignalRoutingModule::replaceGatewayNode(NodeNum oldNode, NodeNum newNode)
{
    if (oldNode == newNode || !routingGraph) return;

    // Transfer downstream entries from old relay to new relay
    size_t transferred = routingGraph->transferDownstream(oldNode, newNode);
    if (transferred > 0) {
        LOG_INFO("[SR] Transferred %u downstream entries from %08x to %08x",
                 (unsigned)transferred, oldNode, newNode);
    }
    routingGraph->clearDownstreamForDestination(oldNode);
}

bool SignalRoutingModule::isPlaceholderConnectedToUs(NodeNum placeholderId) const
{
    if (!routingGraph || !isPlaceholderNode(placeholderId)) {
        return false;
    }

    // Check if the placeholder has edges connected to our node
    NodeNum ourNode = nodeDB->getNodeNum();

    const NodeEdges* edges = routingGraph->getEdgesFrom(placeholderId);
    if (edges) {
        for (uint8_t i = 0; i < edges->edgeCount; i++) {
            if (edges->edges[i].to == ourNode) {
                return true;
            }
        }
    }

    return false;
}

bool SignalRoutingModule::hasBetterPositionedSRNeighbor(NodeNum myNode, NodeNum heardFrom, NodeNum destination)
{
    if (!routingGraph || heardFrom == 0 || heardFrom == myNode)
        return false;

    const NodeEdges *myEdges = routingGraph->getEdgesFrom(myNode);
    if (!myEdges)
        return false;

    for (uint8_t i = 0; i < myEdges->edgeCount; i++) {
        NodeNum neighbor = myEdges->edges[i].to;
        if (neighbor == heardFrom || neighbor == myNode || isPlaceholderNode(neighbor))
            continue;

        // Only consider SR-active neighbors
        CapabilityStatus status = getCapabilityStatus(neighbor);
        if (status != CapabilityStatus::SRactive)
            continue;

        const NodeEdges *neighborEdges = routingGraph->getEdgesFrom(neighbor);
        if (!neighborEdges)
            continue;

        // Check if this SR neighbor also has heardFrom as a neighbor
        bool neighborHearsSource = false;
        for (uint8_t j = 0; j < neighborEdges->edgeCount; j++) {
            if (neighborEdges->edges[j].to == heardFrom) {
                neighborHearsSource = true;
                break;
            }
        }
        if (!neighborHearsSource)
            continue;

        // For unicasts, also check if the neighbor can reach the destination
        if (destination != 0) {
            bool canReachDest = false;
            for (uint8_t j = 0; j < neighborEdges->edgeCount; j++) {
                if (neighborEdges->edges[j].to == destination) {
                    canReachDest = true;
                    break;
                }
            }
            if (!canReachDest) {
                // Check if destination is downstream of this neighbor (in piko's table)
                canReachDest = routingGraph->isDownstream(destination) &&
                               routingGraph->getDownstreamRelay(destination) == neighbor;
            }
            if (!canReachDest) {
                // Check if this neighbor has a route to the destination via its own neighbors
                // (destination is a neighbor-of-neighbor, i.e. the neighbor can relay to reach it)
                for (uint8_t j = 0; j < neighborEdges->edgeCount; j++) {
                    NodeNum neighborOfNeighbor = neighborEdges->edges[j].to;
                    if (neighborOfNeighbor == myNode || neighborOfNeighbor == heardFrom)
                        continue;
                    const NodeEdges *nnEdges = routingGraph->getEdgesFrom(neighborOfNeighbor);
                    if (nnEdges) {
                        for (uint8_t k = 0; k < nnEdges->edgeCount; k++) {
                            if (nnEdges->edges[k].to == destination) {
                                canReachDest = true;
                                break;
                            }
                        }
                    }
                    if (canReachDest)
                        break;
                    // Also check if destination is downstream of the neighbor's neighbor
                    if (routingGraph->isDownstream(destination) &&
                        routingGraph->getDownstreamRelay(destination) == neighborOfNeighbor) {
                        canReachDest = true;
                        break;
                    }
                }
            }
            if (!canReachDest)
                continue;
        }

        // For unicasts: the SR neighbor hears the source and can reach the destination — sufficient
        if (destination != 0) {
            char neighborName[64];
            getNodeDisplayName(neighbor, neighborName, sizeof(neighborName));
            LOG_INFO("[SR] SR neighbor %s hears %08x and can reach destination - no relay needed",
                     neighborName, heardFrom);
            return true;
        }

        // For broadcasts: build a combined coverage set from the transmitter and the SR neighbor.
        // If piko has any neighbor not in that set, piko has unique coverage and should still relay.
        NodeSet coveredNodes;
        coveredNodes.insert(heardFrom);
        coveredNodes.insert(neighbor);

        // Add all nodes covered by the transmitter
        const NodeEdges *transmitterEdges = routingGraph->getEdgesFrom(heardFrom);
        if (transmitterEdges) {
            for (uint8_t j = 0; j < transmitterEdges->edgeCount; j++)
                coveredNodes.insert(transmitterEdges->edges[j].to);
        }
        // Add all nodes covered by the SR neighbor
        for (uint8_t j = 0; j < neighborEdges->edgeCount; j++)
            coveredNodes.insert(neighborEdges->edges[j].to);

        // Check if piko has any neighbor not in the combined coverage set
        bool pikoHasUniqueCoverage = false;
        for (uint8_t m = 0; m < myEdges->edgeCount; m++) {
            NodeNum myNeighbor = myEdges->edges[m].to;
            if (isPlaceholderNode(myNeighbor))
                continue;
            if (!coveredNodes.contains(myNeighbor)) {
                pikoHasUniqueCoverage = true;
                break;
            }
        }

        if (!pikoHasUniqueCoverage) {
            char neighborName[64];
            getNodeDisplayName(neighbor, neighborName, sizeof(neighborName));
            LOG_INFO("[SR] SR neighbor %s also hears %08x and covers all our unique neighbors",
                     neighborName, heardFrom);
            return true;
        }
    }
    return false;
}

bool SignalRoutingModule::shouldRelayForStockNeighbors(NodeNum myNode, NodeNum sourceNode, NodeNum heardFrom, uint32_t currentTime)
{
    if (!routingGraph) {
        return false;
    }

    // Find stock firmware nodes that might need coverage: direct neighbors + downstream nodes
    NodeNum stockNeighbors[NEIGHBOR_GRAPH_MAX_EDGES_PER_NODE];
    uint8_t stockCount = 0;

    // Check our direct neighbors for stock firmware nodes
    // For active stock nodes (CLIENT, ROUTER, etc.), only consider them if they've confirmed
    // they can hear us by relaying our packets (hearsUs flag on the edge).
    // For mute stock nodes (CLIENT_MUTE, CLIENT_HIDDEN, LOST_AND_FOUND), hearing them is enough.
    const NodeEdges* myEdges = routingGraph->getEdgesFrom(myNode);
    if (myEdges) {
        for (uint8_t i = 0; i < myEdges->edgeCount && stockCount < NEIGHBOR_GRAPH_MAX_EDGES_PER_NODE; i++) {
            NodeNum neighbor = myEdges->edges[i].to;
            if (getCapabilityStatus(neighbor) == CapabilityStatus::Legacy) {
                if (isNonRelayingLegacyRole(neighbor)) {
                    // Mute node: we hear it, so we cover it
                    stockNeighbors[stockCount++] = neighbor;
                } else if (myEdges->edges[i].hearsUs) {
                    // Active stock node: only cover if it has relayed our packets (bidirectional)
                    stockNeighbors[stockCount++] = neighbor;
                } else {
                    LOG_INFO("[SR] Skipping active stock neighbor %08x — no confirmed bidirectional link (hearsUs=false)",
                             neighbor);
                }
            }
        }
    }

    if (stockCount == 0) {
        return false; // No stock neighbors to worry about
    }

    LOG_INFO("[SR] Checking broadcast coverage for %u stock neighbors", stockCount);

    // Check if any stock neighbor needs this packet
    // A stock neighbor needs the packet if they didn't hear it directly from the source
    bool hasUncoveredStockNeighbor = false;
    NodeNum bestStockNeighbor = 0;
    float bestStockCost = std::numeric_limits<float>::max();

    for (uint8_t si = 0; si < stockCount; si++) {
        NodeNum stockNeighbor = stockNeighbors[si];
        // Check if stock neighbor heard the transmission directly
        // If source can reach stock neighbor directly, they already heard it
        // Also check if heardFrom (relaying SR node) can reach them directly
        bool heardDirectly = false;

        // If the stock neighbor IS the relay node, it obviously heard the packet
        if (stockNeighbor == heardFrom || stockNeighbor == sourceNode) {
            heardDirectly = true;
        }

        // Check if original source can reach stock neighbor
        if (!heardDirectly) {
            const NodeEdges* sourceEdges = routingGraph->getEdgesFrom(sourceNode);
            if (sourceEdges) {
                for (uint8_t i = 0; i < sourceEdges->edgeCount; i++) {
                    if (sourceEdges->edges[i].to == stockNeighbor) {
                        heardDirectly = true;
                        break;
                    }
                }
            }
        }

        // If not heard from source, check if heard from relaying SR node
        if (!heardDirectly) {
            const NodeEdges* heardFromEdges = routingGraph->getEdgesFrom(heardFrom);
            if (heardFromEdges) {
                for (uint8_t i = 0; i < heardFromEdges->edgeCount; i++) {
                    if (heardFromEdges->edges[i].to == stockNeighbor) {
                        heardDirectly = true;
                        LOG_INFO("[SR] Stock neighbor %08x already covered by relaying SR node %08x",
                                 stockNeighbor, heardFrom);
                        break;
                    }
                }
            }
        }

        if (!heardDirectly) {
            hasUncoveredStockNeighbor = true;
            LOG_INFO("[SR] Stock neighbor %08x did not hear transmission directly", stockNeighbor);

            // Check if we're the best positioned to reach this stock neighbor
            const NodeEdges* myEdges = routingGraph->getEdgesFrom(myNode);
            if (myEdges) {
                for (uint8_t i = 0; i < myEdges->edgeCount; i++) {
                    if (myEdges->edges[i].to == stockNeighbor) {
                        float cost = myEdges->edges[i].getEtx();
                        if (cost < bestStockCost) {
                            bestStockCost = cost;
                            bestStockNeighbor = stockNeighbor;
                        }
                        break;
                    }
                }
            }
        }
    }

    if (hasUncoveredStockNeighbor && bestStockNeighbor != 0) {
        LOG_INFO("[SR] STOCK COVERAGE: Relaying broadcast for uncovered stock neighbor %08x (ETX=%.2f)",
                 bestStockNeighbor, bestStockCost);
        return true;
    }

    if (hasUncoveredStockNeighbor) {
        LOG_INFO("[SR] STOCK COVERAGE: Found %u uncovered stock neighbors but no valid relay path from this node", stockCount);
    }

    return false;
}



void SignalRoutingModule::logNetworkTopology()
{
#ifdef DEBUG_MUTE
    return;
#else
    if (!routingGraph) return;

    // Use fixed-size arrays only, no heap allocations
    NodeNum nodeBuf[NEIGHBOR_GRAPH_MAX_NEIGHBORS];
    size_t rawNodeCount = routingGraph->getAllNodeIds(nodeBuf, NEIGHBOR_GRAPH_MAX_NEIGHBORS);

    // For passive nodes: only show nodes that have edges (direct neighbors)
    // For active nodes: show all nodes in graph
    size_t nodeCount = rawNodeCount;
    if (!isActiveRoutingRole()) {
        // Filter to only nodes with edges
        size_t filteredCount = 0;
        for (size_t i = 0; i < rawNodeCount; i++) {
            const NodeEdges* edges = routingGraph->getEdgesFrom(nodeBuf[i]);
            if (edges && edges->edgeCount > 0) {
                nodeBuf[filteredCount++] = nodeBuf[i];
            }
        }
        nodeCount = filteredCount;
    }

    if (nodeCount == 0) {
        LOG_INFO("[SR] Network Topology: No nodes in graph yet");
        return;
    }

    NodeNum ourNode = nodeDB->getNodeNum();
    // Single shared name buffer to minimize stack usage in nested loops
    char nameBuf[80];
    getNodeDisplayName(ourNode, nameBuf, sizeof(nameBuf));

    // Get our direct edges
    const NodeEdges* ourEdges = routingGraph->getEdgesFrom(ourNode);
    uint8_t directCount = ourEdges ? ourEdges->edgeCount : 0;

    LOG_INFO("[SR] Network Topology: %d nodes, %u direct neighbors", nodeCount, directCount);
    LOG_INFO("[SR] %s (us)", nameBuf);

    // Per-neighbor / per-downstream tree print removed to save flash on the largest
    // size-constrained variants. Use ROUTING_APP traceroute / SR_ROUTING_DEBUG.md for
    // detailed topology inspection.
    (void)ourEdges;
#endif // !DEBUG_MUTE
}

ProcessMessage SignalRoutingModule::handleReceived(const meshtastic_MeshPacket &mp)
{
    // Sanity check: reject packets with obviously corrupted payload sizes
    // Max valid payload is ~237 bytes for LoRa; anything over 256 is definitely garbage
    if (mp.which_payload_variant == meshtastic_MeshPacket_decoded_tag &&
        mp.decoded.payload.size > meshtastic_Constants_DATA_PAYLOAD_LEN) {
        LOG_WARN("[SR] Reject pkt: bad payload %u>%u",
                 mp.decoded.payload.size, meshtastic_Constants_DATA_PAYLOAD_LEN);
        return ProcessMessage::CONTINUE;
    }

    // Update node activity for packet reception and relay tracking
    // For active nodes: track all packets (needed for full topology and routing)
    // For passive nodes: only track direct packets (only need direct neighbors)
    // We'll check this after determining if packet is direct (below)
    bool shouldUpdateNodeActivity = false;


    // Only track DIRECT neighbors - packets heard directly over radio with no relays
    // Conditions for a direct neighbor:
    // 1. Has valid signal data (rx_rssi or rx_snr)
    // 2. Not received via MQTT
    // 3. Direct detection using hopStart and hopLimit:
    //    - hopStart == hopLimit: packet hasn't been relayed (direct transmission)
    //    - In SR, we keep hopLimit >= 1 even for passive nodes, so direct means no decrement from original
    //    
    // relay_node can be ambiguous when multiple nodes share the same last byte,
    // so hopStart/hopLimit is more reliable for detecting direct neighbors.
    
    bool hasSignalData = (mp.rx_rssi != 0 || mp.rx_snr != 0);
    bool notViaMqtt = !mp.via_mqtt;
    
    bool isDirectFromSender = isDirectPacket(mp);
    uint8_t fromLastByte = mp.from & 0xFF;

    // (Per-packet RX log removed to save flash; reception is already visible at the
    // upstream Router level via "Lora RX" debug lines.)
    
    // Update node activity only when appropriate:
    // - Active nodes: all packets (need full topology)
    // - Passive nodes: only direct packets (only need direct neighbors)
    if (shouldUpdateNodeActivity || isActiveRoutingRole() || (hasSignalData && notViaMqtt && isDirectFromSender)) {
        updateNodeActivityForPacketAndRelay(&mp);
    }
    
    // Update SignalRouting graph for directly-heard nodes
    // Active routing nodes: track all nodes for topology-based routing
    // Passive nodes: only track directly-heard nodes (don't relay, so no point in tracking relayed nodes)
    if (routingGraph && notViaMqtt) {
        if (hasSignalData && isDirectFromSender) {
            // Direct reception - add to graph and clear any stale downstream relationship
            updateNeighborInfo(mp.from, mp.rx_rssi, mp.rx_snr, mp.rx_time);
            routingGraph->clearDownstreamForDestination(mp.from);
        } else if (isActiveRoutingRole() && !isDirectFromSender && mp.relay_node != 0) {
            // Relayed packet from active routing node - update activity for topology
            routingGraph->updateNodeActivity(mp.from, millis() / 1000);
        }
        // Passive nodes skip relayed packets entirely
    }

    if (hasSignalData && notViaMqtt && isDirectFromSender) {
        // Check if this sender matches a known placeholder
        NodeNum placeholderId = getPlaceholderForRelay(fromLastByte);
        if (isPlaceholderNode(placeholderId)) {
            // This sender matches a placeholder - resolve it
            if (resolvePlaceholder(placeholderId, mp.from)) {
                LOG_INFO("[SR] Direct: resolved %08x -> %08x", placeholderId, mp.from);
            }
        }

        rememberRelayIdentity(mp.from, fromLastByte);
        trackNodeCapability(mp.from, CapabilityStatus::Unknown);

        char senderName[64];
        getNodeDisplayName(mp.from, senderName, sizeof(senderName));

        float etx =
            NeighborGraph::calculateETX(mp.rx_rssi, mp.rx_snr);

        // When a node is confirmed as a direct neighbor, clear any downstream entries that
        // listed it as a destination reachable via some other relay — those are now obsolete.
        // Note: clearDownstreamForDestination (what we want here) is different from
        // clearDownstreamForRelay (which would incorrectly remove MB9c as a relay for others).
        if (routingGraph) {
            routingGraph->clearDownstreamForDestination(mp.from);
        }

        LOG_INFO("[SR] Direct neighbor %s: RSSI=%d, SNR=%.1f, ETX=%.2f",
                 senderName, mp.rx_rssi, mp.rx_snr, etx);

        // Record that this node transmitted (for contention window tracking)
        if (routingGraph) {
            uint32_t currentTime = millis() / 1000;  // Use monotonic time
            routingGraph->recordNodeTransmission(mp.from, mp.id, currentTime);
        }
    } else if (notViaMqtt && !isDirectFromSender && mp.relay_node != 0) {
        // Process relayed packets to infer network topology (skip for inactive roles - they only track direct neighbors)
        if (!isActiveRoutingRole()) {
            LOG_INFO("[SR] Inactive: skip topo inf");
        } else {
            NodeNum inferredRelayer = resolveRelayIdentity(mp.relay_node, mp.rx_rssi, mp.rx_snr);

        // If still not resolved, try known nodes (both direct neighbors and topology-known nodes)
        // We need to check ALL edges, not just Reported ones, because the relay might be
        // a node we only know through topology broadcasts (Mirrored edges)
        if (inferredRelayer == 0 && routingGraph && nodeDB) {
            const NodeEdges* myEdges = routingGraph->getEdgesFrom(nodeDB->getNodeNum());
            if (myEdges) {
                for (uint8_t i = 0; i < myEdges->edgeCount; i++) {
                    NodeNum neighbor = myEdges->edges[i].to;
                    if ((neighbor & 0xFF) == mp.relay_node && !isPlaceholderNode(neighbor)) {
                        inferredRelayer = neighbor;
                        // Remember this mapping for future use
                        rememberRelayIdentity(neighbor, mp.relay_node);
                        LOG_INFO("[SR] Resolved relay 0x%02x to known node %08x",
                                 mp.relay_node, neighbor);
                        break;
                    }
                }
            }
        }

        // If we still can't resolve the relay identity, create a placeholder node
        if (inferredRelayer == 0) {
            inferredRelayer = createPlaceholderNode(mp.relay_node);
            // Only log on first encounter — placeholder IDs are deterministic so
            // the same ID is returned on every unresolved packet for this relay byte.
            if (!routingGraph->getEdgesFrom(inferredRelayer)) {
                LOG_INFO("[SR] Placeholder %08x for relay 0x%02x",
                         inferredRelayer, mp.relay_node);
            }
        }

        if (inferredRelayer != 0 && inferredRelayer != mp.from) {
            // Remember this relay identity mapping for future use (only for real nodes, not placeholders)
            if (!isPlaceholderNode(inferredRelayer)) {
                rememberRelayIdentity(inferredRelayer, mp.relay_node);
            }

            // We know that inferredRelayer relayed a packet from mp.from
            // This establishes both a gateway relationship and direct connectivity inference
            LOG_INFO("[SR] Inferred gateway relationship: %08x relayed by %08x",
                     mp.from, inferredRelayer);

            // Track that both the original sender and relayer are active
            trackNodeCapability(mp.from, CapabilityStatus::Unknown);
            trackNodeCapability(inferredRelayer, CapabilityStatus::Unknown);

            // Relay node is actively participating, tracked via SR capability system

            // Record gateway relationship: inferredRelayer is gateway for mp.from
            // But only if:
            // 1. We have a direct connection to inferredRelayer (can hear them directly)
            // 2. We don't have a direct connection to mp.from ourselves
            // 3. We haven't already seen this source recently via a different relay
            //    (prevents inflated downstream counts when SR cluster nodes re-relay the same traffic)
            bool hasDirectConnectionToRelay = false;
            const NodeEdges* edges = routingGraph->getEdgesFrom(nodeDB->getNodeNum());
            if (edges) {
                for (uint8_t i = 0; i < edges->edgeCount; i++) {
                    if (edges->edges[i].to == inferredRelayer &&
                        edges->edges[i].source == Edge::Source::Reported) {
                        hasDirectConnectionToRelay = true;
                        break;
                    }
                }
            }

            // Infer downstream relationship based on hop count and source capability:
            // - Single-hop (hop_start - hop_limit == 1): always infer — sender went directly through
            //   inferredRelayer to reach us, so sender is definitively downstream of that relay.
            // - Multi-hop, non-SR-aware source (Unknown/Legacy): also infer — stock nodes never
            //   advertise their own topology, so relay observation is the only signal we have.
            //   Even if inferredRelayer doesn't hear the sender directly (some intermediate relay
            //   exists), from piko's routing perspective the path still goes through inferredRelayer.
            // - Multi-hop, SR-aware source: skip — the source broadcasts its own topology, which
            //   captures relationships more accurately than hop-count inference.
            bool singleHopRelay = (mp.hop_start - mp.hop_limit) == 1;
            CapabilityStatus sourceStatus = getCapabilityStatus(mp.from);
            bool sourceIsSRAware = (sourceStatus == CapabilityStatus::SRactive ||
                                    sourceStatus == CapabilityStatus::Passive);
            if (hasDirectConnectionToRelay && (singleHopRelay || !sourceIsSRAware)) {
                float inferredEtx = NeighborGraph::calculateETX(-70, 5.0f); // Default for inferred
                routingGraph->updateDownstreamExclusive(mp.from, inferredRelayer, inferredEtx, millis() / 1000);
                if (!singleHopRelay) {
                    LOG_INFO("[SR] Multi-hop ds: %08x via %08x (%d hops)",
                             mp.from, inferredRelayer, mp.hop_start - mp.hop_limit);
                }
            } else if (hasDirectConnectionToRelay && !singleHopRelay) {
                LOG_INFO("[SR] Skip downstream inf %08x via %08x (%d hops, SR src)",
                         mp.from, inferredRelayer, mp.hop_start - mp.hop_limit);
            }

            // Infer directed connectivity from relayer to sender when the relayer is a stock node.
            // SR-aware nodes broadcast their topology, so we don't need to infer connectivity for them.
            // Observing a relay proves only one direction: relayer → sender. The reverse is not assumed.
            bool relayerIsLegacy = getCapabilityStatus(inferredRelayer) == CapabilityStatus::Legacy;
            if (relayerIsLegacy) {
                // Since the stock relayer successfully relayed a packet from the sender,
                // we know the relayer can hear the sender (inferredRelayer → mp.from).
                LOG_INFO("[SR] Inf conn: %08x hears %08x (relay)",
                         inferredRelayer, mp.from);

                uint32_t monotonicTimestamp = millis() / 1000;
                int32_t defaultRssi = -70; // default RSSI for inferred connectivity
                float defaultSnr = 5.0f;  // default SNR for inferred connectivity

                routingGraph->updateEdge(inferredRelayer, mp.from, NeighborGraph::calculateETX(defaultRssi, defaultSnr),
                                         monotonicTimestamp, Edge::Source::Mirrored);
            } else {
                LOG_INFO("[SR] Skip conn inf relayer %08x not Legacy (st=%d)",
                         inferredRelayer, (int)getCapabilityStatus(inferredRelayer));
            }

            // Update relay node's edge in the graph since it's actively relaying
            if (hasSignalData) {
                updateNeighborInfo(inferredRelayer, mp.rx_rssi, mp.rx_snr, mp.rx_time);
            } else {
                // No direct signal data available - preserve existing edge or create with defaults
                const NodeEdges *relayEdges = routingGraph->getEdgesFrom(inferredRelayer);
                bool hasExistingEdge = false;
                int32_t existingRssi = -70; // default
                int32_t existingSnr = 5;   // default

                if (relayEdges) {
                    for (uint8_t i = 0; i < relayEdges->edgeCount; i++) {
                        if (relayEdges->edges[i].to == nodeDB->getNodeNum()) {
                            // Found existing edge - preserve its signal data by recalculating backwards
                            float existingEtx = relayEdges->edges[i].getEtx();
                            int32_t approxRssi;
                            NeighborGraph::etxToSignal(existingEtx, approxRssi, existingSnr);
                            existingRssi = approxRssi;
                            hasExistingEdge = true;
                            break;
                        }
                    }
                }

                // (Preserve/Default signal-data log removed; the side effect is the only thing that matters)
                updateNeighborInfo(inferredRelayer, existingRssi, existingSnr, mp.rx_time);
            }

            // Record transmission for contention window tracking
            if (routingGraph) {
                uint32_t currentTime = millis() / 1000;  // Use monotonic time
                routingGraph->recordNodeTransmission(mp.from, mp.id, currentTime);
                routingGraph->recordNodeTransmission(inferredRelayer, mp.id, currentTime);

                // If this node relayed a packet we originated or previously relayed,
                // it can hear us. Mark the edge as bidirectional (hearsUs) for coverage decisions.
                NodeNum ourNode = nodeDB ? nodeDB->getNodeNum() : 0;
                if (ourNode != 0 && inferredRelayer != 0 && inferredRelayer != ourNode &&
                    (mp.from == ourNode || isCommittedRelay(mp.id))) {
                    markStockNodeRelayedOurPacket(inferredRelayer);
                }
            }
        }  // End of else block for active routing roles relayed packet processing
    }
    }

    if (mp.which_payload_variant == meshtastic_MeshPacket_decoded_tag) {
        handleSniffedPayload(mp, isDirectFromSender);
    }

    // Periodic graph maintenance with stability safeguards (CLIENT_MUTE maintains direct neighbors, others do full maintenance)
    if (routingGraph && canSendTopology()) {
        // Use monotonic time (seconds since boot) for aging to avoid RTC sync issues
        uint32_t currentTime = millis() / 1000;
        if (currentTime - lastGraphUpdate > GRAPH_MAINTENANCE_INTERVAL_SECS) {
            uint32_t nodeCountBefore = routingGraph->getNodeCount();
            
            // Single TTL for all nodes in the graph; SR capability expiry handles coverage separately
            uint8_t directBefore = routingGraph->countDirectNeighbors();
            routingGraph->ageEdges(currentTime, cfgNodeTtlSecs);

            uint32_t nodeCountAfter = routingGraph->getNodeCount();
            lastGraphUpdate = currentTime;

            if (nodeCountBefore != nodeCountAfter) {
                LOG_INFO("[SR] Graph aged: %u -> %u nodes", nodeCountBefore, nodeCountAfter);
                uint8_t directAfter = routingGraph->countDirectNeighbors();
                if (directAfter < directBefore) {
                    LOG_INFO("[SR] Direct lost in aging - topo dirty");
                    markTopologyDirty();
                }
            } else {
                LOG_INFO("[SR] Graph aged (no node count change)");
            }

            // Safety check: ensure we still have our own node
            if (!routingGraph->getEdgesFrom(nodeDB->getNodeNum())) {
                LOG_WARN("[SR] Aging removed local edges");
            }
        }
    }

    return ProcessMessage::CONTINUE;
}

bool SignalRoutingModule::shouldRelayUnicastForCoordination(const meshtastic_MeshPacket *p)
{
    if (!routingGraph || !nodeDB) {
        return false;
    }

    NodeNum myNode = nodeDB->getNodeNum();
    NodeNum destination = p->to;
    NodeNum sourceNode = p->from;
    NodeNum heardFrom = resolveHeardFrom(p, sourceNode);
    uint32_t currentTime = millis() / 1000;

    char destName[64], heardFromName[64], srcName[64];
    getNodeDisplayName(destination, destName, sizeof(destName));
    getNodeDisplayName(heardFrom, heardFromName, sizeof(heardFromName));
    getNodeDisplayName(sourceNode, srcName, sizeof(srcName));

    // If src and dst are both downstream of the same relay, that relay handles delivery.
    NodeNum sourceRelay = routingGraph->getDownstreamRelay(sourceNode);
    NodeNum destRelay = routingGraph->getDownstreamRelay(destination);
    if (sourceRelay != 0 && sourceRelay == destRelay && sourceRelay != myNode) {
        char relayName[64];
        getNodeDisplayName(sourceRelay, relayName, sizeof(relayName));
        LOG_INFO("[SR-DECISION] UNICAST SUPPRESS pkt=0x%08x: from %s to %s, src and dst both downstream of %s", p->id, srcName, destName, relayName);
        return false;
    }

    // If heardFrom already has a route to the destination, it can deliver — don't relay back.
    if (heardFrom != 0 && heardFrom != myNode && heardFrom != sourceNode) {
        bool heardFromCanReachDest = hasDirectConnectivity(heardFrom, destination) ||
                                     (routingGraph->isDownstream(destination) &&
                                      routingGraph->getDownstreamRelay(destination) == heardFrom);
        if (heardFromCanReachDest) {
            LOG_INFO("[SR-DECISION] UNICAST SUPPRESS pkt=0x%08x: from %s to %s, heardFrom %s can reach dst directly", p->id, srcName, destName, heardFromName);
            return false;
        }
        if (hasBetterPositionedSRNeighbor(myNode, heardFrom, destination)) {
            LOG_INFO("[SR-DECISION] UNICAST SUPPRESS pkt=0x%08x: from %s to %s, SR neighbor covering %s can reach dst", p->id, srcName, destName, heardFromName);
            return false;
        }
    }

    // No route → can't relay.
    NodeNum myNextHop = getNextHop(destination, sourceNode, heardFrom, false);
    if (myNextHop == 0) {
        LOG_INFO("[SR-DECISION] UNICAST SUPPRESS pkt=0x%08x: from %s to %s, no route via SR topology", p->id, srcName, destName);
        return false;
    }

    // --- Slot-based relay coordination ---
    //
    // If p->next_hop is set, slot 0 is reserved for the designated next_hop node.
    // SR candidates (self + SR-active direct neighbors that can reach destination)
    // are sorted ascending by cost-to-destination and assigned subsequent slots:
    //   - Direct edge to destination            → etxFixed (range ~100–32767)
    //   - Edge to the shared next hop (proxy)   → etxFixed | 0x8000 (always after direct)
    //   - No usable path                        → skip
    //
    // Each node independently picks the same ordering; the one assigned our slot sets
    // pendingRelayDelayMs and returns true.  Any dupe cancels our queued relay.

    uint32_t halfAirtime = 150;
    if (router && router->getRadioInterface()) {
        uint32_t airtime = router->getRadioInterface()->getPacketTime(p);
        halfAirtime = std::max(airtime / 2, (uint32_t)50);
    }

    // Returns the candidate's cost to reach destination.
    // Direct edge: raw etxFixed. Indirect via shared next hop: etxFixed | 0x8000.
    auto getCandidateCost = [&](NodeNum node) -> uint16_t {
        const NodeEdges *edges = routingGraph->getEdgesFrom(node);
        if (!edges) {
            return UINT16_MAX;
        }
        for (uint8_t i = 0; i < edges->edgeCount; i++) {
            if (edges->edges[i].to == destination) {
                return edges->edges[i].etxFixed;
            }
        }
        if (myNextHop != destination) {
            for (uint8_t i = 0; i < edges->edgeCount; i++) {
                if (edges->edges[i].to == myNextHop) {
                    return edges->edges[i].etxFixed | 0x8000u;
                }
            }
        }
        return UINT16_MAX;
    };

    const NodeEdges *myEdges = routingGraph->getEdgesFrom(myNode);

    uint32_t slotDelay = 0;
    bool shouldRelay = false;
    uint32_t myDelay = 0;

    // Phase 1: If the packet has a designated next_hop, slot 0 belongs to it.
    // If we ARE the designated next_hop (SR node), relay immediately at slot 0.
    // Otherwise reserve slot 0 with a width matching the next_hop's expected timing:
    //   - SR next_hop: halfAirtime (they relay at 0ms, dupe suppression handles us)
    //   - Stock next_hop: worst-case stock CLIENT delay, so we don't fire before they do
    if (p->next_hop != NO_NEXT_HOP_PREFERENCE) {
        uint8_t ourLastByte = nodeDB->getLastByteOfNodeNum(myNode);
        if (ourLastByte == p->next_hop) {
            LOG_INFO("[SR-DECISION] UNICAST RELAY pkt=0x%08x: from %s to %s, we are designated next_hop (slot 0)", p->id, srcName, destName);
            pendingRelayDelayMs = 0;
            routingGraph->recordNodeTransmission(myNode, p->id, currentTime);
            return true;
        }

        // Determine slot 0 width:
        //   - SR next_hop: halfAirtime — they fire at 0ms, dupe suppression handles the rest
        //   - Stock/unknown next_hop: worst-case stock CLIENT delay so we don't fire before them
        //     Stock CLIENT uses (2*CWmax + 2^CWsize) * slotTimeMsec — call getTxDelayMsecWeightedWorst
        NodeNum nextHopNode = resolveRelayIdentity(p->next_hop);
        bool nextHopIsSR = (nextHopNode != 0 && getCapabilityStatus(nextHopNode) == CapabilityStatus::SRactive);
        if (nextHopIsSR) {
            slotDelay += halfAirtime;
            LOG_INFO("[SR] Unicast slot 0ms: SR next_hop %08x (halfAirtime=%ums reserved)", nextHopNode, halfAirtime);
        } else {
            uint32_t stockWorstCase = halfAirtime; // fallback if no radio interface
            if (router && router->getRadioInterface()) {
                stockWorstCase = router->getRadioInterface()->getTxDelayMsecWeightedWorst(p->rx_snr);
            }
            slotDelay += stockWorstCase;
            LOG_INFO("[SR] Unicast slot 0ms: stock/unknown next_hop 0x%02x (stockWorstCase=%ums reserved)", p->next_hop, stockWorstCase);
        }
    }

    // Phase 2: SR candidates (including self) sorted by cost to destination.
    struct UnicastCandidate { NodeNum nodeId; uint16_t cost; };
    UnicastCandidate srCandidates[8];
    uint8_t srCount = 0;

    uint16_t myCost = getCandidateCost(myNode);

    // When getNextHop() returns myNode, the route exists but has no coordinated SR next hop.
    // getCandidateCost() returns UINT16_MAX in this case (no self-loop edge), so use a
    // best-effort sentinel cost (0xFFFE) to add self as the sole relay candidate.
    if (myCost == UINT16_MAX && myNextHop == myNode) {
        myCost = 0xFFFEu;
        LOG_INFO("[SR] Best-effort self relay: no coordinated next hop, relaying as sole candidate");
    }

    if (myCost != UINT16_MAX && srCount < 8) {
        srCandidates[srCount++] = {myNode, myCost};
    }

    if (myEdges) {
        for (uint8_t i = 0; i < myEdges->edgeCount; i++) {
            NodeNum nb = myEdges->edges[i].to;
            if (nb == heardFrom || nb == sourceNode) {
                continue;
            }
            if (isLegacyRouter(nb)) {
                LOG_INFO("[SR] Unicast candidate %08x skipped (legacy router)", nb);
                continue;
            }
            if (getCapabilityStatus(nb) != CapabilityStatus::SRactive) {
                LOG_INFO("[SR] Unicast candidate %08x skipped (not SR-active)", nb);
                continue;
            }
            uint16_t cost = getCandidateCost(nb);
            if (cost != UINT16_MAX && srCount < 8) {
                srCandidates[srCount++] = {nb, cost};
            }
        }
    }

    // Insertion sort ascending by cost; tiebreak by node ID (packet ID parity: even→lowest wins, odd→highest wins).
    // This mirrors the broadcast scheduler tiebreak so all nodes compute an identical ordering.
    bool preferHighNodeId = (p->id & 1) != 0;
    for (uint8_t i = 1; i < srCount; i++) {
        UnicastCandidate key = srCandidates[i];
        int8_t j = (int8_t)i - 1;
        while (j >= 0) {
            bool costHigher = srCandidates[j].cost > key.cost;
            bool costEqual = srCandidates[j].cost == key.cost;
            bool tiebreakLoses = costEqual && (preferHighNodeId ? srCandidates[j].nodeId < key.nodeId
                                                                : srCandidates[j].nodeId > key.nodeId);
            if (!costHigher && !tiebreakLoses) {
                break;
            }
            srCandidates[j + 1] = srCandidates[j];
            j--;
        }
        srCandidates[j + 1] = key;
    }

    // ETX-weighted slot delay.
    //
    // Each candidate's delay = slotDelay (Phase 1 reservation) + scaledGap + jitter, where
    // scaledGap stretches with the candidate's cost gap vs. the best candidate. Lower-cost
    // relays fire sooner; higher-cost relays defer long enough that a better candidate's
    // transmission would dupe-cancel them via perhapsCancelDupe(). This degrades gracefully
    // when local SR graphs disagree about ordering — each node picks delay from its own
    // edge cost without needing a global view.
    //
    // etxFixed is ETX*100 (so 100 units == 1 ETX). 1 ETX of gap == 1 halfAirtime of extra delay.
    const uint16_t bestCost = (srCount > 0) ? (srCandidates[0].cost & 0x7FFFu) : 0;
    // Deterministic per-packet jitter, ±halfAirtime/4, breaks ties without global coordination.
    const uint32_t jitterRange = std::max(halfAirtime / 2, (uint32_t)20);
    const int32_t jitter = (int32_t)(((uint32_t)(myNode ^ p->id)) % jitterRange) - (int32_t)(jitterRange / 2);
    const uint32_t MAX_UNICAST_RELAY_HOLD_MS = 2000;

    LOG_INFO("[SR] Unicast slot sched 0x%08x to %s: ht=%ums, %u cands, jit=%dms",
              p->id, destName, halfAirtime, srCount, jitter);

    for (uint8_t i = 0; i < srCount; i++) {
        NodeNum candidate = srCandidates[i].nodeId;
        if (routingGraph->hasNodeTransmitted(candidate, p->id, currentTime)) {
            LOG_INFO("[SR] Unicast slot --: SR node %08x (already transmitted)", candidate);
            continue;
        }
        if (candidate == myNode) {
            // Hop-budget gate: if hop_limit==1 and we have no direct edge to dest, we can't
            // actually deliver — let some other candidate win the slot if possible.
            bool canReachDestDirectly = hasDirectConnectivity(myNode, destination);
            if (p->hop_limit <= 1 && !canReachDestDirectly) {
                LOG_INFO("[SR] Unicast slot --: US (%08x) skipped — hop_limit=%u with no direct edge to dest",
                         myNode, p->hop_limit);
                continue;
            }
            uint16_t myCostEtx = srCandidates[i].cost & 0x7FFFu;
            uint16_t costGap = (myCostEtx > bestCost) ? (myCostEtx - bestCost) : 0;
            uint32_t scaledGap = ((uint32_t)costGap * halfAirtime) / 100u;
            int64_t totalDelay = (int64_t)slotDelay + (int64_t)scaledGap + (int64_t)jitter;
            if (totalDelay < 0) totalDelay = 0;
            if ((uint64_t)totalDelay > MAX_UNICAST_RELAY_HOLD_MS) totalDelay = MAX_UNICAST_RELAY_HOLD_MS;
            shouldRelay = true;
            myDelay = (uint32_t)totalDelay;
            LOG_INFO("[SR] Unicast slot %ums: US gap=%u sgap=%ums", myDelay, costGap, scaledGap);
            break;
        }
        // (other candidates ahead of us: omitted from log to save flash)
    }

    LOG_INFO("[SR-DECISION] UNICAST %s pkt=0x%08x: from %s to %s via %s (delay=%ums)",
             shouldRelay ? "RELAY" : "SUPPRESS", p->id, srcName, destName, heardFromName, shouldRelay ? myDelay : 0u);

    if (shouldRelay) {
        pendingRelayDelayMs = myDelay;
        routingGraph->recordNodeTransmission(myNode, p->id, currentTime);
    }

    return shouldRelay;
}

int8_t SignalRoutingModule::getUnicastHopLimitForDirectNeighbor(const meshtastic_MeshPacket *p)
{
    if (!routingGraph || !nodeDB) {
        return -1;
    }
    if (isBroadcast(p->to)) {
        return -1;
    }

    NodeNum myNode = nodeDB->getNodeNum();
    NodeNum destination = p->to;

    const NodeEdges *myEdges = routingGraph->getEdgesFrom(myNode);
    if (!myEdges) {
        return -1;
    }

    // Destination must be a direct neighbor confirmed to hear us.
    // Track link quality to decide how aggressively to limit hops.
    static constexpr float RELIABLE_ETX_CEILING = 3.0f;

    bool destIsDirectAndHearsUs = false;
    float destEtx = 0;
    for (uint8_t i = 0; i < myEdges->edgeCount; i++) {
        if (myEdges->edges[i].to == destination && myEdges->edges[i].hearsUs) {
            destIsDirectAndHearsUs = true;
            destEtx = myEdges->edges[i].getEtx();
            break;
        }
    }
    if (!destIsDirectAndHearsUs) {
        return -1;
    }

    // Only limit hops if at least one other direct neighbor is not SR-active (stock firmware).
    // SR nodes suppress relays themselves via the slot-based algorithm.
    bool hasStockNeighbor = false;
    for (uint8_t i = 0; i < myEdges->edgeCount; i++) {
        NodeNum nb = myEdges->edges[i].to;
        if (nb == destination) {
            continue;
        }
        if (getCapabilityStatus(nb) != CapabilityStatus::SRactive) {
            hasStockNeighbor = true;
            break;
        }
    }
    if (!hasStockNeighbor) {
        return -1;
    }

    // Good link: zero hops, direct delivery only
    // Marginal link: 1 hop, allow one retry relay if our TX is lost
    return (destEtx < RELIABLE_ETX_CEILING) ? 0 : 1;
}

bool SignalRoutingModule::shouldUseSignalBasedRouting(const meshtastic_MeshPacket *p)
{
    // This function only checks if SR is available and operational.
    // All actual routing decisions are made in shouldRelay().

    if (!p || !signalBasedRoutingEnabled || !routingGraph || !nodeDB) {
        return false;
    }

    // Update SR graph timestamps for any packet we process
    updateNodeActivityForPacketAndRelay(p);

    // Don't use SR for packets addressed to us - let them be delivered normally
    if (!isBroadcast(p->to) && p->to == nodeDB->getNodeNum()) {
        return false;
    }

    // For broadcasts: check if we have enough SR neighbors
    if (isBroadcast(p->to)) {
        // Passive roles can still veto relays through shouldRelay
        if (!isActiveRoutingRole()) {
            return true;
        }
        return topologyHealthyForBroadcast();
    }

    // For unicasts: SR is available if we have any neighbors and are active role
    // shouldRelay() will handle unknown destinations and routing decisions
    if (!isActiveRoutingRole()) {
        return false;
    }

    // Use SR for unicasts if we have topology (at least one SR neighbor)
    // This allows shouldRelay() to make informed decisions about routing
    return topologyHealthyForBroadcast();
}

void SignalRoutingModule::commitRelay(PacketId packetId, NodeNum originalHeardFrom, uint32_t txDelayMs)
{
    // Don't add duplicates
    for (uint8_t i = 0; i < committedRelayCount; i++) {
        if (committedRelays[i].packetId == packetId)
            return;
    }
    CommittedRelay entry;
    entry.packetId = packetId;
    entry.originalHeardFrom = originalHeardFrom;
    entry.txDelayMs = txDelayMs;
    if (committedRelayCount < MAX_COMMITTED_RELAYS) {
        committedRelays[committedRelayCount++] = entry;
    } else {
        // Overwrite oldest entry
        memmove(committedRelays, committedRelays + 1, (MAX_COMMITTED_RELAYS - 1) * sizeof(CommittedRelay));
        committedRelays[MAX_COMMITTED_RELAYS - 1] = entry;
    }
    LOG_INFO("[SR] Committed relay for packet 0x%08x (heardFrom 0x%08x, delay %ums)", packetId, originalHeardFrom, txDelayMs);
}

bool SignalRoutingModule::isCommittedRelay(PacketId packetId) const
{
    for (uint8_t i = 0; i < committedRelayCount; i++) {
        if (committedRelays[i].packetId == packetId)
            return true;
    }
    return false;
}

uint32_t SignalRoutingModule::getCommittedRelayDelay(PacketId packetId) const
{
    for (uint8_t i = 0; i < committedRelayCount; i++) {
        if (committedRelays[i].packetId == packetId)
            return committedRelays[i].txDelayMs;
    }
    return 0;
}

void SignalRoutingModule::clearCommittedRelay(PacketId packetId)
{
    for (uint8_t i = 0; i < committedRelayCount; i++) {
        if (committedRelays[i].packetId == packetId) {
            committedRelays[i] = committedRelays[--committedRelayCount];
            committedRelays[committedRelayCount] = CommittedRelay();
            return;
        }
    }
}

bool SignalRoutingModule::hasAnyHearsUsNeighbor() const
{
    if (!routingGraph || !nodeDB) {
        return false;
    }
    const NodeEdges *myEdges = routingGraph->getEdgesFrom(nodeDB->getNodeNum());
    if (!myEdges) {
        return false;
    }
    for (uint8_t i = 0; i < myEdges->edgeCount; i++) {
        if (myEdges->edges[i].hearsUs) {
            return true;
        }
    }
    return false;
}

bool SignalRoutingModule::allHearsUsNeighborsHeardPacket(PacketId packetId) const
{
    if (!routingGraph || !nodeDB) {
        return false;
    }
    NodeNum myNode = nodeDB->getNodeNum();
    const NodeEdges *myEdges = routingGraph->getEdgesFrom(myNode);
    if (!myEdges || myEdges->edgeCount == 0) {
        return false;
    }

    // Find the committed relay entry for this packet
    const CommittedRelay *relay = nullptr;
    for (uint8_t i = 0; i < committedRelayCount; i++) {
        if (committedRelays[i].packetId == packetId) {
            relay = &committedRelays[i];
            break;
        }
    }
    if (!relay) {
        return false; // No committed relay record — can't determine
    }

    // Build the set of known transmitters: originalHeardFrom + heardTransmitters.
    // These are nodes we observed transmitting this packet before our relay.
    NodeNum transmitters[1 + MAX_HEARD_TRANSMITTERS];
    uint8_t txCount = 0;
    if (relay->originalHeardFrom != 0 && relay->originalHeardFrom != myNode) {
        transmitters[txCount++] = relay->originalHeardFrom;
    }
    for (uint8_t t = 0; t < relay->heardTransmitterCount; t++) {
        NodeNum tx = relay->heardTransmitters[t];
        if (tx != relay->originalHeardFrom) {
            transmitters[txCount++] = tx;
        }
    }
    if (txCount == 0) {
        return false; // No known transmitters — can't infer anything
    }

    // Check every hearsUs neighbor: they must have heard the packet from at least one
    // known transmitter. A neighbor N heard the packet if:
    //   (a) N is itself a known transmitter (we directly observed it), OR
    //   (b) A known transmitter T has a link to N in the graph:
    //       - T→N edge: T reports hearing N (link exists, likely bidirectional)
    //       - N→T edge: N reports hearing T (N definitely receives T's broadcasts)
    // This covers both SR nodes (full topology data) and stock nodes (inferred edges).
    uint8_t hearsUsCount = 0;
    for (uint8_t i = 0; i < myEdges->edgeCount; i++) {
        if (!myEdges->edges[i].hearsUs) {
            continue;
        }
        NodeNum neighbor = myEdges->edges[i].to;

        // Placeholder nodes have unknown identity — can't match against transmitters
        if (isPlaceholderNode(neighbor)) {
            continue;
        }
        hearsUsCount++;

        // (a) Is N itself a known transmitter?
        bool heard = false;
        for (uint8_t t = 0; t < txCount; t++) {
            if (transmitters[t] == neighbor) {
                heard = true;
                break;
            }
        }

        // (b) Does any known transmitter T have a link to N in the graph?
        if (!heard) {
            for (uint8_t t = 0; t < txCount && !heard; t++) {
                // T→N: transmitter reports neighbor as its neighbor
                const NodeEdges *txEdges = routingGraph->getEdgesFrom(transmitters[t]);
                if (txEdges) {
                    for (uint8_t j = 0; j < txEdges->edgeCount; j++) {
                        if (txEdges->edges[j].to == neighbor) {
                            heard = true;
                            break;
                        }
                    }
                }
                // N→T: neighbor reports transmitter as its neighbor
                if (!heard) {
                    const NodeEdges *nEdges = routingGraph->getEdgesFrom(neighbor);
                    if (nEdges) {
                        for (uint8_t j = 0; j < nEdges->edgeCount; j++) {
                            if (nEdges->edges[j].to == transmitters[t]) {
                                heard = true;
                                break;
                            }
                        }
                    }
                }
            }
        }

        if (!heard) {
            return false; // This hearsUs neighbor may not have heard the packet
        }
    }

    return hearsUsCount > 0; // At least one hearsUs neighbor, and all accounted for
}

void SignalRoutingModule::maybeScheduleBroadcastRetransmit(const meshtastic_MeshPacket *p)
{
    if (!routingGraph || !nodeDB || !p) {
        return;
    }
    if (!isBroadcast(p->to)) {
        return;
    }
    // SR topology packets use their own interval-based scheduling; skip them
    if (p->which_payload_variant == meshtastic_MeshPacket_decoded_tag &&
        p->decoded.portnum == meshtastic_PortNum_SIGNAL_ROUTING_APP) {
        return;
    }
    // Only schedule for broadcasts we originated or are committed to relay via SR
    NodeNum myNode = nodeDB->getNodeNum();
    bool isOriginated = (p->from == 0 || p->from == myNode);
    bool isSRRelay = !isOriginated && isCommittedRelay(p->id);
    if (!isOriginated && !isSRRelay) {
        return;
    }
    // Guard: when firing T1 we re-enter send(); prevent scheduling T2
    if (isRetransmitting) {
        return;
    }
    // Respect config: T1 retransmit insurance may be disabled
    if (!t1RetransmitEnabled) {
        return;
    }
    // Require at least one confirmed relay neighbor (hearsUs = true)
    if (!hasAnyHearsUsNeighbor()) {
        LOG_DEBUG("[SR] No confirmed relay neighbors — skipping T1 for 0x%08x", p->id);
        return;
    }
    // Skip if this packet already has a slot (active or canceled) — prevents T2
    for (uint8_t i = 0; i < MAX_PENDING_RETRANSMITS; i++) {
        if (pendingRetransmits[i].packetId == p->id) {
            return;
        }
    }
    // T1 delay = worst-case ROUTER_LATE window (SNR=+10 → CWsize=CWmax) + one packet airtime,
    // ensuring every ROUTER_LATE node has had time to fire before we retransmit.
    uint32_t latestRelayWindowMs = 0;
    uint32_t airtimeMs = 0;
    if (router && router->getRadioInterface()) {
        latestRelayWindowMs = router->getRadioInterface()->getTxDelayMsecWeightedWorst(10.0f);
        airtimeMs = router->getRadioInterface()->getPacketTime(p);
    } else {
        latestRelayWindowMs = 10000; // 10 s fallback
        airtimeMs = 500;
    }
    uint32_t fireDelayMs = latestRelayWindowMs + airtimeMs;

    // Allocate the copy before Router::send() encrypts the original in place
    meshtastic_MeshPacket *copy = packetPool.allocCopy(*p);
    if (!copy) {
        LOG_WARN("[SR] Packet pool exhausted — skipping T1 for 0x%08x", p->id);
        return;
    }

    // Find a free slot — prefer canceled/empty entries
    PendingRetransmit *slot = nullptr;
    for (uint8_t i = 0; i < MAX_PENDING_RETRANSMITS; i++) {
        if (pendingRetransmits[i].packetId == 0 || pendingRetransmits[i].canceled) {
            if (pendingRetransmits[i].packet) {
                packetPool.release(pendingRetransmits[i].packet);
            }
            pendingRetransmits[i] = PendingRetransmit();
            slot = &pendingRetransmits[i];
            break;
        }
    }
    if (!slot) {
        // All four slots simultaneously active — extremely rare given MAX_PENDING_RETRANSMITS = 4
        LOG_WARN("[SR] All retransmit slots occupied — dropping T1 for 0x%08x", p->id);
        packetPool.release(copy);
        return;
    }

    slot->packetId = p->id;
    slot->packet = copy;
    slot->fireAfterMs = millis() + fireDelayMs;
    slot->canceled = false;

    LOG_INFO("[SR] T1 scheduled for 0x%08x (%s) — fires in %ums (ROUTER_LATE window %ums + airtime %ums)",
             p->id, isOriginated ? "originated" : "SR-relay", fireDelayMs, latestRelayWindowMs, airtimeMs);

    // Ensure runOnce() wakes up in time to fire the retransmit
    setIntervalFromNow(fireDelayMs);
}

void SignalRoutingModule::cancelBroadcastRetransmit(PacketId packetId)
{
    for (uint8_t i = 0; i < MAX_PENDING_RETRANSMITS; i++) {
        if (pendingRetransmits[i].packetId == packetId && !pendingRetransmits[i].canceled) {
            pendingRetransmits[i].canceled = true;
            if (pendingRetransmits[i].packet) {
                packetPool.release(pendingRetransmits[i].packet);
                pendingRetransmits[i].packet = nullptr;
            }
            LOG_INFO("[SR] T1 canceled for 0x%08x — relay confirmed heard", packetId);
            return;
        }
    }
}

bool SignalRoutingModule::areAllNeighborsCovered(const meshtastic_MeshPacket *p)
{
    if (!routingGraph || !nodeDB || !p) {
        return false; // Can't evaluate — keep our relay
    }

    // Resolve the dupe's relay node
    NodeNum dupeRelayer = 0;
    if (p->relay_node != 0) {
        dupeRelayer = resolveRelayIdentity(p->relay_node, p->rx_rssi, p->rx_snr);
        if (dupeRelayer == 0) {
            // Try matching from our known neighbors
            NodeNum myNode = nodeDB->getNodeNum();
            const NodeEdges *myEdges = routingGraph->getEdgesFrom(myNode);
            if (myEdges) {
                for (uint8_t i = 0; i < myEdges->edgeCount; i++) {
                    if ((myEdges->edges[i].to & 0xFF) == p->relay_node && !isPlaceholderNode(myEdges->edges[i].to)) {
                        dupeRelayer = myEdges->edges[i].to;
                        break;
                    }
                }
            }
        }
    }

    NodeNum myNode = nodeDB->getNodeNum();

    if (dupeRelayer == 0) {
        LOG_INFO("[SR] cov pkt=0x%08x: relay 0x%02x unresolved (keep)", p->id, p->relay_node);
        return false;
    }
    if (dupeRelayer == myNode) {
        LOG_INFO("[SR] cov pkt=0x%08x: own dupe (keep)", p->id);
        return false;
    }

    char relayerName[64];
    getNodeDisplayName(dupeRelayer, relayerName, sizeof(relayerName));

    // Unicast: cancel only if the dupe relayer can actually reach the destination.
    // If it cannot but we can (direct edge or destination is our downstream), keep our relay
    // to ensure delivery — otherwise the message is lost even though we hold the best path.
    if (!isBroadcast(p->to)) {
        char destName[64];
        getNodeDisplayName(p->to, destName, sizeof(destName));

        bool dupeCanReachDest = hasDirectConnectivity(dupeRelayer, p->to) ||
                                 routingGraph->getDownstreamRelay(p->to) == dupeRelayer;
        bool weCanReachDest   = hasDirectConnectivity(myNode, p->to) ||
                                 routingGraph->getDownstreamRelay(p->to) == myNode;

        if (!dupeCanReachDest && weCanReachDest) {
            LOG_INFO("[SR] U-dupe 0x%08x to %s from %s (keep, dupe cant reach)",
                     p->id, destName, relayerName);
            return false;
        }

        LOG_INFO("[SR] U-dupe 0x%08x to %s from %s (cancel)", p->id, destName, relayerName);
        return true;
    }

    // Broadcast: accumulate all transmitters heard for this packet and check whether
    // they collectively cover all our neighbors.
    CommittedRelay *relay = nullptr;
    for (uint8_t i = 0; i < committedRelayCount; i++) {
        if (committedRelays[i].packetId == p->id) {
            relay = &committedRelays[i];
            break;
        }
    }

    NodeNum originalHeardFrom = relay ? relay->originalHeardFrom : 0;

    // Add dupeRelayer to the accumulated set if not already present
    if (relay && dupeRelayer != 0 && dupeRelayer != myNode) {
        bool alreadyTracked = false;
        for (uint8_t i = 0; i < relay->heardTransmitterCount; i++) {
            if (relay->heardTransmitters[i] == dupeRelayer) {
                alreadyTracked = true;
                break;
            }
        }
        if (!alreadyTracked && relay->heardTransmitterCount < MAX_HEARD_TRANSMITTERS) {
            relay->heardTransmitters[relay->heardTransmitterCount++] = dupeRelayer;
            LOG_INFO("[SR] tx accum %s pkt=0x%08x (%u)",
                      p->id, relayerName, relay->heardTransmitterCount);
        } else if (!alreadyTracked) {
            LOG_INFO("[SR] tx tbl full pkt=0x%08x (%u) cant add %s",
                      p->id, relay->heardTransmitterCount, relayerName);
        }
    }

    // Build coveredBy: originalHeardFrom + all accumulated transmitters
    NodeNum coveredBy[1 + MAX_HEARD_TRANSMITTERS];
    uint8_t coveredByCount = 0;
    if (originalHeardFrom != 0 && originalHeardFrom != myNode) {
        coveredBy[coveredByCount++] = originalHeardFrom;
    }
    if (relay) {
        for (uint8_t i = 0; i < relay->heardTransmitterCount; i++) {
            NodeNum t = relay->heardTransmitters[i];
            if (t != originalHeardFrom) {
                coveredBy[coveredByCount++] = t;
            }
        }
    }

    bool unique = routingGraph->hasUniqueCoverage(myNode, coveredBy, coveredByCount);

    if (unique) {
        LOG_INFO("[SR] B-dupe 0x%08x from %s, %u tx incomplete (keep)",
                  p->id, relayerName, coveredByCount);
    } else {
        LOG_INFO("[SR] B-dupe 0x%08x from %s, %u tx covers all (cancel)",
                 p->id, relayerName, coveredByCount);
    }

    return !unique;
}

bool SignalRoutingModule::shouldRelay(const meshtastic_MeshPacket *p)
{
    if (!routingGraph || !nodeDB) {
        return true;  // Default to relay if SR unavailable
    }

    // For broadcasts, use the existing broadcast relay logic
    if (isBroadcast(p->to)) {
        return shouldRelayBroadcast(p);
    }

    // === UNICAST RELAY DECISION ===
    // All unicast routing logic is now consolidated here

    char destName[64], senderName[64];
    getNodeDisplayName(p->to, destName, sizeof(destName));
    getNodeDisplayName(p->from, senderName, sizeof(senderName));
    LOG_INFO("[SR] Considering unicast relay pkt=0x%08x from %s to %s (hop_limit=%d)",
             p->id, senderName, destName, p->hop_limit);

    // Check if destination is reachable through SR topology
    if (!topologyHealthyForUnicast(p->to)) {
        // If the node exists in NodeDB, fall back to broadcast-style relay
        // This handles legacy/stock nodes not in the SR graph
        if (nodeDB->getMeshNode(p->to)) {
            LOG_INFO("[SR-DECISION] UNICAST RELAY pkt=0x%08x: from %s to %s, not routable via SR but known in NodeDB", p->id, senderName, destName);
            return true;
        }
        // If the destination is known as a downstream node in the topology (e.g. reachable via a
        // neighbour's neighbour), fall back to SR relay rather than suppressing. This covers cases
        // where the direct relay chain is missing a Dijkstra edge (destination only appears in
        // downstream table entries whose relay is not our direct neighbour).
        if (routingGraph->isDownstream(p->to)) {
            LOG_INFO("[SR-DECISION] UNICAST RELAY pkt=0x%08x: from %s to %s, not routable via SR but known as downstream", p->id, senderName, destName);
            return true;
        }
        LOG_INFO("[SR-DECISION] UNICAST SUPPRESS pkt=0x%08x: from %s to %s, unknown destination — not in SR graph or NodeDB", p->id, senderName, destName);
        return false;
    }

    NodeNum sourceNode = p->from;

    // Resolve placeholder for the direct sender before the relay decision so connectivity
    // checks have accurate node identity. handleReceived() also does this, but runs after shouldRelay().
    if (isDirectPacket(*p) && p->relay_node != 0) {
        uint8_t relayByte = p->relay_node;
        NodeNum placeholderId = getPlaceholderForRelay(relayByte);
        if (isPlaceholderNode(placeholderId)) {
            if (resolvePlaceholder(placeholderId, sourceNode)) {
                LOG_INFO("[SR] Pre-decision placeholder resolved: %08x -> %08x", placeholderId, sourceNode);
            }
        }
        rememberRelayIdentity(sourceNode, relayByte);
    }

    NodeNum heardFrom = resolveHeardFrom(p, sourceNode);

    // If we heard directly from source, check if next hop already has the packet
    if (heardFrom == sourceNode) {
        NodeNum nextHop = getNextHop(p->to, sourceNode, heardFrom, false);
        if (nextHop != 0 && nextHop != nodeDB->getNodeNum()) {
            // Check if source and next hop are neighbors
            bool nextHopHeardFromSource = false;
            const NodeEdges* sourceEdges = routingGraph->getEdgesFrom(sourceNode);
            if (sourceEdges) {
                for (uint8_t i = 0; i < sourceEdges->edgeCount; i++) {
                    if (sourceEdges->edges[i].to == nextHop) {
                        nextHopHeardFromSource = true;
                        break;
                    }
                }
            }
            if (!nextHopHeardFromSource) {
                const NodeEdges* nextHopEdges = routingGraph->getEdgesFrom(nextHop);
                if (nextHopEdges) {
                    for (uint8_t i = 0; i < nextHopEdges->edgeCount; i++) {
                        if (nextHopEdges->edges[i].to == sourceNode) {
                            nextHopHeardFromSource = true;
                            break;
                        }
                    }
                }
            }
            if (nextHopHeardFromSource) {
                char nextHopName[64];
                getNodeDisplayName(nextHop, nextHopName, sizeof(nextHopName));
                // The designated next-hop is a graph-neighbor of the source, so it probably
                // heard the same transmission we did. But topology inference is not the same
                // as confirmed RF reception: if the next-hop misses this particular frame
                // (asymmetric/marginal link, collision, busy RX), the packet is lost unless
                // someone defers a backup relay. Hand off to coordination with a hint to
                // wait long enough for the next-hop's slot 0 to fire first; if we overhear
                // its retransmission, perhapsCancelDupe() will cancel our queued relay.
                //
                // Suppress outright only when we cannot add value: hop_limit==0 means we
                // can't relay at all, and hop_limit==1 with no direct edge to destination
                // means our one-hop relay can't reach the destination either.
                bool canReachDestDirectly = hasDirectConnectivity(nodeDB->getNodeNum(), p->to);
                if (p->hop_limit == 0 || (p->hop_limit <= 1 && !canReachDestDirectly)) {
                    LOG_INFO("[SR-DECISION] UNICAST SUPPRESS pkt=0x%08x: no headroom (hop_limit=%u)",
                             p->id, p->hop_limit);
                    return false;
                }
                LOG_INFO("[SR-DECISION] UNICAST DEFER pkt=0x%08x via %s (backup)",
                         p->id, nextHopName);
                // Fall through to coordination — it will assign us a non-zero slot.
            }
        }
    }

    // Use the contention-based unicast relay coordination
    return shouldRelayUnicastForCoordination(p);
}

bool SignalRoutingModule::shouldRelayBroadcast(const meshtastic_MeshPacket *p)
{
    if (!routingGraph || !nodeDB) {
        return true;
    }

    // Skip SR processing entirely for packets we sent (detected as rebroadcasts)
    // This prevents processing our own packets that come back to us
    NodeNum ourNode = nodeDB->getNodeNum();
    uint32_t currentTime = millis() / 1000;
    if (routingGraph->hasNodeTransmitted(ourNode, p->id, currentTime)) {
        LOG_INFO("[SR] Skipping relay decision for rebroadcast of our packet %08x", p->id);
        return false; // Don't relay our own rebroadcasted packets
    }

    // Special handling for unicast packets being relayed with SR coordination
    if (!isBroadcast(p->to)) {
        // This is a unicast packet being relayed with SR coordination
        // Relay decision should be based on our ability to reach the destination
        return shouldRelayUnicastForCoordination(p);
    }

    if (!isActiveRoutingRole()) {
        return false;
    }

    // Compute packet received timestamp once for all SignalRouting operations
    uint32_t packetReceivedTimestamp = millis() / 1000;

    // Only access decoded fields if packet is actually decoded
    if (p->which_payload_variant == meshtastic_MeshPacket_decoded_tag &&
        p->decoded.portnum == meshtastic_PortNum_SIGNAL_ROUTING_APP) {
        preProcessSignalRoutingPacket(p, packetReceivedTimestamp);
    }

    // Now check if our topology is healthy for making relay decisions
    // If not healthy, default to relay (conservative behavior)
    if (!topologyHealthyForBroadcast()) {
        return true;
    }

    NodeNum myNode = nodeDB->getNodeNum();
    NodeNum sourceNode = p->from;
    NodeNum heardFrom = resolveHeardFrom(p, sourceNode);

    // Downstream awareness: check if WE are the recorded relay for the source or destination
    NodeNum relayForSource = routingGraph->getDownstreamRelay(sourceNode);
    NodeNum relayForDest = routingGraph->getDownstreamRelay(p->to);
    bool weAreRelayForSource = (relayForSource != 0 && relayForSource == myNode);
    bool weAreRelayForDest = (relayForDest != 0 && relayForDest == myNode);
    size_t downstreamCount = (weAreRelayForSource || weAreRelayForDest) ? routingGraph->getDownstreamCountForRelay(myNode) : 0;

    uint32_t relayDecisionTime = packetReceivedTimestamp; // Use the packet received timestamp computed above

    // === Slot-based relay coordination ===
    // Rank all relay candidates and assign time slots spaced by half the packet
    // airtime.  Each node independently computes the same ordering, picks its
    // own slot, and schedules TX.  If it hears a relay (dupe) before its slot
    // fires, existing dupe suppression cancels the queued TX.

    // Half-airtime slot spacing: long enough for busyRx detection, short enough
    // for fast propagation.
    uint32_t halfAirtime = 150; // safe default
    if (router && router->getRadioInterface()) {
        uint32_t airtime = router->getRadioInterface()->getPacketTime(p);
        halfAirtime = std::max(airtime / 2, (uint32_t)50);
    }

    // Build coverage sets.
    // Only mark heardFrom's neighbors as already-covered if the link is good.
    // Poor-quality links (high ETX) should NOT be pre-covered: if our link to
    // that node is much better, we should still relay and get an earlier slot.
    NodeSet alreadyCovered;
    alreadyCovered.insert(sourceNode);
    alreadyCovered.insert(heardFrom);
    const NodeEdges *heardFromEdges = routingGraph->getEdgesFrom(heardFrom);
    if (heardFromEdges) {
        for (uint8_t i = 0; i < heardFromEdges->edgeCount; i++) {
            float etx = heardFromEdges->edges[i].getEtx();
            NodeNum neighbor = heardFromEdges->edges[i].to;
            if (etx < cfgPoorLinkEtxThreshold) {
                alreadyCovered.insert(neighbor);
            }
        }
    }

    // Build candidates: our direct SR-active neighbors (plus stock routers handled in Phase 1)
    NodeSet candidates;
    const NodeEdges *myEdges = routingGraph->getEdgesFrom(myNode);

    // Log any of our own neighbors excluded from pre-coverage due to poor heardFrom link
    if (myEdges && heardFromEdges) {
        for (uint8_t i = 0; i < myEdges->edgeCount; i++) {
            NodeNum myNeighbor = myEdges->edges[i].to;
            if (alreadyCovered.contains(myNeighbor) || myNeighbor == heardFrom || myNeighbor == sourceNode) {
                continue;
            }
            for (uint8_t j = 0; j < heardFromEdges->edgeCount; j++) {
                if (heardFromEdges->edges[j].to == myNeighbor) {
                    char neighborName[32];
                    getNodeDisplayName(myNeighbor, neighborName, sizeof(neighborName));
                    LOG_INFO("[SR] Pre-coverage: our neighbor %s excluded from heardFrom coverage (ETX=%.1f >= %.1f)",
                              neighborName, heardFromEdges->edges[j].getEtx(), cfgPoorLinkEtxThreshold);
                    break;
                }
            }
        }
    }
    if (myEdges) {
        for (uint8_t i = 0; i < myEdges->edgeCount; i++) {
            NodeNum neighbor = myEdges->edges[i].to;
            if (neighbor == heardFrom || neighbor == sourceNode) {
                continue;
            }
            // Only include SR-active nodes and stock routers (stock routers are handled/removed in Phase 1)
            if (getCapabilityStatus(neighbor) != CapabilityStatus::SRactive && !isImmediateRelayRouter(neighbor)) {
                continue;
            }
            candidates.insert(neighbor);
        }
    }
    // Add ourselves
    candidates.insert(myNode);

    // Also add SR-active co-listeners that can hear heardFrom (for stock gateway case)
    if (myEdges) {
        for (uint8_t i = 0; i < myEdges->edgeCount; i++) {
            NodeNum neighbor = myEdges->edges[i].to;
            if (neighbor == heardFrom || neighbor == sourceNode) {
                continue;
            }
            if (getCapabilityStatus(neighbor) != CapabilityStatus::SRactive) {
                continue;
            }
            const NodeEdges *neighborEdges = routingGraph->getEdgesFrom(neighbor);
            if (neighborEdges) {
                for (uint8_t j = 0; j < neighborEdges->edgeCount; j++) {
                    if (neighborEdges->edges[j].to == heardFrom) {
                        candidates.insert(neighbor);
                        break;
                    }
                }
            }
        }
    }

    bool preferHighNodeId = (p->id & 1) != 0;
    uint32_t slotDelay = 0;
    bool shouldRelay = false;
    uint32_t myDelay = 0;
    const char *decisionReason = "no unique coverage";

    LOG_INFO("[SR] Slot sched 0x%08x ht=%ums cands=%u", p->id, halfAirtime, candidates.count);

    // Phase 1: Assign first slots to stock routers (they transmit regardless)
    if (myEdges) {
        for (uint8_t i = 0; i < myEdges->edgeCount; i++) {
            NodeNum neighbor = myEdges->edges[i].to;
            if (neighbor == heardFrom || neighbor == sourceNode) {
                continue;
            }
            if (!isImmediateRelayRouter(neighbor)) {
                continue;
            }

            // Only reserve a slot if the stock router can hear the transmitter.
            // We infer this from Mirrored edges added when we observe the stock
            // router relay packets from that node.  Without evidence it heard
            // the packet, the slot would be wasted and just delays our own TX.
            const NodeEdges *stockEdges = routingGraph->getEdgesFrom(neighbor);
            bool canHearTransmitter = false;
            if (stockEdges) {
                for (uint8_t j = 0; j < stockEdges->edgeCount; j++) {
                    if (stockEdges->edges[j].to == heardFrom) {
                        canHearTransmitter = true;
                        break;
                    }
                }
            }
            if (!canHearTransmitter) {
                LOG_INFO("[SR] Skip stock %08x (no ev hears %08x)", neighbor, heardFrom);
                candidates.erase(neighbor);
                continue;
            }

            candidates.erase(neighbor);

            if (routingGraph->hasNodeTransmitted(neighbor, p->id, currentTime)) {
                const NodeEdges *ne = routingGraph->getEdgesFrom(neighbor);
                if (ne) {
                    for (uint8_t j = 0; j < ne->edgeCount; j++) {
                        alreadyCovered.insert(ne->edges[j].to);
                    }
                }
                alreadyCovered.insert(neighbor);
                LOG_INFO("[SR] Slot %ums: stock router %08x (already transmitted)", slotDelay, neighbor);
            } else {
                LOG_INFO("[SR] Slot %ums: stock router %08x (expected)", slotDelay, neighbor);
            }

            slotDelay += halfAirtime;
        }
    }

    // Phase 2: Iteratively pick best SR candidate, assign slots
    while (!candidates.empty()) {
        RelayCandidate best = routingGraph->findBestRelayCandidate(candidates, alreadyCovered,
                                                                    currentTime, p->id, preferHighNodeId, sourceNode);
        if (best.nodeId == 0) {
            break;
        }

        candidates.erase(best.nodeId);

        if (routingGraph->hasNodeTransmitted(best.nodeId, p->id, currentTime)) {
            const NodeEdges *ne = routingGraph->getEdgesFrom(best.nodeId);
            if (ne) {
                for (uint8_t j = 0; j < ne->edgeCount; j++) {
                    alreadyCovered.insert(ne->edges[j].to);
                }
            }
            alreadyCovered.insert(best.nodeId);
            LOG_INFO("[SR] Slot --: SR node %08x (already transmitted, coverage absorbed)", best.nodeId);
            continue;
        }

        if (best.nodeId == myNode) {
            shouldRelay = true;
            myDelay = slotDelay;
            decisionReason = "SR slot assignment";
            LOG_INFO("[SR] Slot %ums: US (%08x) — assigned%s", slotDelay, myNode, best.tier > 0 ? " (bidi)" : "");
            break;
        }

        LOG_INFO("[SR] Slot %ums: SR node %08x (coverage=%u, cost=%.2f%s)", slotDelay, best.nodeId,
                  best.coverageCount, best.getAvgCost(), best.tier > 0 ? ", bidi" : "");
        slotDelay += halfAirtime;
    }

    // Phase 3: Force relay if we are the recorded downstream relay for source
    if (!shouldRelay && (weAreRelayForSource || weAreRelayForDest)) {
        NodeNum forcedFor = weAreRelayForSource ? sourceNode : p->to;
        LOG_INFO("[SR-DECISION] BROADCAST RELAY (forced) pkt=0x%08x: we are relay for %08x (downstream=%u)",
                 p->id, forcedFor, static_cast<unsigned int>(downstreamCount));
        shouldRelay = true;
        myDelay = slotDelay;
        decisionReason = "downstream relay override";
    }

    // Phase 5: Check stock coverage needs
    if (!shouldRelay) {
        bool stockCoverageNeeded = shouldRelayForStockNeighbors(myNode, sourceNode, heardFrom, relayDecisionTime);
        if (stockCoverageNeeded) {
            shouldRelay = true;
            myDelay = slotDelay;
            decisionReason = "stock coverage";
        }
    }

    char sourceName[64], heardFromName[64];
    getNodeDisplayName(sourceNode, sourceName, sizeof(sourceName));
    getNodeDisplayName(heardFrom, heardFromName, sizeof(heardFromName));

    LOG_INFO("[SR-DECISION] BROADCAST %s pkt=0x%08x: from %s via %s (%s, delay=%ums)",
             shouldRelay ? "RELAY" : "SUPPRESS", p->id, sourceName, heardFromName,
             decisionReason, myDelay);

    if (shouldRelay) {
        pendingRelayDelayMs = myDelay;
        routingGraph->recordNodeTransmission(myNode, p->id, currentTime);
    }

    return shouldRelay;
}

NodeNum SignalRoutingModule::getNextHop(NodeNum destination, NodeNum sourceNode, NodeNum heardFrom, bool allowOpportunistic)
{
    if (!routingGraph) {
        LOG_WARN("[SR] No graph available for routing");
        return 0;
    }

    uint32_t currentTime = millis() / 1000;  // Use monotonic time for consistency

    char destName[64];
    getNodeDisplayName(destination, destName, sizeof(destName));

    Route route = routingGraph->calculateRoute(destination, currentTime,
                        [this](NodeNum nodeId) { return isNodeRoutable(nodeId); });

    float routeCost = route.getCost();

    if (route.nextHop != 0) {
        char nextHopName[64];
        getNodeDisplayName(route.nextHop, nextHopName, sizeof(nextHopName));

        LOG_INFO("[SR] Route to %s via %s (cost: %.2f)",
                 destName, nextHopName, routeCost);

        if (routeCost > 10.0f) {
            LOG_WARN("[SR] High-cost route to %s (%.2f) - poor link quality expected",
                    destName, routeCost);
        }

        // Verify the next hop can hear the transmitting node (heardFrom). If not, we check
        // whether hearsUs allows us to forward anyway (see below), then fall back to
        // opportunistic routing or best-effort self relay.
        bool nextHopCanHearTransmitter = true;
        bool connectivityUnknown = false;

        if (heardFrom != 0 && route.nextHop != heardFrom) {
            nextHopCanHearTransmitter = hasVerifiedConnectivity(heardFrom, route.nextHop, &connectivityUnknown);

            if (!nextHopCanHearTransmitter) {
                char heardFromName[64];
                getNodeDisplayName(heardFrom, heardFromName, sizeof(heardFromName));

                if (connectivityUnknown) {
                    LOG_INFO("[SR] Route via %s rejected - cannot verify connectivity to %s (stock/unknown node)",
                             nextHopName, heardFromName);
                } else {
                    LOG_INFO("[SR] Route via %s rejected - no connectivity to transmitter %s",
                             nextHopName, heardFromName);
                }
            }
        }

        if (nextHopCanHearTransmitter) {
            // Even if we have a route, check if any neighbor has a significantly better route
            // This ensures unicasts are forwarded to better-positioned nodes
            if (allowOpportunistic && routeCost > 2.0f) { // Only check if our route is not excellent
                NodeNum betterNeighbor = findBetterPositionedNeighbor(destination, sourceNode, heardFrom, routeCost, currentTime);
                if (betterNeighbor != 0) {
                    return betterNeighbor;
                }
            }

            return route.nextHop;
        }
        
        // If the next hop has confirmed it hears us (hearsUs), forwarding to it will work
        // regardless of whether the original sender's connectivity can be verified — we are
        // the one retransmitting.
        NodeNum myNode = nodeDB ? nodeDB->getNodeNum() : 0;
        if (myNode != 0) {
            const NodeEdges *myEdges = routingGraph->getEdgesFrom(myNode);
            if (myEdges) {
                for (uint8_t i = 0; i < myEdges->edgeCount; i++) {
                    if (myEdges->edges[i].to == route.nextHop && myEdges->edges[i].hearsUs) {
                        LOG_INFO("[SR] Route via %s approved despite unverified sender connectivity — hearsUs confirmed",
                                 nextHopName);
                        return route.nextHop;
                    }
                }
            }
        }

        // Next hop can't hear transmitter - try to find alternative through opportunistic routing
        if (allowOpportunistic) {
            NodeNum betterNeighbor = findBetterPositionedNeighbor(destination, sourceNode, heardFrom,
                                                                  std::numeric_limits<float>::infinity(), currentTime);
            if (betterNeighbor != 0) {
                char altName[64];
                getNodeDisplayName(betterNeighbor, altName, sizeof(altName));
                LOG_INFO("[SR] Using alternative next hop %s (can hear transmitter)", altName);
                return betterNeighbor;
            }
        }

        // No usable next hop — signal best-effort self relay by returning our own node ID.
        if (myNode != 0) {
            LOG_INFO("[SR] No verified next hop and no hearsUs — best-effort self relay");
            return myNode;
        }
    }

    // Fallback 1: if we know a relay for this destination, and we have a direct link to it, forward there
    // But only if the relay can hear the transmitter (heardFrom)
    NodeNum relayForDest = routingGraph->getDownstreamRelay(destination);
    if (relayForDest != 0 && nodeDB) {
        // Verify relay can hear transmitter before using it
        bool relayCanHearTransmitter = true;
        bool connectivityUnknown = false;
        if (heardFrom != 0 && relayForDest != heardFrom) {
            relayCanHearTransmitter = hasVerifiedConnectivity(heardFrom, relayForDest, &connectivityUnknown);
        }

        // Only use relay if we can verify connectivity (be conservative with stock nodes)
        if (relayCanHearTransmitter && !connectivityUnknown) {
            const NodeEdges* myEdges = routingGraph->getEdgesFrom(nodeDB->getNodeNum());
            if (myEdges) {
                for (uint8_t i = 0; i < myEdges->edgeCount; i++) {
                    if (myEdges->edges[i].to == relayForDest) {
                        char gwName[64];
                        getNodeDisplayName(relayForDest, gwName, sizeof(gwName));
                        LOG_INFO("[SR] No direct route to %s, but forwarding to relay %s", destName, gwName);
                        return relayForDest;
                    }
                }
            }
        } else {
            char gwName[64], heardFromName[64];
            getNodeDisplayName(relayForDest, gwName, sizeof(gwName));
            getNodeDisplayName(heardFrom, heardFromName, sizeof(heardFromName));
            if (connectivityUnknown) {
                LOG_INFO("[SR] Relay %s connectivity to %s unknown (stock node) - skipping", gwName, heardFromName);
            } else {
                LOG_INFO("[SR] Relay %s cannot hear transmitter %s - skipping", gwName, heardFromName);
            }
        }
    }

    // Fallback 2: opportunistic forward — find neighbor with better position for destination
    // Only do this if opportunistic forwarding is allowed
    if (allowOpportunistic) {
        NodeNum betterNeighbor = findBetterPositionedNeighbor(destination, sourceNode, heardFrom,
                                                            std::numeric_limits<float>::infinity(), currentTime);
        if (betterNeighbor != 0) {
            return betterNeighbor;
        }
    }

    // Fallback 3: Special case for unicast packets to direct neighbors that didn't hear the transmission
    // If we received this as a relayed packet (heardFrom != sourceNode) and destination is our direct neighbor,
    // we should deliver it directly since the destination didn't hear the original transmission
    NodeNum myNode = nodeDB ? nodeDB->getNodeNum() : 0;
    if (routingGraph && nodeDB && myNode != 0 && heardFrom != sourceNode) {
        bool isDirectNeighbor = false;
        float directEtx = 1e9f;

        const NodeEdges* myEdges = routingGraph->getEdgesFrom(myNode);
        if (myEdges) {
            for (uint8_t i = 0; i < myEdges->edgeCount; i++) {
                if (myEdges->edges[i].to == destination) {
                    isDirectNeighbor = true;
                    directEtx = myEdges->edges[i].getEtx();
                    break;
                }
            }
        }

        if (isDirectNeighbor) {
            LOG_INFO("[SR] Delivering unicast to direct neighbor %s (ETX=%.2f) since destination didn't hear transmission",
                     destName, directEtx);
            return destination; // Deliver directly to our neighbor
        }
    }

    // Fallback 4: if we are recorded as the relay for this destination, we can deliver directly
    // This handles true relay scenarios where we have unique connectivity that other SR nodes don't
    if (routingGraph->getDownstreamRelay(destination) == myNode) {
        LOG_INFO("[SR] We are the designated relay for %s - delivering directly", destName);
        // Refresh the downstream entry since we're actively using it
        routingGraph->updateDownstream(destination, myNode, 1.0f, millis() / 1000);
        return destination; // We are the relay, deliver directly
    }

    // Fallback 5: if the destination only has us as a neighbor (effective relay scenario),
    // we should try to deliver directly even without formal relay designation
    // This handles cases like FMC6 where a node only connects through us
    if (routingGraph && nodeDB) {
        const NodeEdges *destEdges = routingGraph->getEdgesFrom(destination);
        if (destEdges && destEdges->edgeCount == 1 && destEdges->edges[0].to == myNode) {
            LOG_INFO("[SR] %s only connects through us (effective relay) - delivering directly", destName);
            // Record ourselves as relay for this destination since we're the only connection
            routingGraph->updateDownstream(destination, myNode, 1.0f, millis() / 1000);
            return destination; // We are the effective relay, deliver directly
        }
    }

    LOG_INFO("[SR] No route found to %s", destName);
    return 0;
}

NodeNum SignalRoutingModule::findBetterPositionedNeighbor(NodeNum destination, NodeNum sourceNode, NodeNum heardFrom,
                                                         float ourRouteCost, uint32_t currentTime) {
    if (!routingGraph || !nodeDB) {
        return 0;
    }

    NodeNum myNode = nodeDB->getNodeNum();
    NodeNum bestNeighbor = 0;
    float bestNeighborRouteCost = ourRouteCost;

    const NodeEdges* myEdges = routingGraph->getEdgesFrom(myNode);
    if (!myEdges) {
        return 0;
    }

    for (uint8_t i = 0; i < myEdges->edgeCount; i++) {
        NodeNum neighbor = myEdges->edges[i].to;

        // Don't forward back to source or heardFrom nodes
        if (neighbor == sourceNode || neighbor == heardFrom) {
            continue;
        }

        // CRITICAL: Only consider neighbors that can hear the transmitting node (heardFrom)
        // If they didn't hear the transmission, they can't relay it
        if (heardFrom != 0) {
            bool connectivityUnknown = false;
            bool canHearTransmitter = hasVerifiedConnectivity(heardFrom, neighbor, &connectivityUnknown);
            if (!canHearTransmitter) {
                // Skip if no connectivity, or if connectivity is unknown (stock node - be conservative)
                continue;
            }
        }

        // Check if this neighbor has a direct connection to the destination
        const NodeEdges* neighborEdges = routingGraph->getEdgesFrom(neighbor);
        if (neighborEdges) {
            for (uint8_t j = 0; j < neighborEdges->edgeCount; j++) {
                if (neighborEdges->edges[j].to == destination) {
                    float directEtx = neighborEdges->edges[j].getEtx();
                    // If neighbor has a direct connection that's significantly better than our route cost,
                    // forward to them. Direct connection ETX should be much better than multi-hop route cost.
                    if (directEtx < ourRouteCost - 1.0f && directEtx < bestNeighborRouteCost) {
                        bestNeighbor = neighbor;
                        bestNeighborRouteCost = directEtx;
                    }
                    break;
                }
            }
        }
    }

    if (bestNeighbor != 0) {
        char nhName[64], destName[64];
        getNodeDisplayName(bestNeighbor, nhName, sizeof(nhName));
        getNodeDisplayName(destination, destName, sizeof(destName));
        LOG_INFO("[SR] Found better positioned neighbor %s for %s (our cost: %.2f, neighbor direct ETX: %.2f)",
                 nhName, destName, ourRouteCost, bestNeighborRouteCost);
    }

    return bestNeighbor;
}

void SignalRoutingModule::updateNeighborInfo(NodeNum nodeId, int32_t rssi, float snr, uint32_t lastRxTime)
{
    if (!routingGraph || !nodeDB) return;

    NodeNum myNode = nodeDB->getNodeNum();

    // IMPORTANT: Use monotonic time (seconds since boot) for edge timestamps, not RTC time
    uint32_t monotonicTimestamp = millis() / 1000;
    (void)lastRxTime;  // Unused - we use monotonic time instead

    // Calculate ETX from the received signal quality
    // The RSSI/SNR describes how well we received from nodeId,
    // which characterizes the nodeId→us transmission quality
    float etx =
        NeighborGraph::calculateETX(rssi, snr);

    // Store edge: nodeId → us (the direction of the transmission we measured)
    // This is used for routing decisions when traffic needs to reach us
    int changeType =
        routingGraph->updateEdge(nodeId, myNode, etx, monotonicTimestamp, Edge::Source::Reported);

    // Also store reverse edge: us → nodeId (assuming approximately symmetric link)
    // Since we directly measured the link quality (even if in the opposite direction),
    // mark this as Reported source, not Mirrored
    routingGraph->updateEdge(myNode, nodeId, etx, monotonicTimestamp, Edge::Source::Reported);

    // If significant change, consider sending an update sooner
    if (changeType != EDGE_NO_CHANGE) {
        char neighborName[64];
        getNodeDisplayName(nodeId, neighborName, sizeof(neighborName));

        if (changeType == EDGE_NEW) {
            // We now have a DIRECT connection to this node - clear any downstream entries
            // that were created based on topology broadcasts before we heard from them directly
            routingGraph->clearDownstreamForDestination(nodeId);

            LOG_INFO("[SR] Topology changed: new neighbor %s (total nodes: %u)", neighborName, static_cast<unsigned int>(routingGraph->getNodeCount()));
            markTopologyDirty();
        } else if (changeType == EDGE_SIGNIFICANT_CHANGE) {
            LOG_INFO("[SR] Topology changed: ETX change for %s (total nodes: %u)", neighborName, static_cast<unsigned int>(routingGraph->getNodeCount()));
            markTopologyDirty();
        }

    }
}

bool SignalRoutingModule::isSignalBasedCapable(NodeNum nodeId) const
{
    if (!nodeDB) {
        return false;
    }
    if (nodeId == nodeDB->getNodeNum()) {
        return isActiveRoutingRole();
    }

    CapabilityStatus status = getCapabilityStatus(nodeId);
    return status == CapabilityStatus::SRactive;
}

void SignalRoutingModule::updateNodeActivityForPacket(NodeNum nodeId)
{
    if (routingGraph) {
        routingGraph->updateNodeActivity(nodeId, millis() / 1000);
    }
}

void SignalRoutingModule::updateNodeActivityForPacketAndRelay(const meshtastic_MeshPacket *p)
{
    if (!routingGraph || !nodeDB) return;

    uint32_t currentTime = millis() / 1000;
    NodeNum ourNodeId = nodeDB->getNodeNum();

    // Update original sender activity
    routingGraph->updateNodeActivity(p->from, currentTime);

    // Update relay node activity if this is a relayed packet
    // Only update if relay node is not us and not the sender (safety checks)
    if (p->relay_node != 0) {
        NodeNum relayNodeId = resolveRelayIdentity(p->relay_node, p->rx_rssi, p->rx_snr);
        if (relayNodeId != 0 && relayNodeId != ourNodeId && relayNodeId != p->from) {
            routingGraph->updateNodeActivity(relayNodeId, currentTime);
        }
    }
}



void SignalRoutingModule::handleNodeInfoPacket(const meshtastic_MeshPacket &mp)
{
    meshtastic_User user = meshtastic_User_init_zero;
    if (!pb_decode_from_bytes(mp.decoded.payload.bytes, mp.decoded.payload.size, &meshtastic_User_msg, &user)) {
        return;
    }

    // Only update capability status if the node is not already known to be SR-capable
    // NodeInfo packets don't contain SR capability info, so don't downgrade from SR-active/inactive to Unknown
    CapabilityStatus currentStatus = getCapabilityStatus(mp.from);
    if (currentStatus != CapabilityStatus::SRactive && currentStatus != CapabilityStatus::Passive) {
        CapabilityStatus status = capabilityFromRole(user.role);
        if (status != CapabilityStatus::Unknown) {
            trackNodeCapability(mp.from, status);
        }
    }

    if (user.has_is_unmessagable && user.is_unmessagable) {
        // Don't downgrade SR-capable nodes — they proved their capability via topology broadcasts.
        // is_unmessagable just means the node doesn't handle user messages, not that it can't route.
        CapabilityStatus current = getCapabilityStatus(mp.from);
        if (current != CapabilityStatus::SRactive && current != CapabilityStatus::Passive) {
            trackNodeCapability(mp.from, CapabilityStatus::Legacy);
        }
    }
}

void SignalRoutingModule::handleSniffedPayload(const meshtastic_MeshPacket &mp, bool isDirectNeighbor)
{
    switch (mp.decoded.portnum) {
    case meshtastic_PortNum_NODEINFO_APP:
        handleNodeInfoPacket(mp);
        break;
    case meshtastic_PortNum_POSITION_APP:
        handlePositionPacket(mp, isDirectNeighbor);
        break;
    case meshtastic_PortNum_TELEMETRY_APP:
        handleTelemetryPacket(mp);
        break;
    case meshtastic_PortNum_ROUTING_APP:
        handleRoutingControlPacket(mp);
        break;
    default:
        break;
    }
}

void SignalRoutingModule::handlePositionPacket(const meshtastic_MeshPacket &mp, bool isDirectNeighbor)
{
    meshtastic_Position position = meshtastic_Position_init_zero;
    if (!pb_decode_from_bytes(mp.decoded.payload.bytes, mp.decoded.payload.size, &meshtastic_Position_msg, &position)) {
        return;
    }

    // We only care about position packets for neighbor RSSI/SNR tracking. The PositionModule
    // already logs the lat/lon/speed/PDOP details, so SR-side re-logging just bloats .text
    // (lat/lon need %.5f which drags in the soft-float printf path).
    (void)position;
    if (isDirectNeighbor && mp.rx_rssi != 0) {
        updateNeighborInfo(mp.from, mp.rx_rssi, mp.rx_snr, mp.rx_time);
    }
}

void SignalRoutingModule::handleTelemetryPacket(const meshtastic_MeshPacket &mp)
{
    // SR's only interest in telemetry is keeping its capability-status timestamp fresh for
    // nodes we already track. TelemetryModule already prints the metric values; re-logging
    // them here (especially the float-formatted volt/airUtil/temp/humidity/pressure values)
    // costs significant flash on size-constrained variants.
    CapabilityStatus currentStatus = getCapabilityStatus(mp.from);
    if (currentStatus != CapabilityStatus::Unknown) {
        trackNodeCapability(mp.from, currentStatus);
    }
}

void SignalRoutingModule::handleRoutingControlPacket(const meshtastic_MeshPacket &mp)
{
    meshtastic_Routing routing = meshtastic_Routing_init_zero;
    if (!pb_decode_from_bytes(mp.decoded.payload.bytes, mp.decoded.payload.size, &meshtastic_Routing_msg, &routing)) {
        return;
    }

    char senderName[64];
    getNodeDisplayName(mp.from, senderName, sizeof(senderName));
    
    // Only resolve placeholders if the routing packet itself is from a direct sender
    bool isDirectRoutingPacket = isDirectPacket(mp);

    switch (routing.which_variant) {
    case meshtastic_Routing_route_request_tag:
        LOG_INFO("[SR] Routing request from %s with %u hops recorded", senderName,
                  routing.route_request.route_count);

        // Check for placeholder resolution in route_request hops
        // Only resolve if: 1) routing packet is from direct sender, and 2) hop node is direct neighbor of ours
        if (isDirectRoutingPacket) {
            for (size_t i = 0; i < routing.route_request.route_count; i++) {
                NodeNum hopNode = routing.route_request.route[i];
                uint8_t hopLastByte = hopNode & 0xFF;
                NodeNum placeholderId = getPlaceholderForRelay(hopLastByte);
                if (isPlaceholderNode(placeholderId) && isPlaceholderConnectedToUs(placeholderId)) {
                    // Additional check: the hop node must be a direct neighbor of ours (Reported edge)
                    bool isDirectNeighbor = false;
                    NodeNum ourNode = nodeDB->getNodeNum();
                    const NodeEdges* ourEdges = routingGraph->getEdgesFrom(ourNode);
                    if (ourEdges) {
                        for (uint8_t j = 0; j < ourEdges->edgeCount; j++) {
                            if (ourEdges->edges[j].to == hopNode && ourEdges->edges[j].source == Edge::Source::Reported) {
                                isDirectNeighbor = true;
                                break;
                            }
                        }
                    }
                    if (isDirectNeighbor) {
                        LOG_INFO("[SR] Traceroute resolution: placeholder %08x -> %08x (direct neighbor in route_request)", placeholderId, hopNode);
                        resolvePlaceholder(placeholderId, hopNode);
                    } else {
                        LOG_INFO("[SR] Skipping traceroute resolution: %08x is not a direct neighbor", hopNode);
                    }
                }
            }
        } else {
            LOG_INFO("[SR] Skip placeholder res (relayed pkt)");
        }
        break;
    case meshtastic_Routing_route_reply_tag:
        LOG_INFO("[SR] Routing reply from %s for %u hops", senderName, routing.route_reply.route_back_count);

        // Check for placeholder resolution in route_reply hops
        // Only resolve if: 1) routing packet is from direct sender, and 2) hop node is direct neighbor of ours
        if (isDirectRoutingPacket) {
            for (size_t i = 0; i < routing.route_reply.route_back_count; i++) {
                NodeNum hopNode = routing.route_reply.route_back[i];
                uint8_t hopLastByte = hopNode & 0xFF;
                NodeNum placeholderId = getPlaceholderForRelay(hopLastByte);
                if (isPlaceholderNode(placeholderId) && isPlaceholderConnectedToUs(placeholderId)) {
                    // Additional check: the hop node must be a direct neighbor of ours (Reported edge)
                    bool isDirectNeighbor = false;
                    NodeNum ourNode = nodeDB->getNodeNum();
                    const NodeEdges* ourEdges = routingGraph->getEdgesFrom(ourNode);
                    if (ourEdges) {
                        for (uint8_t j = 0; j < ourEdges->edgeCount; j++) {
                            if (ourEdges->edges[j].to == hopNode && ourEdges->edges[j].source == Edge::Source::Reported) {
                                isDirectNeighbor = true;
                                break;
                            }
                        }
                    }
                    if (isDirectNeighbor) {
                        LOG_INFO("[SR] Traceroute resolution: placeholder %08x -> %08x (direct neighbor in route_reply)", placeholderId, hopNode);
                        resolvePlaceholder(placeholderId, hopNode);
                    } else {
                        LOG_INFO("[SR] Skipping traceroute resolution: %08x is not a direct neighbor", hopNode);
                    }
                }
            }
        } else {
            LOG_INFO("[SR] Skip placeholder res (relayed pkt)");
        }
        break;
    case meshtastic_Routing_error_reason_tag:
        if (routing.error_reason == meshtastic_Routing_Error_NONE) {
            LOG_INFO("[SR] Routing status from %s (no error)", senderName);
        } else {
            LOG_WARN("[SR] Routing error from %s reason=%u", senderName, routing.error_reason);
        }
        break;
    default:
        LOG_INFO("[SR] Routing control variant %u from %s", routing.which_variant, senderName);
        break;
    }
}

bool SignalRoutingModule::isActiveRoutingRole() const
{
    switch (config.device.role) {
    case meshtastic_Config_DeviceConfig_Role_ROUTER:
    case meshtastic_Config_DeviceConfig_Role_ROUTER_LATE:
    case meshtastic_Config_DeviceConfig_Role_ROUTER_CLIENT:
    case meshtastic_Config_DeviceConfig_Role_REPEATER:
    case meshtastic_Config_DeviceConfig_Role_CLIENT:
    case meshtastic_Config_DeviceConfig_Role_CLIENT_BASE:
        return true;
    default:
        return false;
    }
}

bool SignalRoutingModule::canSendTopology() const
{
    // Returns true if this node can send topology broadcasts
    // This includes:
    // - Active routing roles (ROUTER, REPEATER, CLIENT, etc.)
    // - Passive roles (CLIENT_MUTE, TRACKER, SENSOR, TAK, etc.) to announce themselves as SR-aware
    switch (config.device.role) {
    // Active routing roles
    case meshtastic_Config_DeviceConfig_Role_ROUTER:
    case meshtastic_Config_DeviceConfig_Role_ROUTER_LATE:
    case meshtastic_Config_DeviceConfig_Role_ROUTER_CLIENT:
    case meshtastic_Config_DeviceConfig_Role_REPEATER:
    case meshtastic_Config_DeviceConfig_Role_CLIENT:
    case meshtastic_Config_DeviceConfig_Role_CLIENT_BASE:
    // Passive roles that participate in SR
    case meshtastic_Config_DeviceConfig_Role_CLIENT_MUTE:
    case meshtastic_Config_DeviceConfig_Role_TRACKER:
    case meshtastic_Config_DeviceConfig_Role_SENSOR:
    case meshtastic_Config_DeviceConfig_Role_TAK:
    case meshtastic_Config_DeviceConfig_Role_TAK_TRACKER:
    case meshtastic_Config_DeviceConfig_Role_CLIENT_HIDDEN:
        return true;
    default:
        return false;
    }
}

SignalRoutingModule::CapabilityStatus SignalRoutingModule::capabilityFromRole(
    meshtastic_Config_DeviceConfig_Role role) const
{
    // Role alone cannot determine SR capability — stock firmware uses the same roles.
    // Only mark as Legacy for roles that definitely cannot participate in routing.
    // SR-passive/SR-active status is set exclusively when we receive an actual SR topology packet.
    switch (role) {
    case meshtastic_Config_DeviceConfig_Role_LOST_AND_FOUND:
    case meshtastic_Config_DeviceConfig_Role_CLIENT_MUTE:
    case meshtastic_Config_DeviceConfig_Role_CLIENT_HIDDEN:
        return CapabilityStatus::Legacy;
    default:
        return CapabilityStatus::Unknown;
    }
}

void SignalRoutingModule::trackNodeCapability(NodeNum nodeId, CapabilityStatus status)
{
    if (nodeId == 0) {
        return;
    }

    uint32_t now = millis() / 1000;  // Use monotonic time for TTL calculations

    // Lite mode: linear search in fixed array
    for (uint8_t i = 0; i < capabilityRecordCount; i++) {
        if (capabilityRecords[i].nodeId == nodeId) {
            capabilityRecords[i].record.lastUpdated = now;
            if (status == CapabilityStatus::SRactive || status == CapabilityStatus::Passive) {
                capabilityRecords[i].record.status = status;
            } else if (status == CapabilityStatus::Legacy) {
                capabilityRecords[i].record.status = CapabilityStatus::Legacy;
            }
            return;
        }
    }
    // Add new entry
    if (capabilityRecordCount < MAX_CAPABILITY_RECORDS) {
        capabilityRecords[capabilityRecordCount].nodeId = nodeId;
        capabilityRecords[capabilityRecordCount].record.lastUpdated = now;
        capabilityRecords[capabilityRecordCount].record.status = status;
        capabilityRecordCount++;
    }
}

void SignalRoutingModule::pruneCapabilityCache(uint32_t nowSecs)
{
    NodeNum myNode = nodeDB ? nodeDB->getNodeNum() : 0;

    // Lite mode: remove stale entries by swapping with last
    for (uint8_t i = 0; i < capabilityRecordCount;) {
        // Never prune our own node's capability record
        if (capabilityRecords[i].nodeId == myNode) {
            i++;
            continue;
        }

        if ((nowSecs - capabilityRecords[i].record.lastUpdated) > CAPABILITY_TTL_SECS) {
            // When an SR node's capability expires, clear hearsUs so we stop counting it for coverage
            if (routingGraph && (capabilityRecords[i].record.status == CapabilityStatus::SRactive ||
                                 capabilityRecords[i].record.status == CapabilityStatus::Passive)) {
                NodeNum expiredNode = capabilityRecords[i].nodeId;
                routingGraph->setEdgeHearsUs(myNode, expiredNode, false);
                LOG_INFO("[SR] Capability expired for %08x — cleared hearsUs", expiredNode);
            }
            if (i < capabilityRecordCount - 1) {
                capabilityRecords[i] = capabilityRecords[capabilityRecordCount - 1];
            }
            capabilityRecordCount--;
        } else {
            i++;
        }
    }
}

// Gateway pruning now handled by NeighborGraph's downstream table aging

SignalRoutingModule::CapabilityStatus SignalRoutingModule::getCapabilityStatus(NodeNum nodeId) const
{
    uint32_t now = millis() / 1000;  // Use monotonic time for TTL calculations

    // Special case: local node capability based on its role
    if (nodeDB && nodeId == nodeDB->getNodeNum()) {
        if (!signalBasedRoutingEnabled) {
            return CapabilityStatus::Legacy;  // SR module disabled
        }

        if (isActiveRoutingRole()) {
            return CapabilityStatus::SRactive;  // Active routing roles are SR-active
        } else if (canSendTopology()) {
            return CapabilityStatus::Passive;  // Can send topology (passive roles)
        } else {
            return CapabilityStatus::Legacy;  // Can't send topology, doesn't participate in SR
        }
    }

    NodeNum myNode = nodeDB ? nodeDB->getNodeNum() : 0;

    // Lite mode: linear search
    for (uint8_t i = 0; i < capabilityRecordCount; i++) {
        if (capabilityRecords[i].nodeId == nodeId) {
            // Never return Unknown for our own node
            if (capabilityRecords[i].nodeId == myNode) {
                return capabilityRecords[i].record.status;
            }

            uint32_t age = now - capabilityRecords[i].record.lastUpdated;
            if (age > CAPABILITY_TTL_SECS) {
                return CapabilityStatus::Unknown;
            }
            return capabilityRecords[i].record.status;
        }
    }
    return CapabilityStatus::Unknown;
}

// Returns true for nodes that relay immediately with no added delay (ROUTER and REPEATER).
// Used for Phase 1 slot scheduling — we give these an early slot and assume they'll transmit.
// ROUTER_LATE is excluded: it deliberately delays, so waiting for it would slow SR relays.
bool SignalRoutingModule::isImmediateRelayRouter(NodeNum nodeId) const
{
    if (!nodeDB) return false;
    const meshtastic_NodeInfoLite *node = nodeDB->getMeshNode(nodeId);
    if (!node || !node->has_user) return false;
    auto role = node->user.role;
    return role == meshtastic_Config_DeviceConfig_Role_ROUTER ||
           role == meshtastic_Config_DeviceConfig_Role_REPEATER ||
           role == meshtastic_Config_DeviceConfig_Role_ROUTER_CLIENT;
}

bool SignalRoutingModule::isLegacyRouter(NodeNum nodeId) const
{
    if (!nodeDB) {
        return false;
    }
    const meshtastic_NodeInfoLite *node = nodeDB->getMeshNode(nodeId);
    if (!node || !node->has_user) {
        return false;
    }

    auto role = node->user.role;
    switch (role) {
    case meshtastic_Config_DeviceConfig_Role_ROUTER:
    case meshtastic_Config_DeviceConfig_Role_ROUTER_LATE:
    case meshtastic_Config_DeviceConfig_Role_ROUTER_CLIENT:
    case meshtastic_Config_DeviceConfig_Role_REPEATER:
        return true;
    default:
        return false;
    }
}

/**
 * Check if a stock node has a non-relaying role (CLIENT_MUTE, CLIENT_HIDDEN, LOST_AND_FOUND).
 * These nodes never relay packets in stock firmware, so hearing them is sufficient proof of coverage.
 */
bool SignalRoutingModule::isNonRelayingLegacyRole(NodeNum nodeId) const
{
    if (!nodeDB) return true;
    const meshtastic_NodeInfoLite *node = nodeDB->getMeshNode(nodeId);
    if (!node || !node->has_user) return true; // Unknown role — assume non-relaying (conservative)

    switch (node->user.role) {
    case meshtastic_Config_DeviceConfig_Role_CLIENT_MUTE:
    case meshtastic_Config_DeviceConfig_Role_CLIENT_HIDDEN:
    case meshtastic_Config_DeviceConfig_Role_LOST_AND_FOUND:
        return true;
    default:
        return false;
    }
}

/**
 * Mark the edge to a stock node as hearsUs (confirmed bidirectional link).
 * Called when we observe a stock node relaying a packet we transmitted.
 */
void SignalRoutingModule::markStockNodeRelayedOurPacket(NodeNum stockNode)
{
    if (!routingGraph || !nodeDB || stockNode == 0) return;

    NodeNum myNode = nodeDB->getNodeNum();
    NodeEdges *myEdges = const_cast<NodeEdges *>(routingGraph->getEdgesFrom(myNode));
    if (!myEdges) return;

    for (uint8_t i = 0; i < myEdges->edgeCount; i++) {
        if (myEdges->edges[i].to == stockNode) {
            if (!myEdges->edges[i].hearsUs) {
                myEdges->edges[i].hearsUs = true;
                char nodeName[64];
                getNodeDisplayName(stockNode, nodeName, sizeof(nodeName));
                LOG_INFO("[SR] Stock node %s relayed our packet — confirmed bidirectional link", nodeName);
            }
            return;
        }
    }
}

/**
 * Check if a node is routable (can be used as intermediate hop for routing)
 * Mute nodes are not routable since they don't relay packets
 */
bool SignalRoutingModule::isNodeRoutable(NodeNum nodeId) const {
    // Mute nodes don't relay, so they cannot be used as intermediate routing hops
    if (config.device.role == meshtastic_Config_DeviceConfig_Role_CLIENT_MUTE &&
        nodeId == nodeDB->getNodeNum()) {
        // Local mute node - not routable
        return false;
    }

    // Check if this is a known mute node (signal_routing_active = false)
    // For remote nodes, we use the capability tracking
    CapabilityStatus status = getCapabilityStatus(nodeId);
    if (status == CapabilityStatus::Legacy) {
        // Legacy nodes participate in SR routing only if they are routers/repeaters
        // that will actually relay packets
        return isLegacyRouter(nodeId);
    }

    return true;
}

bool SignalRoutingModule::topologyHealthyForBroadcast() const
{
    LOG_INFO("[SR] Topology healthy for broadcast");
    if (!routingGraph || !nodeDB) {
        LOG_WARN("[SR] routingGraph or nodeDB is null, returning false");
        return false;
    }

    // Check if we have direct SR-capable neighbors for intelligent broadcast routing
    LOG_INFO("[SR] Checking direct neighbors");

    const NodeEdges* nodeEdges = routingGraph->getEdgesFrom(nodeDB->getNodeNum());
    if (!nodeEdges || nodeEdges->edgeCount == 0) {
        LOG_INFO("[SR] No edges found, returning false");
        return false;
    }

    size_t capableNeighbors = 0;
    for (uint8_t i = 0; i < nodeEdges->edgeCount; i++) {
        NodeNum to = nodeEdges->edges[i].to;
        CapabilityStatus status = getCapabilityStatus(to);
        if (status == CapabilityStatus::SRactive || status == CapabilityStatus::Unknown) {
            capableNeighbors++;
        } else if (isLegacyRouter(to)) {
            capableNeighbors++;
        }
    }

    // Need at least 1 direct neighbor that could be SR-capable for meaningful broadcast routing
    return capableNeighbors >= 1;
}

bool SignalRoutingModule::topologyHealthyForUnicast(NodeNum destination) const
{
    if (!routingGraph || !nodeDB) {
        return false;
    }

    // For unicast, we need to know that the destination is reachable through the topology graph
    // The NodeDB check is unreliable due to RTC sync issues (last_heard may be 0)
    // If we can find ANY route to the destination, consider it reachable

    NodeNum myNode = nodeDB->getNodeNum();
    if (myNode == 0) {
        return false;
    }

    Route route = routingGraph->calculateRoute(destination, millis() / 1000,
        [this](NodeNum nodeId) { return isNodeRoutable(nodeId); });

    if (route.nextHop != 0) {
        LOG_INFO("[SR] Node %08x is reachable through topology (nextHop=%08x, cost=%.2f)",
                 destination, route.nextHop, route.getCost());
        return true;
    }

    // Fallback: Check if destination is reachable via a downstream relay
    NodeNum relay = routingGraph->getDownstreamRelay(destination);
    if (relay != 0) {
        // Check if we can reach the relay (relay must be routable)
        Route relayRoute = routingGraph->calculateRoute(relay, millis() / 1000,
            [this](NodeNum nodeId) { return isNodeRoutable(nodeId); });
        if (relayRoute.nextHop != 0) {
            LOG_INFO("[SR] Node %08x is reachable via relay %08x (nextHop=%08x, cost=%.2f)",
                     destination, relay, relayRoute.nextHop, relayRoute.getCost());
            return true;
        }
    }

    return false;
}

void SignalRoutingModule::rememberRelayIdentity(NodeNum nodeId, uint8_t relayId)
{
    if (relayId == 0 || nodeId == 0) {
        return;
    }

    uint32_t nowMs = millis();

    // Find or create bucket for this relayId
    RelayIdentityCacheEntry *bucket = nullptr;
    for (uint8_t i = 0; i < relayIdentityCacheCount; i++) {
        if (relayIdentityCache[i].relayId == relayId) {
            bucket = &relayIdentityCache[i];
            break;
        }
    }
    if (!bucket && relayIdentityCacheCount < MAX_RELAY_IDENTITY_ENTRIES) {
        bucket = &relayIdentityCache[relayIdentityCacheCount++];
        bucket->relayId = relayId;
        bucket->entryCount = 0;
    }
    if (!bucket) return;

    // Prune stale entries in bucket
    for (uint8_t i = 0; i < bucket->entryCount;) {
        if ((nowMs - bucket->entries[i].lastHeardMs) > RELAY_ID_CACHE_TTL_MS) {
            if (i < bucket->entryCount - 1) {
                bucket->entries[i] = bucket->entries[bucket->entryCount - 1];
            }
            bucket->entryCount--;
        } else {
            i++;
        }
    }

    // Update existing or add new
    for (uint8_t i = 0; i < bucket->entryCount; i++) {
        if (bucket->entries[i].nodeId == nodeId) {
            bucket->entries[i].lastHeardMs = nowMs;
            return;
        }
    }
    if (bucket->entryCount < 4) {
        bucket->entries[bucket->entryCount].nodeId = nodeId;
        bucket->entries[bucket->entryCount].lastHeardMs = nowMs;
        bucket->entryCount++;
    }
}

void SignalRoutingModule::pruneRelayIdentityCache(uint32_t nowMs)
{
    for (uint8_t b = 0; b < relayIdentityCacheCount;) {
        RelayIdentityCacheEntry *bucket = &relayIdentityCache[b];
        // Prune entries
        for (uint8_t i = 0; i < bucket->entryCount;) {
            if ((nowMs - bucket->entries[i].lastHeardMs) > RELAY_ID_CACHE_TTL_MS) {
                if (i < bucket->entryCount - 1) {
                    bucket->entries[i] = bucket->entries[bucket->entryCount - 1];
                }
                bucket->entryCount--;
            } else {
                i++;
            }
        }
        // Remove empty buckets
        if (bucket->entryCount == 0) {
            if (b < relayIdentityCacheCount - 1) {
                relayIdentityCache[b] = relayIdentityCache[relayIdentityCacheCount - 1];
            }
            relayIdentityCacheCount--;
        } else {
            b++;
        }
    }
}

NodeNum SignalRoutingModule::resolveRelayIdentity(uint8_t relayId, int16_t rxRssi, float rxSnr) const
{
    uint32_t nowMs = millis();
    NodeNum bestNode = 0;
    uint32_t newest = 0;

    // Collect direct neighbor candidates (up to 4 per bucket)
    struct DirectCandidate {
        NodeNum nodeId;
        uint16_t edgeEtx; // ETX * 100 from the edge
    };
    DirectCandidate directCandidates[4];
    uint8_t directCount = 0;

    // Determine our direct neighbors for tiebreaking
    NodeNum myNode = nodeDB ? nodeDB->getNodeNum() : 0;
    const NodeEdges *myEdges = (routingGraph && myNode) ? routingGraph->getEdgesFrom(myNode) : nullptr;

    auto getDirectEdgeEtx = [&](NodeNum nodeId) -> int32_t {
        if (!myEdges) {
            return -1;
        }
        for (uint8_t i = 0; i < myEdges->edgeCount; i++) {
            if (myEdges->edges[i].to == nodeId) {
                return myEdges->edges[i].etxFixed;
            }
        }
        return -1;
    };

    for (uint8_t b = 0; b < relayIdentityCacheCount; b++) {
        if (relayIdentityCache[b].relayId == relayId) {
            const RelayIdentityCacheEntry *bucket = &relayIdentityCache[b];
            for (uint8_t i = 0; i < bucket->entryCount; i++) {
                if ((nowMs - bucket->entries[i].lastHeardMs) > RELAY_ID_CACHE_TTL_MS) {
                    continue;
                }
                uint32_t t = bucket->entries[i].lastHeardMs;
                NodeNum n = bucket->entries[i].nodeId;
                int32_t edgeEtx = getDirectEdgeEtx(n);
                if (edgeEtx >= 0 && directCount < 4) {
                    directCandidates[directCount++] = {n, static_cast<uint16_t>(edgeEtx)};
                } else if (edgeEtx < 0) {
                    if (t >= newest) { newest = t; bestNode = n; }
                }
            }
            break;
        }
    }

    // Pick best direct neighbor
    NodeNum bestDirectNode = 0;
    if (directCount == 1) {
        bestDirectNode = directCandidates[0].nodeId;
    } else if (directCount > 1 && rxRssi != 0) {
        // Multiple direct neighbors share this relay byte — use packet ETX to disambiguate
        float packetEtx = NeighborGraph::calculateETX(rxRssi, rxSnr);
        uint16_t packetEtxFixed = static_cast<uint16_t>(packetEtx * 100.0f);
        uint16_t bestDiff = UINT16_MAX;
        for (uint8_t i = 0; i < directCount; i++) {
            uint16_t diff = (packetEtxFixed > directCandidates[i].edgeEtx)
                                ? (packetEtxFixed - directCandidates[i].edgeEtx)
                                : (directCandidates[i].edgeEtx - packetEtxFixed);
            if (diff < bestDiff) {
                bestDiff = diff;
                bestDirectNode = directCandidates[i].nodeId;
            }
        }
    } else if (directCount > 1) {
        // No RSSI hint — fall back to most recently heard
        uint32_t newestDirect = 0;
        for (uint8_t b = 0; b < relayIdentityCacheCount; b++) {
            if (relayIdentityCache[b].relayId == relayId) {
                const RelayIdentityCacheEntry *bucket = &relayIdentityCache[b];
                for (uint8_t i = 0; i < bucket->entryCount; i++) {
                    if ((nowMs - bucket->entries[i].lastHeardMs) > RELAY_ID_CACHE_TTL_MS) {
                        continue;
                    }
                    for (uint8_t d = 0; d < directCount; d++) {
                        if (directCandidates[d].nodeId == bucket->entries[i].nodeId) {
                            if (bucket->entries[i].lastHeardMs >= newestDirect) {
                                newestDirect = bucket->entries[i].lastHeardMs;
                                bestDirectNode = bucket->entries[i].nodeId;
                            }
                            break;
                        }
                    }
                }
                break;
            }
        }
    }

    // Prefer direct neighbor over non-neighbor when there's a collision
    NodeNum result = bestDirectNode ? bestDirectNode : bestNode;

    // Don't return placeholders - they should be resolved to real nodes
    if (isPlaceholderNode(result)) {
        return 0;
    }
    return result;
}



NodeNum SignalRoutingModule::resolveHeardFrom(const meshtastic_MeshPacket *p, NodeNum sourceNode) const
{
    if (!p) {
        return sourceNode;
    }

    if (p->relay_node == 0) {
        return sourceNode;
    }

    NodeNum resolved = resolveRelayIdentity(p->relay_node, p->rx_rssi, p->rx_snr);
    if (resolved != 0) {
        return resolved;
    }

    // Check ALL known nodes (both Reported and Mirrored edges), not just direct neighbors,
    // because the relay might be a node we only know through topology broadcasts
    NodeNum placeholderMatch = 0;
    if (routingGraph && nodeDB) {
        const NodeEdges *myEdges = routingGraph->getEdgesFrom(nodeDB->getNodeNum());
        if (myEdges) {
            for (uint8_t i = 0; i < myEdges->edgeCount; i++) {
                if ((myEdges->edges[i].to & 0xFF) == p->relay_node) {
                    if (!isPlaceholderNode(myEdges->edges[i].to)) {
                        // Remember this mapping for future use
                        const_cast<SignalRoutingModule*>(this)->rememberRelayIdentity(myEdges->edges[i].to, p->relay_node);
                        return myEdges->edges[i].to;
                    }
                    // Remember the placeholder in case no real node matches
                    placeholderMatch = myEdges->edges[i].to;
                }
            }
        }
    }

    // Return placeholder if we found one in the graph
    if (placeholderMatch != 0) {
        return placeholderMatch;
    }

    // Last resort: synthesize placeholder ID from relay byte so we never
    // falsely claim the packet came directly from the source
    return 0xFF000000 | p->relay_node;
}


bool SignalRoutingModule::hasDirectConnectivity(NodeNum nodeA, NodeNum nodeB)
{
    if (!routingGraph || !nodeDB) {
        return false;
    }

    // Check if nodeA has a direct edge to nodeB
    const NodeEdges* edges = routingGraph->getEdgesFrom(nodeA);
    if (edges) {
        for (uint8_t i = 0; i < edges->edgeCount; i++) {
            if (edges->edges[i].to == nodeB) {
                return true;
            }
        }
    }

    return false;
}

// Enhanced connectivity check that considers stock firmware limitations
// Returns: true = verified connectivity, false = no connectivity or unknown
// The unknownOut parameter indicates if we couldn't verify (stock node involved)
bool SignalRoutingModule::hasVerifiedConnectivity(NodeNum transmitter, NodeNum receiver, bool* unknownOut)
{
    if (!routingGraph || !nodeDB) {
        if (unknownOut) *unknownOut = true;
        return false;
    }

    // Get capability status of both nodes
    CapabilityStatus txStatus = getCapabilityStatus(transmitter);
    CapabilityStatus rxStatus = getCapabilityStatus(receiver);
    
    bool txIsStock = isPlaceholderNode(transmitter) || 
                     txStatus == CapabilityStatus::Legacy || 
                     txStatus == CapabilityStatus::Unknown;
    bool rxIsStock = isPlaceholderNode(receiver) || 
                     rxStatus == CapabilityStatus::Legacy || 
                     rxStatus == CapabilityStatus::Unknown;

    // If both are stock, we have no topology data - unknown
    if (txIsStock && rxIsStock) {
        if (unknownOut) *unknownOut = true;
        return false;
    }

    // Check both directions for edges:
    // 1. transmitter → receiver: transmitter reported hearing receiver
    // 2. receiver → transmitter: receiver reported hearing transmitter
    
    bool foundEdge = false;
    
    // Check transmitter → receiver (only useful if transmitter is SR-active)
    if (!txIsStock) {
        if (hasDirectConnectivity(transmitter, receiver)) {
            foundEdge = true;
        }
    }
    
    // Check receiver → transmitter (only useful if receiver is SR-active)
    if (!foundEdge && !rxIsStock) {
        if (hasDirectConnectivity(receiver, transmitter)) {
            foundEdge = true;
        }
    }
    
    if (foundEdge) {
        if (unknownOut) *unknownOut = false;
        return true;
    }
    
    // No edge found. Determine if this is "unknown" or "no connectivity"
    // If the SR-active node has edges but none to the stock node, it's likely no connectivity
    // But if the SR-active node has very few edges or the stock node is new, it could be unknown
    
    if (txIsStock || rxIsStock) {
        // One node is stock - we can't be certain, mark as unknown
        if (unknownOut) *unknownOut = true;
    } else {
        // Both are SR-active and no edge exists - they likely can't hear each other
        if (unknownOut) *unknownOut = false;
    }

    return false;
}

#endif // !MESHTASTIC_EXCLUDE_SIGNALROUTING

