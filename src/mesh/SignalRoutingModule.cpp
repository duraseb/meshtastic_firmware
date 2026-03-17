#include "SignalRoutingModule.h"
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

int32_t SignalRoutingModule::runOnce()
{
    uint32_t nowMs = millis();
    uint32_t nowSecs = millis() / 1000;  // Use monotonic time for aging

    pruneCapabilityCache(nowSecs);
    pruneRelayIdentityCache(nowMs);

    if (routingGraph && signalBasedRoutingEnabled) {
        // Send empty SR broadcast at boot to announce presence and trigger
        // neighbor topology responses, regardless of whether we have neighbors yet
        if (needsBootBroadcast) {
            needsBootBroadcast = false;
            LOG_INFO("[SR] Sending empty boot broadcast to bootstrap topology");
            sendTopologyPacket(NODENUM_BROADCAST, nullptr, 0, 0);
            lastBroadcast = nowMs;
        } else if (nowMs - lastBroadcast >= SIGNAL_ROUTING_BROADCAST_SECS * 1000) {
            sendSignalRoutingInfo();
        } else if (topologyDirty && nowMs - lastBroadcast >= 60 * 1000) {
            LOG_INFO("[SR] Topology dirty — sending early broadcast");
            sendSignalRoutingInfo();
            topologyDirty = false;
        }

        // Topology logging: periodic (every 60s) or when topology changed
        static uint32_t lastTopologyLog = 0;
        if (topologyDirty || nowMs - lastTopologyLog >= 60 * 1000) {
            logNetworkTopology();
            lastTopologyLog = nowMs;
            topologyDirty = false;
        }
    }

    uint32_t broadcastCycle = topologyDirty ? 60 * 1000 : SIGNAL_ROUTING_BROADCAST_SECS * 1000;
    uint32_t elapsed = nowMs - lastBroadcast;
    uint32_t timeToBroadcast = (elapsed < broadcastCycle) ? (broadcastCycle - elapsed) : 0;

    uint32_t nextDelay = timeToBroadcast;

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

    // Collect all neighbors into fixed-size array (max 2 packets worth = 22 neighbors)
    static constexpr uint8_t MAX_BROADCAST_NEIGHBORS = MAX_SIGNAL_ROUTING_NEIGHBORS * 2;
    meshtastic_SignalNeighbor allNeighbors[MAX_BROADCAST_NEIGHBORS];
    uint8_t totalNeighbors = 0;
    collectNeighborsForBroadcast(allNeighbors, totalNeighbors, MAX_BROADCAST_NEIGHBORS);

    if (totalNeighbors == 0) {
        // Send empty broadcast to announce SR capability.
        // SR nodes that receive this from a direct neighbor will trigger an early
        // broadcast of their own topology, helping the new node bootstrap.
        sendTopologyPacket(dest, nullptr, 0, 0);
        return;
    }

    // Split into chunks of MAX_SIGNAL_ROUTING_NEIGHBORS and send multiple packets
    uint8_t topologyVersion = currentTopologyVersion++;
    uint8_t packetsNeeded = (totalNeighbors + MAX_SIGNAL_ROUTING_NEIGHBORS - 1) / MAX_SIGNAL_ROUTING_NEIGHBORS;

    char ourName[48];
    getNodeDisplayName(nodeDB->getNodeNum(), ourName, sizeof(ourName));

    LOG_INFO("[SR] SENDING: Broadcasting %u neighbors in %u packet(s) from %s (version %u)",
             totalNeighbors, packetsNeeded, ourName, topologyVersion);

    for (uint8_t packetIndex = 0; packetIndex < packetsNeeded; packetIndex++) {
        uint8_t startIdx = packetIndex * MAX_SIGNAL_ROUTING_NEIGHBORS;
        uint8_t count = std::min((uint8_t)MAX_SIGNAL_ROUTING_NEIGHBORS, (uint8_t)(totalNeighbors - startIdx));
        sendTopologyPacket(dest, &allNeighbors[startIdx], count, topologyVersion);
    }

    // Update our own capability after sending
    trackNodeCapability(nodeDB->getNodeNum(), isActiveRoutingRole() ? CapabilityStatus::SRactive : CapabilityStatus::Passive);
}
void SignalRoutingModule::collectNeighborsForBroadcast(meshtastic_SignalNeighbor *outNeighbors, uint8_t &outCount, uint8_t maxCount)
{
    outCount = 0;
    if (!routingGraph || !nodeDB) return;

    const NodeEdges* nodeEdges = routingGraph->getEdgesFrom(nodeDB->getNodeNum());
    if (!nodeEdges || nodeEdges->edgeCount == 0) {
        return;
    }

    // Collect non-placeholder edge pointers into a fixed-size array for sorting
    const Edge* edgePtrs[NEIGHBOR_GRAPH_MAX_EDGES_PER_NODE];
    uint8_t edgeCount = 0;
    for (uint8_t i = 0; i < nodeEdges->edgeCount && edgeCount < NEIGHBOR_GRAPH_MAX_EDGES_PER_NODE; i++) {
        if (!isPlaceholderNode(nodeEdges->edges[i].to)) {
            edgePtrs[edgeCount++] = &nodeEdges->edges[i];
        }
    }

    // Sort by quality (reported first, then by ETX)
    std::sort(edgePtrs, edgePtrs + edgeCount, [](const Edge* a, const Edge* b) {
        if (a->source != b->source) {
            return a->source == Edge::Source::Reported;
        }
        return a->getEtx() < b->getEtx();
    });

    // Convert to SignalNeighbor format
    for (uint8_t i = 0; i < edgeCount && outCount < maxCount; i++) {
        const Edge* edge = edgePtrs[i];
        meshtastic_SignalNeighbor &neighbor = outNeighbors[outCount];
        memset(&neighbor, 0, sizeof(neighbor));

        neighbor.node_id = edge->to;
        neighbor.position_variance = edge->variance;

        CapabilityStatus neighborStatus = getCapabilityStatus(edge->to);
        neighbor.signal_routing_active = (neighborStatus == CapabilityStatus::SRactive);
        neighbor.hears_us = edge->hearsUs;

        int32_t rssi32, snr32;
        NeighborGraph::etxToSignal(edge->getEtx(), rssi32, snr32);
        neighbor.rssi = static_cast<int8_t>(std::max((int32_t)-128, std::min((int32_t)127, rssi32)));
        neighbor.snr = static_cast<int8_t>(std::max((int32_t)-128, std::min((int32_t)127, snr32)));

        outCount++;
    }
}

void SignalRoutingModule::sendTopologyPacket(NodeNum dest, const meshtastic_SignalNeighbor *neighbors, uint8_t count, uint8_t topologyVersion)
{
    meshtastic_SignalRoutingInfo info = meshtastic_SignalRoutingInfo_init_zero;
    info.signal_routing_active = isActiveRoutingRole();
    info.routing_version = SIGNAL_ROUTING_VERSION;
    info.topology_version = topologyVersion;
    info.neighbors_count = count;

    // Copy neighbors to info struct
    for (uint8_t i = 0; i < count && i < MAX_SIGNAL_ROUTING_NEIGHBORS; i++) {
        info.neighbors[i] = neighbors[i];
    }

    meshtastic_MeshPacket *p = allocDataProtobuf(info);
    p->to = dest;
    p->priority = meshtastic_MeshPacket_Priority_BACKGROUND;

    service->sendToMesh(p);
    lastBroadcast = millis();

    // Record our transmission for contention window tracking
    if (routingGraph) {
        uint32_t currentTime = millis() / 1000;  // Use monotonic time
        routingGraph->recordNodeTransmission(nodeDB->getNodeNum(), p->id, currentTime);
    }
}


void SignalRoutingModule::updateGraphWithNeighbor(NodeNum sender, const meshtastic_SignalNeighbor &neighbor)
{
    // Add/update edge from sender to this neighbor
    if (routingGraph) {
        float etx = 1.0f; // Default ETX, will be updated with real measurements
        uint32_t currentTime = millis() / 1000;

        routingGraph->updateEdge(sender, neighbor.node_id, etx, currentTime);

        // Propagate the bidirectional link flag from the authoritative sender
        routingGraph->setEdgeHearsUs(sender, neighbor.node_id, neighbor.hears_us);

        LOG_DEBUG("[SR] Added edge %08x -> %08x from topology", sender, neighbor.node_id);
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
        LOG_DEBUG("[SR] Skipping topology processing for rebroadcast of our packet %08x", p->id);
        return;
    }

    // Only process SignalRoutingInfo packets
    if (p->decoded.portnum != meshtastic_PortNum_SIGNAL_ROUTING_APP) {
        LOG_DEBUG("[SR] Skipping non-SR packet in preProcessSignalRoutingPacket (portnum=%d)", p->decoded.portnum);
        return;
    }
    // Reject packets from invalid node IDs (0 is invalid)
    if (p->from == 0) {
        LOG_DEBUG("[SR] Ignoring SR broadcast from invalid node ID 0");
        return;
    }
    
    // For passive nodes: only process SR broadcasts from direct neighbors
    // Active nodes: process all SR broadcasts for full topology
    if (!isActiveRoutingRole()) {
        // Passive node: check if SR broadcast is from direct sender
        if (!isDirectPacket(*p)) {
            LOG_DEBUG("[SR] Passive role: Ignoring SR broadcast from 0x%08x (not direct, hopStart=%d, hopLimit=%d)",
                     p->from, p->hop_start, p->hop_limit);
            return;
        }
    }

    // Decode the protobuf to get neighbor data
    meshtastic_SignalRoutingInfo info = meshtastic_SignalRoutingInfo_init_zero;
    if (!pb_decode_from_bytes(p->decoded.payload.bytes, p->decoded.payload.size,
                              &meshtastic_SignalRoutingInfo_msg, &info)) {
        LOG_WARN("[SR] Failed to decode SignalRoutingInfo from %08x (payload size=%u)", 
                 p->from, p->decoded.payload.size);
        return;
    }

    // Check version validity with wraparound logic
    uint8_t receivedVersion = info.topology_version;
    uint8_t lastProcessedVersion = getTopologyVersion(lastTopologyVersion, lastTopologyVersionCount, p->from);

    bool accept = false;
    if (receivedVersion > lastProcessedVersion) {
        // Normal case: received is higher, accept
        accept = true;
    } else if (receivedVersion < lastProcessedVersion) {
        // Check if received is within 100 of the wraparound point
        uint8_t threshold = (lastProcessedVersion + 256 - 100) % 256;
        if (receivedVersion >= threshold || receivedVersion < 100) {
            accept = true;
        }
    } else if (receivedVersion == lastProcessedVersion) {
        // Same version - this could be a retransmission or multi-packet
        // Accept it to handle merging
        accept = true;
    }

    if (!accept) {
        LOG_DEBUG("[SR] Ignoring stale topology broadcast from %08x (version %u, last processed %u)",
                 p->from, receivedVersion, lastProcessedVersion);
        return;
    }

    // Check if this is a NEW topology version (not a continuation of multi-packet broadcast)
    bool isNewVersion = (receivedVersion != lastProcessedVersion);
    
    // Update version tracking
    setTopologyVersion(lastTopologyVersion, lastTopologyVersionCount, p->from, receivedVersion);

    // Update capability status for the sender (this is normally done in handleReceivedProtobuf)
    CapabilityStatus newStatus = info.signal_routing_active ? CapabilityStatus::SRactive : CapabilityStatus::Passive;
    CapabilityStatus oldStatus = getCapabilityStatus(p->from);
    trackNodeCapability(p->from, newStatus);

    if (oldStatus != newStatus) {
        char senderName[64];
        getNodeDisplayName(p->from, senderName, sizeof(senderName));
        LOG_INFO("[SR] Capability update: %s changed from %d to %d",
                senderName, (int)oldStatus, (int)newStatus);
    }

    // Process topology directly from the received packet - no intermediate storage
    char senderNameForTopo[48];
    getNodeDisplayName(p->from, senderNameForTopo, sizeof(senderNameForTopo));
    LOG_INFO("[SR] Processing topology from %s: %d neighbors (version %u, %s, relay=0x%02x)",
              senderNameForTopo, info.neighbors_count, receivedVersion,
              isNewVersion ? "new version" : "continuation", p->relay_node);

    // Empty SR broadcast from a direct SR neighbor = bootstrap request.
    // Mark topology dirty so we broadcast our topology at the next opportunity
    // (immediately if last broadcast was >60s ago, otherwise at the 60s mark).
    if (info.neighbors_count == 0 && isDirectPacket(*p) && info.signal_routing_active) {
        LOG_INFO("[SR] Empty broadcast from direct SR neighbor %s — marking topology dirty",
                 senderNameForTopo);
        topologyDirty = true;
    }

    // Mirrored edges are not cleared on new topology versions — they age out naturally.

    // Process each neighbor directly from the received info - memory efficient
    for (pb_size_t i = 0; i < info.neighbors_count; i++) {
        const meshtastic_SignalNeighbor& neighbor = info.neighbors[i];

        // Reject neighbors with invalid node IDs (0 or placeholders)
        if (neighbor.node_id == 0 || isPlaceholderNode(neighbor.node_id)) {
            LOG_DEBUG("[SR] Skipping invalid neighbor node ID: %08x", neighbor.node_id);
            continue;
        }

        // Process this neighbor directly - no need for protobuf handler since we already validated the main packet
        // This is just for graph updates, capability status was already handled for the main sender
        updateGraphWithNeighbor(p->from, neighbor);

        // Create gateway relationship ONLY for nodes we cannot hear directly
        // This ensures remote nodes appear as downstream of the SR broadcaster node,
        // but nodes we can hear directly are not incorrectly marked as downstream
        bool hasDirectConnection = false;
        NodeNum ourNode = nodeDB ? nodeDB->getNodeNum() : 0;
        
        // Never mark ourselves as downstream of anyone
        if (neighbor.node_id == ourNode) {
            hasDirectConnection = true;
        } else if (routingGraph) {
            // Check if we have a direct radio connection to this neighbor
            // A direct connection exists if we have a Reported edge FROM us TO the neighbor
            // This represents our actual reception of their signal with RSSI/SNR data
            // Check edges FROM us TO neighbor with Reported source (actual direct radio connection)
            const NodeEdges* ourEdges = routingGraph->getEdgesFrom(ourNode);
            if (ourEdges) {
                for (uint8_t j = 0; j < ourEdges->edgeCount; j++) {
                    if (ourEdges->edges[j].to == neighbor.node_id &&
                        ourEdges->edges[j].source == Edge::Source::Reported) {
                        hasDirectConnection = true;
                        break;
                    }
                }
            }
        }
        
        // Topology broadcasts share graph connectivity (ETX/routing information) and establish hierarchy
        // Create topology-based gateway relationships for all neighbors in broadcasts. This creates
        // an information-source hierarchy where nodes appear under the SR node that provided
        // their topology information, regardless of relaying history.

        // Detailed logging for debugging topology processing
        char neighborName[48];
        getNodeDisplayName(neighbor.node_id, neighborName, sizeof(neighborName));

        if (!hasDirectConnection) {
            // Establish topology-based hierarchy: nodes learned through topology broadcasts
            // appear as downstream of the broadcasting node, creating a information-source hierarchy
            LOG_INFO("[SR]   -> %s: NO direct connection, marking as downstream of topology source %s",
                    neighborName, senderNameForTopo);
            float etxForDownstream = NeighborGraph::calculateETX(neighbor.rssi, neighbor.snr);
            routingGraph->updateDownstream(neighbor.node_id, p->from, etxForDownstream, millis() / 1000);
        } else {
            LOG_DEBUG("[SR]   -> %s: HAS direct connection, sender confirms reachability",
                    neighborName);
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

    // Mark sender based on their claimed SR capability
    // ALL nodes (active and passive) should track capability status of other SR nodes
    CapabilityStatus newStatus = p->signal_routing_active ? CapabilityStatus::SRactive : CapabilityStatus::Passive;
    CapabilityStatus oldStatus = getCapabilityStatus(mp.from);
    trackNodeCapability(mp.from, newStatus);

    if (oldStatus != newStatus) {
        LOG_INFO("[SR] Capability update: %s changed from %d to %d",
                senderName, (int)oldStatus, (int)newStatus);
    }

    // Inactive SR roles don't participate in routing decisions - skip topology learning from broadcasts
    // But they still tracked the sender's capability above
    if (!isActiveRoutingRole()) {
        LOG_DEBUG("[SR] Passive role: Tracking capability from %s but not processing topology (node count %d)",
                  senderName, p->neighbors_count);
        return false;
    }

    if (p->neighbors_count == 0) {
        LOG_INFO("[SR] %s is online (SR v%d, %s) - no neighbors detected yet",
                 senderName, p->routing_version,
                 p->signal_routing_active ? "SR-active" : "passive");

        // Clear downstream entries for SR-capable nodes with no neighbors - they can't be relays
        if (p->signal_routing_active) {
            routingGraph->clearDownstreamForRelay(mp.from);
        }

        // Bootstrap: if preProcessSignalRoutingPacket didn't run (topology not yet healthy),
        // mark dirty here so we send our topology at the next opportunity.
        if (p->signal_routing_active && isDirectPacket(mp)) {
            LOG_INFO("[SR] Empty broadcast from direct SR neighbor %s — marking topology dirty",
                     senderName);
            topologyDirty = true;
        }

        return false;
    }

    LOG_INFO("[SR] RECEIVED: %s reports %d neighbors (SR v%d, %s)",
             senderName, p->neighbors_count, p->routing_version,
             p->signal_routing_active ? "SR-active" : "passive");

    // For passive SR nodes (signal_routing_active = false), we still need to store their edges for direct connection checks
    // Active nodes use these edges to determine if a passive SR node has direct connections to destinations
    // However, routing algorithms must not consider paths through passive SR nodes since they don't relay
    if (!p->signal_routing_active) {
        LOG_DEBUG("[SR] Received topology from passive SR node %08x - storing edges for direct connection detection", mp.from);
    }

    // Check if preProcessSignalRoutingPacket already handled edge clearing and rebuilding
    // for this exact version — skip redundant work if so
    uint8_t preProcessedVer = getTopologyVersion(lastPreProcessedVersion, lastPreProcessedVersionCount, mp.from);
    bool alreadyPreProcessed = (preProcessedVer != 0 && preProcessedVer == p->routing_version);

    if (!alreadyPreProcessed) {
        // Clear inferred edges pointing TO this node that were created before we knew it was SR-capable
        routingGraph->clearInferredEdgesToNode(mp.from);

        // Add edges from each neighbor TO the sender
        uint32_t rxTime = millis() / 1000;
        for (pb_size_t i = 0; i < p->neighbors_count; i++) {
            const meshtastic_SignalNeighbor& neighbor = p->neighbors[i];

            if (neighbor.node_id == 0 || isPlaceholderNode(neighbor.node_id)) {
                continue;
            }

            float etx = NeighborGraph::calculateETX(neighbor.rssi, neighbor.snr);

            uint32_t scaledVariance = static_cast<uint32_t>(neighbor.position_variance) * 12;

            routingGraph->updateEdge(neighbor.node_id, mp.from, etx, rxTime, scaledVariance,
                                     Edge::Source::Reported);
            routingGraph->updateEdge(mp.from, neighbor.node_id, etx, rxTime, scaledVariance,
                                     Edge::Source::Mirrored);

            // Propagate the bidirectional link flag from the authoritative sender.
            // SR nodes report their own edges — always trust their current claim.
            routingGraph->setEdgeHearsUs(mp.from, neighbor.node_id, neighbor.hears_us);
        }
    } else {
        LOG_DEBUG("[SR] Skipping redundant edge rebuild for %s (already pre-processed version %u)",
                 senderName, p->routing_version);
    }

    // Always process gateway relations and logging (even if edges were already built)
    for (pb_size_t i = 0; i < p->neighbors_count; i++) {
        const meshtastic_SignalNeighbor& neighbor = p->neighbors[i];

        if (neighbor.node_id == 0 || isPlaceholderNode(neighbor.node_id)) {
            continue;
        }

        char neighborName[64];
        getNodeDisplayName(neighbor.node_id, neighborName, sizeof(neighborName));

        float etx = NeighborGraph::calculateETX(neighbor.rssi, neighbor.snr);

        // Classify signal quality for user-friendly display
        const char* quality;
        if (etx < 2.0f) quality = "excellent";
        else if (etx < 4.0f) quality = "good";
        else if (etx < 8.0f) quality = "fair";
        else quality = "poor";

        LOG_INFO("  ├── %s: %s link (%s, ETX=%.1f, var=%u)",
                 neighborName,
                 neighbor.signal_routing_active ? "SR-active" : "SR-inactive",
                 quality, etx,
                 neighbor.position_variance);

        // If the sender is SR-capable and reports this neighbor as directly reachable,
        // clear downstream entries for this neighbor - it's now reachable via the SR network
        if (p->signal_routing_active) {
            NodeNum relayForNeighbor = routingGraph->getDownstreamRelay(neighbor.node_id);
            if (relayForNeighbor != 0 && relayForNeighbor != mp.from) {
                char gwName[64];
                getNodeDisplayName(relayForNeighbor, gwName, sizeof(gwName));
                LOG_INFO("[SR] Clearing downstream for %s (now directly reachable via %s, was via %s)",
                         neighborName, senderName, gwName);
                routingGraph->clearDownstreamForDestination(neighbor.node_id);
            }
        }
    }

    // Log network topology summary
    LOG_DEBUG("[SR] Network topology updated - %s now connected to %d neighbors",
             senderName, p->neighbors_count);

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
    NodeNum placeholderId = PLACEHOLDER_BASE | relayId;
    LOG_INFO("[SR] Created placeholder node %08x for unknown relay 0x%02x", placeholderId, relayId);
    return placeholderId;
}

bool SignalRoutingModule::resolvePlaceholder(NodeNum placeholderId, NodeNum realNodeId)
{
    if (!isPlaceholderNode(placeholderId)) {
        return false; // Not a placeholder
    }

    if (isPlaceholderNode(realNodeId)) {
        return false; // Can't resolve to another placeholder
    }

    // Check if this placeholder is already resolved to a different node
    uint8_t relayId = placeholderId & 0xFF;
    NodeNum alreadyResolved = resolveRelayIdentity(relayId);
    if (alreadyResolved != 0 && alreadyResolved != realNodeId) {
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
        LOG_DEBUG("[SR] Removed placeholder node %08x from graph", placeholderId);
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
        LOG_DEBUG("[SR] Transferred %u downstream entries from %08x to %08x",
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
            LOG_DEBUG("[SR] SR neighbor %s hears %08x and can reach destination - no relay needed",
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
            LOG_DEBUG("[SR] SR neighbor %s also hears %08x and covers all our unique neighbors",
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
                    LOG_DEBUG("[SR] Skipping active stock neighbor %08x — no confirmed bidirectional link (hearsUs=false)",
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
                        LOG_DEBUG("[SR] Stock neighbor %08x already covered by relaying SR node %08x",
                                 stockNeighbor, heardFrom);
                        break;
                    }
                }
            }
        }

        if (!heardDirectly) {
            hasUncoveredStockNeighbor = true;
            LOG_DEBUG("[SR] Stock neighbor %08x did not hear transmission directly", stockNeighbor);

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
        LOG_DEBUG("[SR] STOCK COVERAGE: Found %u uncovered stock neighbors but no valid relay path from this node", stockCount);
    }

    return false;
}



void SignalRoutingModule::logNetworkTopology()
{
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
    char nameBuf[48];
    getNodeDisplayName(ourNode, nameBuf, sizeof(nameBuf));

    // Get our direct edges
    const NodeEdges* ourEdges = routingGraph->getEdgesFrom(ourNode);
    uint8_t directCount = ourEdges ? ourEdges->edgeCount : 0;

    LOG_INFO("[SR] Network Topology: %d nodes, %u direct neighbors", nodeCount, directCount);
    LOG_INFO("[SR] %s (us)", nameBuf);

    if (!ourEdges || ourEdges->edgeCount == 0) {
        LOG_INFO("[SR]   (no direct neighbors)");
    } else {
        // Display each direct neighbor and their downstream nodes
        for (uint8_t i = 0; i < ourEdges->edgeCount; i++) {
            const Edge& edge = ourEdges->edges[i];

            getNodeDisplayName(edge.to, nameBuf, sizeof(nameBuf));

            CapabilityStatus neighborStatus = getCapabilityStatus(edge.to);
            const char* nprefix = "";
            if (neighborStatus == CapabilityStatus::SRactive) {
                nprefix = "[SR-active] ";
            } else if (neighborStatus == CapabilityStatus::Passive) {
                nprefix = "[SR-passive] ";
            }

            float etx = edge.getEtx();
            const char* quality;
            if (etx < 2.0f) quality = "excellent";
            else if (etx < 4.0f) quality = "good";
            else if (etx < 8.0f) quality = "fair";
            else quality = "poor";

            int32_t age = computeAgeSecs(edge.lastUpdate, millis() / 1000);
            char ageBuf[16];
            if (age < 0) {
                snprintf(ageBuf, sizeof(ageBuf), "-");
            } else {
                snprintf(ageBuf, sizeof(ageBuf), "%d", age);
            }

            bool isLast = (i == ourEdges->edgeCount - 1);
            const char* branch = isLast ? "\\-" : "+-";
            const char* cont = isLast ? " " : "|";

            // Get the neighbor's own edges (nodes that can hear them = their coverage)
            const NodeEdges* neighborEdges = routingGraph->getEdgesFrom(edge.to);
            uint8_t listenerCount = neighborEdges ? neighborEdges->edgeCount : 0;

            // Get downstream nodes that route through this neighbor
            static constexpr size_t MAX_DS_DISPLAY = 16;
            NodeNum dsBuf[MAX_DS_DISPLAY];
            uint16_t dsCosts[MAX_DS_DISPLAY];
            size_t totalDsCount = routingGraph->getDownstreamCountForRelay(edge.to);
            size_t dsCount = routingGraph->getDownstreamNodesForRelay(edge.to, dsBuf, dsCosts, MAX_DS_DISPLAY);

            const char* bidir = edge.hearsUs ? ", hearsUs" : "";
            LOG_INFO("[SR]   %s %s%s: %s (ETX=%.1f, %ss ago, covers %u nodes, relay for %u downstream%s)",
                     branch, nprefix, nameBuf, quality, etx, ageBuf,
                     static_cast<unsigned int>(listenerCount), static_cast<unsigned int>(totalDsCount), bidir);

            // Show nodes that can hear this neighbor (their topology-reported edges)
            if (listenerCount > 0) {
                for (uint8_t n = 0; n < listenerCount; n++) {
                    const Edge& nEdge = neighborEdges->edges[n];
                    getNodeDisplayName(nEdge.to, nameBuf, sizeof(nameBuf));

                    CapabilityStatus lStatus = getCapabilityStatus(nEdge.to);
                    const char* lprefix = "";
                    if (lStatus == CapabilityStatus::SRactive) lprefix = "[SR-active] ";
                    else if (lStatus == CapabilityStatus::Passive) lprefix = "[SR-passive] ";

                    bool lLast = (n == listenerCount - 1) && (totalDsCount == 0);
                    const char* lBranch = lLast ? "\\-" : "+-";
                    LOG_INFO("[SR]   %s    %s %s%s (ETX=%.1f)", cont, lBranch, lprefix, nameBuf, nEdge.getEtx());
                }
            }

            // Show all downstream nodes in pages of MAX_DS_DISPLAY
            if (totalDsCount > 0) {
                size_t printed = 0;
                while (printed < totalDsCount) {
                    dsCount = routingGraph->getDownstreamNodesForRelay(edge.to, dsBuf, dsCosts, MAX_DS_DISPLAY, printed);
                    if (dsCount == 0) break;
                    for (size_t d = 0; d < dsCount; d++) {
                        getNodeDisplayName(dsBuf[d], nameBuf, sizeof(nameBuf));

                        CapabilityStatus dsStatus = getCapabilityStatus(dsBuf[d]);
                        const char* dsprefix = "";
                        if (dsStatus == CapabilityStatus::SRactive) dsprefix = "[SR-active] ";
                        else if (dsStatus == CapabilityStatus::Passive) dsprefix = "[SR-passive] ";

                        bool dsLast = (printed + 1 == totalDsCount);
                        const char* dsBranch = dsLast ? "\\-" : "+-";
                        float dsCost = dsCosts[d] / 100.0f;
                        // Show Dijkstra route cost if available (more accurate than flat downstream cost)
                        Route dsRoute = routingGraph->calculateRoute(dsBuf[d], millis() / 1000);
                        if (dsRoute.nextHop != 0 && dsRoute.costFixed < 0xFFFF) {
                            float routeCost = dsRoute.getCost();
                            LOG_INFO("[SR]   %s    %s [downstream] %s%s (routeETX=%.1f, dsETX=%.1f)", cont, dsBranch, dsprefix, nameBuf, routeCost, dsCost);
                        } else {
                            LOG_INFO("[SR]   %s    %s [downstream] %s%s (dsETX=%.1f)", cont, dsBranch, dsprefix, nameBuf, dsCost);
                        }
                        printed++;
                    }
                }
            }
        }
    }

    // Add legend explaining ETX to signal quality mapping
    LOG_INFO("[SR] ETX to signal mapping: ETX=1.0~RSSI=-60dB/SNR=10dB, ETX=2.0~RSSI=-90dB/SNR=0dB, ETX=4.0~RSSI=-110dB/SNR=-5dB");
    LOG_DEBUG("[SR] Topology logging complete");
}

ProcessMessage SignalRoutingModule::handleReceived(const meshtastic_MeshPacket &mp)
{
    // Sanity check: reject packets with obviously corrupted payload sizes
    // Max valid payload is ~237 bytes for LoRa; anything over 256 is definitely garbage
    if (mp.which_payload_variant == meshtastic_MeshPacket_decoded_tag &&
        mp.decoded.payload.size > meshtastic_Constants_DATA_PAYLOAD_LEN) {
        LOG_WARN("[SR] Rejecting packet with invalid payload size: %u bytes (max %u)",
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

    // Debug logging to understand packet reception and relay state
    if (hasSignalData && notViaMqtt) {
        LOG_DEBUG("[SR] Packet from 0x%08x: relay=0x%02x, hopStart=%d, hopLimit=%d, direct=%d",
                  mp.from, mp.relay_node, mp.hop_start, mp.hop_limit, isDirectFromSender);
    }
    
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
            // Direct reception - always add to graph
            updateNeighborInfo(mp.from, mp.rx_rssi, mp.rx_snr, mp.rx_time);
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
                LOG_INFO("[SR] Direct contact: resolved placeholder %08x with node %08x", placeholderId, mp.from);
            }
        }

        rememberRelayIdentity(mp.from, fromLastByte);
        trackNodeCapability(mp.from, CapabilityStatus::Unknown);

        char senderName[64];
        getNodeDisplayName(mp.from, senderName, sizeof(senderName));

        float etx =
            NeighborGraph::calculateETX(mp.rx_rssi, mp.rx_snr);

        // NOTE: We used to clear downstream relationships when a node becomes directly reachable,
        // but this is wrong. A node can be both a direct neighbor AND a gateway for other nodes.
        // Only clear downstream relationships through aging or when relationships become invalid.

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
            LOG_DEBUG("[SR] Inactive role: Skipping relayed packet topology inference");
        } else {
            NodeNum inferredRelayer = resolveRelayIdentity(mp.relay_node);

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
                        LOG_DEBUG("[SR] Resolved relay 0x%02x to known node %08x",
                                 mp.relay_node, neighbor);
                        break;
                    }
                }
            }
        }

        // If we still can't resolve the relay identity, create a placeholder node
        if (inferredRelayer == 0) {
            inferredRelayer = createPlaceholderNode(mp.relay_node);
            LOG_DEBUG("[SR] Created placeholder %08x for unknown relay 0x%02x",
                     inferredRelayer, mp.relay_node);

            // Placeholders will be resolved when we hear the real node directly,
            // or through topology broadcasts from SR neighbors.
        }

        if (inferredRelayer != 0 && inferredRelayer != mp.from) {
            // Remember this relay identity mapping for future use (only for real nodes, not placeholders)
            if (!isPlaceholderNode(inferredRelayer)) {
                rememberRelayIdentity(inferredRelayer, mp.relay_node);
            }

            // We know that inferredRelayer relayed a packet from mp.from
            // This establishes both a gateway relationship and direct connectivity inference
            LOG_DEBUG("[SR] Inferred gateway relationship: %08x relayed by %08x",
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
            bool hasDirectConnectionToSender = false;
            const NodeEdges* edges = routingGraph->getEdgesFrom(nodeDB->getNodeNum());
            if (edges) {
                for (uint8_t i = 0; i < edges->edgeCount; i++) {
                    if (edges->edges[i].to == inferredRelayer &&
                        edges->edges[i].source == Edge::Source::Reported) {
                        hasDirectConnectionToRelay = true;
                    }
                    if (edges->edges[i].to == mp.from &&
                        edges->edges[i].source == Edge::Source::Reported) {
                        hasDirectConnectionToSender = true;
                    }
                }
            }

            if (hasDirectConnectionToRelay && !hasDirectConnectionToSender) {
                float inferredEtx = NeighborGraph::calculateETX(-70, 5.0f); // Default for inferred
                // Update downstream, replacing any existing relay for this source.
                // This ensures each source is tracked via exactly one relay at a time,
                // preventing inflated downstream counts from SR cluster re-relaying,
                // while still adapting when a node moves to a different relay path.
                routingGraph->updateDownstreamExclusive(mp.from, inferredRelayer, inferredEtx, millis() / 1000);
            }

            // Infer direct connectivity between relayer and sender only for stock firmware nodes
            // SR-aware nodes broadcast their topology, so we don't need to infer connectivity for them
            if (getCapabilityStatus(inferredRelayer) == CapabilityStatus::Legacy) {
                // Since the relayer successfully relayed a packet from the sender,
                // we can assume they have direct connectivity
                LOG_DEBUG("[SR] Inferred direct connectivity: legacy node %08x can hear %08x directly",
                         inferredRelayer, mp.from);

                // Add edge between inferredRelayer and mp.from with default signal quality
                // Use Mirrored source since this is inferred, not directly measured
                uint32_t monotonicTimestamp = millis() / 1000;
                int32_t defaultRssi = -70; // default RSSI for inferred connectivity
                float defaultSnr = 5.0f;  // default SNR for inferred connectivity

                routingGraph->updateEdge(mp.from, inferredRelayer, NeighborGraph::calculateETX(defaultRssi, defaultSnr),
                                         monotonicTimestamp, 0, Edge::Source::Mirrored);
                routingGraph->updateEdge(inferredRelayer, mp.from, NeighborGraph::calculateETX(defaultRssi, defaultSnr),
                                         monotonicTimestamp, 0, Edge::Source::Mirrored);
            } else {
                LOG_DEBUG("[SR] Skipping direct connectivity inference for SR-aware node %08x (capability: %d)",
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

                if (hasExistingEdge) {
                    LOG_DEBUG("[SR] Preserving existing signal data for relay node 0x%08x", inferredRelayer);
                } else {
                    LOG_DEBUG("[SR] Using default signal data for new relay node 0x%08x", inferredRelayer);
                }

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
        if (currentTime - lastGraphUpdate > GRAPH_UPDATE_INTERVAL_SECS) {
            uint32_t nodeCountBefore = routingGraph->getNodeCount();
            
            // Single TTL for all nodes in the graph; SR capability expiry handles coverage separately
            routingGraph->ageEdges(currentTime, NODE_TTL_SECS);
            
            uint32_t nodeCountAfter = routingGraph->getNodeCount();
            lastGraphUpdate = currentTime;

            if (nodeCountBefore != nodeCountAfter) {
                LOG_INFO("[SR] Graph aged: %u -> %u nodes", nodeCountBefore, nodeCountAfter);
                topologyDirty = true;
            } else {
                LOG_DEBUG("[SR] Graph aged (no node count change)");
            }

            // Safety check: ensure we still have our own node
            if (!routingGraph->getEdgesFrom(nodeDB->getNodeNum())) {
                LOG_WARN("[SR] Graph aging removed local node edges - topology unstable");
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

    char destName[64], heardFromName[64];
    getNodeDisplayName(destination, destName, sizeof(destName));
    getNodeDisplayName(heardFrom, heardFromName, sizeof(heardFromName));

    // If src and dst are both downstream of the same relay, that relay handles delivery.
    NodeNum sourceRelay = routingGraph->getDownstreamRelay(sourceNode);
    NodeNum destRelay = routingGraph->getDownstreamRelay(destination);
    if (sourceRelay != 0 && sourceRelay == destRelay && sourceRelay != myNode) {
        char relayName[64];
        getNodeDisplayName(sourceRelay, relayName, sizeof(relayName));
        LOG_INFO("[SR-DECISION] UNICAST SUPPRESS: to %s, src and dst both downstream of %s", destName, relayName);
        return false;
    }

    // If heardFrom already has a route to the destination, it can deliver — don't relay back.
    if (heardFrom != 0 && heardFrom != myNode && heardFrom != sourceNode) {
        bool heardFromCanReachDest = hasDirectConnectivity(heardFrom, destination) ||
                                     (routingGraph->isDownstream(destination) &&
                                      routingGraph->getDownstreamRelay(destination) == heardFrom);
        if (heardFromCanReachDest) {
            LOG_INFO("[SR-DECISION] UNICAST SUPPRESS: to %s, heardFrom %s can reach dst directly", destName, heardFromName);
            return false;
        }
        if (hasBetterPositionedSRNeighbor(myNode, heardFrom, destination)) {
            LOG_INFO("[SR-DECISION] UNICAST SUPPRESS: to %s, SR neighbor covering %s can reach dst", destName, heardFromName);
            return false;
        }
    }

    // No route → can't relay.
    NodeNum myNextHop = getNextHop(destination, sourceNode, heardFrom, false);
    if (myNextHop == 0) {
        LOG_INFO("[SR-DECISION] UNICAST SUPPRESS: to %s, no route via SR topology", destName);
        return false;
    }

    // --- Slot-based relay coordination ---
    //
    // For each candidate (self + SR-active direct neighbors that can reach destination),
    // compute a cost-to-destination:
    //   - Direct edge to destination            → etxFixed (range ~100–32767)
    //   - Edge to the shared next hop (proxy)   → etxFixed | 0x8000 (always after direct)
    //   - No usable path                        → skip
    //
    // Stock router neighbors are given sequential slots first (they transmit regardless).
    // SR candidates are then sorted ascending by cost and assigned subsequent slots.
    // Each node independently picks the same ordering; the one assigned our slot sets
    // pendingRelayDelayMs and returns true.  If a dupe arrives before our TX fires,
    // isDupeRelayRedundant / cancelSending handles cancellation.

    uint32_t halfAirtime = 150;
    if (router && router->getRadioInterface()) {
        uint32_t airtime = router->getRadioInterface()->getPacketTime(p);
        halfAirtime = std::max(airtime / 2, (uint32_t)50);
    }

    // Returns the candidate's cost to reach destination.
    // Direct edge: raw etxFixed. Indirect via shared next hop: etxFixed | 0x8000.
    auto getCandidateCost = [&](NodeNum node) -> uint16_t {
        const NodeEdges *edges = routingGraph->getEdgesFrom(node);
        if (!edges) return UINT16_MAX;
        for (uint8_t i = 0; i < edges->edgeCount; i++)
            if (edges->edges[i].to == destination)
                return edges->edges[i].etxFixed;
        if (myNextHop != destination) {
            for (uint8_t i = 0; i < edges->edgeCount; i++)
                if (edges->edges[i].to == myNextHop)
                    return edges->edges[i].etxFixed | 0x8000u;
        }
        return UINT16_MAX;
    };

    const NodeEdges *myEdges = routingGraph->getEdgesFrom(myNode);

    uint32_t slotDelay = 0;
    bool shouldRelay = false;
    uint32_t myDelay = 0;

    // Phase 1: stock router neighbors that can reach destination — sequential slots.
    if (myEdges) {
        for (uint8_t i = 0; i < myEdges->edgeCount; i++) {
            NodeNum nb = myEdges->edges[i].to;
            if (nb == heardFrom || nb == sourceNode) continue;
            if (!isImmediateRelayRouter(nb)) continue;
            if (getCandidateCost(nb) == UINT16_MAX) continue;
            if (routingGraph->hasNodeTransmitted(nb, p->id, currentTime)) {
                LOG_DEBUG("[SR] Unicast slot %ums: stock router %08x (already transmitted)", slotDelay, nb);
            } else {
                LOG_DEBUG("[SR] Unicast slot %ums: stock router %08x (expected)", slotDelay, nb);
            }
            slotDelay += halfAirtime;
        }
    }

    // Phase 2: SR candidates (including self) sorted by cost to destination.
    struct UnicastCandidate { NodeNum nodeId; uint16_t cost; };
    UnicastCandidate srCandidates[8];
    uint8_t srCount = 0;

    uint16_t myCost = getCandidateCost(myNode);
    if (myCost != UINT16_MAX && srCount < 8)
        srCandidates[srCount++] = {myNode, myCost};

    if (myEdges) {
        for (uint8_t i = 0; i < myEdges->edgeCount; i++) {
            NodeNum nb = myEdges->edges[i].to;
            if (nb == heardFrom || nb == sourceNode) continue;
            if (isLegacyRouter(nb)) continue;
            if (getCapabilityStatus(nb) != CapabilityStatus::SRactive) continue;
            uint16_t cost = getCandidateCost(nb);
            if (cost != UINT16_MAX && srCount < 8)
                srCandidates[srCount++] = {nb, cost};
        }
    }

    // Insertion sort ascending by cost.
    for (uint8_t i = 1; i < srCount; i++) {
        UnicastCandidate key = srCandidates[i];
        int8_t j = (int8_t)i - 1;
        while (j >= 0 && srCandidates[j].cost > key.cost) {
            srCandidates[j + 1] = srCandidates[j];
            j--;
        }
        srCandidates[j + 1] = key;
    }

    LOG_DEBUG("[SR] Unicast slot scheduling for pkt 0x%08x to %s: halfAirtime=%ums, %u SR candidates",
              p->id, destName, halfAirtime, srCount);

    for (uint8_t i = 0; i < srCount; i++) {
        NodeNum candidate = srCandidates[i].nodeId;
        if (routingGraph->hasNodeTransmitted(candidate, p->id, currentTime)) {
            LOG_DEBUG("[SR] Unicast slot --: SR node %08x (already transmitted)", candidate);
            continue;
        }
        if (candidate == myNode) {
            shouldRelay = true;
            myDelay = slotDelay;
            LOG_DEBUG("[SR] Unicast slot %ums: US (%08x) — assigned", slotDelay, myNode);
            break;
        }
        LOG_DEBUG("[SR] Unicast slot %ums: SR node %08x (cost=%.2f)", slotDelay, candidate,
                  srCandidates[i].cost / 100.0f);
        slotDelay += halfAirtime;
    }

    LOG_INFO("[SR-DECISION] UNICAST %s: to %s via %s (delay=%ums)",
             shouldRelay ? "RELAY" : "SUPPRESS", destName, heardFromName, shouldRelay ? myDelay : 0u);

    if (shouldRelay) {
        pendingRelayDelayMs = myDelay;
        routingGraph->recordNodeTransmission(myNode, p->id, currentTime);
    }

    return shouldRelay;
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
    LOG_DEBUG("[SR] Committed relay for packet 0x%08x (heardFrom 0x%08x, delay %ums)", packetId, originalHeardFrom, txDelayMs);
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

bool SignalRoutingModule::isDupeRelayRedundant(const meshtastic_MeshPacket *p)
{
    if (!routingGraph || !nodeDB || !p) {
        return false; // Can't evaluate — keep our relay
    }

    // Resolve the dupe's relay node
    NodeNum dupeRelayer = 0;
    if (p->relay_node != 0) {
        dupeRelayer = resolveRelayIdentity(p->relay_node);
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

    if (dupeRelayer == 0 || dupeRelayer == myNode) {
        return false; // Unknown relayer or it's us — keep our relay
    }

    char relayerName[64];
    getNodeDisplayName(dupeRelayer, relayerName, sizeof(relayerName));

    // Unicast: redundant if the dupe relayer can reach the destination.
    if (!isBroadcast(p->to)) {
        bool relayerCanReachDest = hasDirectConnectivity(dupeRelayer, p->to) ||
                                   (routingGraph->isDownstream(p->to) &&
                                    routingGraph->getDownstreamRelay(p->to) == dupeRelayer);
        if (relayerCanReachDest) {
            LOG_INFO("[SR] Unicast dupe relayer %s can reach dst — relay is redundant", relayerName);
            return true;
        }
        LOG_DEBUG("[SR] Unicast dupe relayer %s cannot confirm reach to dst — keeping relay", relayerName);
        return false;
    }

    // Broadcast: redundant if the dupe relayer's edges cover all our neighbors.
    NodeNum originalHeardFrom = 0;
    for (uint8_t i = 0; i < committedRelayCount; i++) {
        if (committedRelays[i].packetId == p->id) {
            originalHeardFrom = committedRelays[i].originalHeardFrom;
            break;
        }
    }

    NodeNum coveredBy[2];
    uint8_t coveredByCount = 0;
    if (originalHeardFrom != 0 && originalHeardFrom != myNode)
        coveredBy[coveredByCount++] = originalHeardFrom;
    if (dupeRelayer != originalHeardFrom)
        coveredBy[coveredByCount++] = dupeRelayer;
    bool unique = routingGraph->hasUniqueCoverage(myNode, coveredBy, coveredByCount);

    if (unique) {
        LOG_DEBUG("[SR] Dupe relayer %s does not cover all our neighbors — keeping relay", relayerName);
    } else {
        LOG_INFO("[SR] Dupe relayer %s covers all our neighbors — relay is redundant", relayerName);
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
    LOG_DEBUG("[SR] Considering unicast relay from %s to %s (hop_limit=%d)",
             senderName, destName, p->hop_limit);

    // Check if destination is reachable through SR topology
    if (!topologyHealthyForUnicast(p->to)) {
        // If the node exists in NodeDB, fall back to broadcast-style relay
        // This handles legacy/stock nodes not in the SR graph
        if (nodeDB->getMeshNode(p->to)) {
            LOG_INFO("[SR-DECISION] UNICAST RELAY: to %s, not routable via SR but known in NodeDB", destName);
            return true;
        }
        LOG_INFO("[SR-DECISION] UNICAST SUPPRESS: to %s, unknown destination", destName);
        return false;
    }

    NodeNum sourceNode = p->from;
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
                LOG_INFO("[SR-DECISION] UNICAST SUPPRESS: to %s, next hop %s already heard source %s",
                         destName, nextHopName, senderName);
                return false;
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
        LOG_DEBUG("[SR] Skipping relay decision for rebroadcast of our packet %08x", p->id);
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

    // Check for stock gateway nodes that can be heard directly
    // If we have stock nodes that could serve as gateways, be conservative with SR relaying
    bool hasStockGateways = false;
    bool heardFromStockGateway = false;
    if (routingGraph && nodeDB) {
        // In lite mode, check capability records for legacy nodes
        for (uint8_t i = 0; i < capabilityRecordCount; i++) {
            if (capabilityRecords[i].record.status == CapabilityStatus::Legacy) {
                hasStockGateways = true;
                if (capabilityRecords[i].nodeId == heardFrom) {
                    heardFromStockGateway = true;
                }
            }
        }
    }

    bool mustRelayForBranchCoverage = false;

    if (heardFromStockGateway) {
        LOG_DEBUG("[SR] Packet from stock gateway %08x - checking if SR neighbors also heard it", heardFrom);
        if (!hasBetterPositionedSRNeighbor(myNode, heardFrom)) {
            mustRelayForBranchCoverage = true;
            LOG_DEBUG("[SR] No better-positioned SR neighbor for stock gateway %08x - must relay", heardFrom);
        }
    }

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

    // Build coverage sets
    NodeSet alreadyCovered;
    alreadyCovered.insert(sourceNode);
    alreadyCovered.insert(heardFrom);
    const NodeEdges *heardFromEdges = routingGraph->getEdgesFrom(heardFrom);
    if (heardFromEdges) {
        for (uint8_t i = 0; i < heardFromEdges->edgeCount; i++)
            alreadyCovered.insert(heardFromEdges->edges[i].to);
    }

    // Build candidates: our direct neighbors that might relay
    NodeSet candidates;
    const NodeEdges *myEdges = routingGraph->getEdgesFrom(myNode);
    if (myEdges) {
        for (uint8_t i = 0; i < myEdges->edgeCount; i++) {
            NodeNum neighbor = myEdges->edges[i].to;
            if (neighbor == heardFrom || neighbor == sourceNode)
                continue;
            candidates.insert(neighbor);
        }
    }
    // Add ourselves
    candidates.insert(myNode);

    // Also add SR-active co-listeners that can hear heardFrom (for stock gateway case)
    if (myEdges) {
        for (uint8_t i = 0; i < myEdges->edgeCount; i++) {
            NodeNum neighbor = myEdges->edges[i].to;
            if (neighbor == heardFrom || neighbor == sourceNode)
                continue;
            if (getCapabilityStatus(neighbor) != CapabilityStatus::SRactive)
                continue;
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

    LOG_DEBUG("[SR] Slot scheduling for pkt 0x%08x: halfAirtime=%ums, %u candidates", p->id, halfAirtime, candidates.count);

    // Phase 1: Assign first slots to stock routers (they transmit regardless)
    if (myEdges) {
        for (uint8_t i = 0; i < myEdges->edgeCount; i++) {
            NodeNum neighbor = myEdges->edges[i].to;
            if (neighbor == heardFrom || neighbor == sourceNode)
                continue;
            if (!isImmediateRelayRouter(neighbor))
                continue;

            candidates.erase(neighbor);

            if (routingGraph->hasNodeTransmitted(neighbor, p->id, currentTime)) {
                const NodeEdges *ne = routingGraph->getEdgesFrom(neighbor);
                if (ne) {
                    for (uint8_t j = 0; j < ne->edgeCount; j++)
                        alreadyCovered.insert(ne->edges[j].to);
                }
                alreadyCovered.insert(neighbor);
                LOG_DEBUG("[SR] Slot %ums: stock router %08x (already transmitted)", slotDelay, neighbor);
            } else {
                LOG_DEBUG("[SR] Slot %ums: stock router %08x (expected)", slotDelay, neighbor);
            }

            slotDelay += halfAirtime;
        }
    }

    // Phase 2: Iteratively pick best SR candidate, assign slots
    while (!candidates.empty()) {
        RelayCandidate best = routingGraph->findBestRelayCandidate(candidates, alreadyCovered,
                                                                    currentTime, p->id, preferHighNodeId);
        if (best.nodeId == 0)
            break;

        candidates.erase(best.nodeId);

        if (routingGraph->hasNodeTransmitted(best.nodeId, p->id, currentTime)) {
            const NodeEdges *ne = routingGraph->getEdgesFrom(best.nodeId);
            if (ne) {
                for (uint8_t j = 0; j < ne->edgeCount; j++)
                    alreadyCovered.insert(ne->edges[j].to);
            }
            alreadyCovered.insert(best.nodeId);
            LOG_DEBUG("[SR] Slot --: SR node %08x (already transmitted, coverage absorbed)", best.nodeId);
            continue;
        }

        if (best.nodeId == myNode) {
            shouldRelay = true;
            myDelay = slotDelay;
            decisionReason = "SR slot assignment";
            LOG_DEBUG("[SR] Slot %ums: US (%08x) — assigned", slotDelay, myNode);
            break;
        }

        LOG_DEBUG("[SR] Slot %ums: SR node %08x (coverage=%u, cost=%.2f)", slotDelay, best.nodeId,
                  best.coverageCount, best.getAvgCost());
        slotDelay += halfAirtime;
    }

    // Phase 3: If we weren't assigned a slot, check unique coverage
    if (!shouldRelay && myEdges) {
        for (uint8_t i = 0; i < myEdges->edgeCount; i++) {
            NodeNum neighbor = myEdges->edges[i].to;
            if (!alreadyCovered.contains(neighbor)) {
                shouldRelay = true;
                // Hash-based delay for unique coverage relays
                uint32_t hash = myNode ^ p->id ^ (myNode >> 16) ^ (p->id >> 16);
                myDelay = slotDelay + (hash % 100) * 20;
                decisionReason = "unique coverage";
                LOG_DEBUG("[SR] Unique coverage: neighbor %08x uncovered, hash delay %ums", neighbor, myDelay);
                break;
            }
        }
    }

    // Phase 4: Force relay if we are the recorded downstream relay for source
    if (!shouldRelay && (weAreRelayForSource || weAreRelayForDest)) {
        NodeNum forcedFor = weAreRelayForSource ? sourceNode : p->to;
        LOG_INFO("[SR-DECISION] BROADCAST RELAY (forced): we are relay for %08x (downstream=%u)",
                 forcedFor, static_cast<unsigned int>(downstreamCount));
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

    LOG_INFO("[SR-DECISION] BROADCAST %s: from %s via %s (%s, delay=%ums)",
             shouldRelay ? "RELAY" : "SUPPRESS", sourceName, heardFromName,
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
        LOG_DEBUG("[SR] No graph available for routing");
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

        LOG_DEBUG("[SR] Route to %s via %s (cost: %.2f)",
                 destName, nextHopName, routeCost);

        if (routeCost > 10.0f) {
            LOG_WARN("[SR] High-cost route to %s (%.2f) - poor link quality expected",
                    destName, routeCost);
        }

        // CRITICAL: Verify the next hop can hear the transmitting node (heardFrom)
        // If heardFrom is known and next hop didn't hear the transmission, it can't relay
        bool nextHopCanHearTransmitter = true;  // Assume true if we can't verify
        bool connectivityUnknown = false;
        
        if (heardFrom != 0 && route.nextHop != heardFrom) {
            // Use enhanced connectivity check that handles stock firmware nodes
            nextHopCanHearTransmitter = hasVerifiedConnectivity(heardFrom, route.nextHop, &connectivityUnknown);
            
            if (!nextHopCanHearTransmitter) {
                char heardFromName[64];
                getNodeDisplayName(heardFrom, heardFromName, sizeof(heardFromName));
                
                if (connectivityUnknown) {
                    // Stock node involved - we can't verify connectivity
                    // Be conservative: don't trust unverified relays, relay ourselves
                    LOG_DEBUG("[SR] Route via %s rejected - cannot verify connectivity to %s (stock/unknown node)",
                             nextHopName, heardFromName);
                } else {
                    // Both are SR-active but no edge exists - they likely can't hear each other
                    LOG_DEBUG("[SR] Route via %s rejected - no connectivity to transmitter %s",
                             nextHopName, heardFromName);
                }
                // Don't return this next hop - fall through to try alternatives
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
        
        // Next hop can't hear transmitter - try to find alternative through opportunistic routing
        if (allowOpportunistic) {
            NodeNum betterNeighbor = findBetterPositionedNeighbor(destination, sourceNode, heardFrom, 
                                                                  std::numeric_limits<float>::infinity(), currentTime);
            if (betterNeighbor != 0) {
                char altName[64];
                getNodeDisplayName(betterNeighbor, altName, sizeof(altName));
                LOG_DEBUG("[SR] Using alternative next hop %s (can hear transmitter)", altName);
                return betterNeighbor;
            }
        }
        
        // No alternative found - indicate we should relay ourselves
        // by returning our own node number
        NodeNum myNode = nodeDB ? nodeDB->getNodeNum() : 0;
        if (myNode != 0) {
            LOG_DEBUG("[SR] No next hop can hear transmitter - we should relay ourselves");
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
                        LOG_DEBUG("[SR] No direct route to %s, but forwarding to relay %s", destName, gwName);
                        return relayForDest;
                    }
                }
            }
        } else {
            char gwName[64], heardFromName[64];
            getNodeDisplayName(relayForDest, gwName, sizeof(gwName));
            getNodeDisplayName(heardFrom, heardFromName, sizeof(heardFromName));
            if (connectivityUnknown) {
                LOG_DEBUG("[SR] Relay %s connectivity to %s unknown (stock node) - skipping", gwName, heardFromName);
            } else {
                LOG_DEBUG("[SR] Relay %s cannot hear transmitter %s - skipping", gwName, heardFromName);
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
            LOG_DEBUG("[SR] Delivering unicast to direct neighbor %s (ETX=%.2f) since destination didn't hear transmission",
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

    LOG_DEBUG("[SR] No route found to %s", destName);
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
        LOG_DEBUG("[SR] Found better positioned neighbor %s for %s (our cost: %.2f, neighbor direct ETX: %.2f)",
                 nhName, destName, ourRouteCost, bestNeighborRouteCost);
    }

    return bestNeighbor;
}

void SignalRoutingModule::updateNeighborInfo(NodeNum nodeId, int32_t rssi, float snr, uint32_t lastRxTime, uint32_t variance)
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
        routingGraph->updateEdge(nodeId, myNode, etx, monotonicTimestamp, variance, Edge::Source::Reported);

    // Also store reverse edge: us → nodeId (assuming approximately symmetric link)
    // Since we directly measured the link quality (even if in the opposite direction),
    // mark this as Reported source, not Mirrored
    routingGraph->updateEdge(myNode, nodeId, etx, monotonicTimestamp, variance
                             , Edge::Source::Reported
                             );

    // If significant change, consider sending an update sooner
    if (changeType != EDGE_NO_CHANGE) {
        char neighborName[64];
        getNodeDisplayName(nodeId, neighborName, sizeof(neighborName));

        if (changeType == EDGE_NEW) {
            // We now have a DIRECT connection to this node - clear any downstream entries
            // that were created based on topology broadcasts before we heard from them directly
            routingGraph->clearDownstreamForDestination(nodeId);

            LOG_INFO("[SR] Topology changed: new neighbor %s (total nodes: %u)", neighborName, static_cast<unsigned int>(routingGraph->getNodeCount()));
            topologyDirty = true;
        } else if (changeType == EDGE_SIGNIFICANT_CHANGE) {
            LOG_INFO("[SR] Topology changed: ETX change for %s (total nodes: %u)", neighborName, static_cast<unsigned int>(routingGraph->getNodeCount()));
            topologyDirty = true;
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
        NodeNum relayNodeId = resolveRelayIdentity(p->relay_node);
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
        trackNodeCapability(mp.from, CapabilityStatus::Legacy);
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

    char senderName[64];
    getNodeDisplayName(mp.from, senderName, sizeof(senderName));

    double latitude = position.has_latitude_i ? position.latitude_i / 1e7 : 0.0;
    double longitude = position.has_longitude_i ? position.longitude_i / 1e7 : 0.0;
    uint32_t dop = position.PDOP;
    uint32_t speed = position.has_ground_speed ? position.ground_speed : 0;

    LOG_DEBUG("[SR] Position packet from %s (direct=%s) lat=%.5f lon=%.5f speed=%u m/s PDOP=%u "
              "rssi=%d snr=%.1f",
              senderName, isDirectNeighbor ? "true" : "false", latitude, longitude, speed, dop, mp.rx_rssi, mp.rx_snr);

    if (isDirectNeighbor && mp.rx_rssi != 0) {
        uint32_t variance = 0;
        if (position.gps_accuracy && position.PDOP) {
            uint32_t dopFactor = std::max<uint32_t>(1, position.PDOP / 100);
            variance = std::min<uint32_t>(3000, (position.gps_accuracy / 1000) * dopFactor);
        } else if (position.has_ground_speed && position.ground_speed) {
            variance = std::min<uint32_t>(3000, position.ground_speed * 5);
        }

        if (variance > 0) {
            updateNeighborInfo(mp.from, mp.rx_rssi, mp.rx_snr, mp.rx_time, variance);
        }
    }
}

void SignalRoutingModule::handleTelemetryPacket(const meshtastic_MeshPacket &mp)
{
    meshtastic_Telemetry telemetry = meshtastic_Telemetry_init_zero;
    if (!pb_decode_from_bytes(mp.decoded.payload.bytes, mp.decoded.payload.size, &meshtastic_Telemetry_msg, &telemetry)) {
        return;
    }

    char senderName[64];
    getNodeDisplayName(mp.from, senderName, sizeof(senderName));

    switch (telemetry.which_variant) {
    case meshtastic_Telemetry_device_metrics_tag: {
        const meshtastic_DeviceMetrics &metrics = telemetry.variant.device_metrics;
        int battery = metrics.has_battery_level ? static_cast<int>(metrics.battery_level) : 0;
        float voltage = metrics.has_voltage ? metrics.voltage : 0.0f;
        float air = metrics.has_air_util_tx ? metrics.air_util_tx : 0.0f;
        LOG_DEBUG("[SR] Device metrics from %s batt=%s%d%% volt=%s%.2fV airUtil=%s%.1f%%",
                  senderName, metrics.has_battery_level ? "" : "~", battery, metrics.has_voltage ? "" : "~", voltage,
                  metrics.has_air_util_tx ? "" : "~", air);
        break;
    }
    case meshtastic_Telemetry_environment_metrics_tag: {
        const meshtastic_EnvironmentMetrics &env = telemetry.variant.environment_metrics;
        LOG_DEBUG("[SR] Environment metrics from %s temp=%s%.1fC humidity=%s%.1f%% pressure=%s%.1fhPa",
                  senderName, env.has_temperature ? "" : "~",
                  env.has_temperature ? env.temperature : 0.0f, env.has_relative_humidity ? "" : "~",
                  env.has_relative_humidity ? env.relative_humidity : 0.0f, env.has_barometric_pressure ? "" : "~",
                  env.has_barometric_pressure ? env.barometric_pressure : 0.0f);
        break;
    }
    case meshtastic_Telemetry_air_quality_metrics_tag:
    case meshtastic_Telemetry_power_metrics_tag:
    case meshtastic_Telemetry_local_stats_tag:
    case meshtastic_Telemetry_health_metrics_tag:
    case meshtastic_Telemetry_host_metrics_tag:
        LOG_DEBUG("[SR] Telemetry variant %u from %s", telemetry.which_variant, senderName);
        break;
    default:
        LOG_DEBUG("[SR] Unknown telemetry variant %u from %s", telemetry.which_variant, senderName);
        break;
    }

    CapabilityStatus currentStatus = getCapabilityStatus(mp.from);
    if (currentStatus != CapabilityStatus::Unknown) {
        // Only refresh timestamp for nodes with known capability status
        // Unknown nodes stay unknown until they prove their capability via SR packets
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
        LOG_DEBUG("[SR] Routing request from %s with %u hops recorded", senderName,
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
                        LOG_DEBUG("[SR] Skipping traceroute resolution: %08x is not a direct neighbor", hopNode);
                    }
                }
            }
        } else {
            LOG_DEBUG("[SR] Skipping placeholder resolution for route_request: packet is relayed (not from direct sender)");
        }
        break;
    case meshtastic_Routing_route_reply_tag:
        LOG_DEBUG("[SR] Routing reply from %s for %u hops", senderName, routing.route_reply.route_back_count);

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
                        LOG_DEBUG("[SR] Skipping traceroute resolution: %08x is not a direct neighbor", hopNode);
                    }
                }
            }
        } else {
            LOG_DEBUG("[SR] Skipping placeholder resolution for route_reply: packet is relayed (not from direct sender)");
        }
        break;
    case meshtastic_Routing_error_reason_tag:
        if (routing.error_reason == meshtastic_Routing_Error_NONE) {
            LOG_DEBUG("[SR] Routing status from %s (no error)", senderName);
        } else {
            LOG_WARN("[SR] Routing error from %s reason=%u", senderName, routing.error_reason);
        }
        break;
    default:
        LOG_DEBUG("[SR] Routing control variant %u from %s", routing.which_variant, senderName);
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
    LOG_DEBUG("[SR] Topology healthy for broadcast");
    if (!routingGraph || !nodeDB) {
        LOG_DEBUG("[SR] routingGraph or nodeDB is null, returning false");
        return false;
    }

    // Check if we have direct SR-capable neighbors for intelligent broadcast routing
    LOG_DEBUG("[SR] Checking direct neighbors");

    const NodeEdges* nodeEdges = routingGraph->getEdgesFrom(nodeDB->getNodeNum());
    if (!nodeEdges || nodeEdges->edgeCount == 0) {
        LOG_DEBUG("[SR] No edges found, returning false");
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
        LOG_DEBUG("[SR] Node %08x is reachable through topology (nextHop=%08x, cost=%.2f)",
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
            LOG_DEBUG("[SR] Node %08x is reachable via relay %08x (nextHop=%08x, cost=%.2f)",
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

NodeNum SignalRoutingModule::resolveRelayIdentity(uint8_t relayId) const
{
    uint32_t nowMs = millis();
    NodeNum bestNode = 0;
    uint32_t newest = 0;

    for (uint8_t b = 0; b < relayIdentityCacheCount; b++) {
        if (relayIdentityCache[b].relayId == relayId) {
            const RelayIdentityCacheEntry *bucket = &relayIdentityCache[b];
            for (uint8_t i = 0; i < bucket->entryCount; i++) {
                if ((nowMs - bucket->entries[i].lastHeardMs) > RELAY_ID_CACHE_TTL_MS) {
                    continue;
                }
                if (bucket->entries[i].lastHeardMs >= newest) {
                    newest = bucket->entries[i].lastHeardMs;
                    bestNode = bucket->entries[i].nodeId;
                }
            }
            break;
        }
    }

    // Don't return placeholders - they should be resolved to real nodes
    if (isPlaceholderNode(bestNode)) {
        return 0;
    }
    return bestNode;
}



NodeNum SignalRoutingModule::resolveHeardFrom(const meshtastic_MeshPacket *p, NodeNum sourceNode) const
{
    if (!p) {
        return sourceNode;
    }

    if (p->relay_node == 0) {
        return sourceNode;
    }

    if ((sourceNode & 0xFF) == p->relay_node) {
        return sourceNode;
    }

    NodeNum resolved = resolveRelayIdentity(p->relay_node);
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

