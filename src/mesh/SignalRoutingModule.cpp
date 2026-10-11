#include "SignalRoutingModule.h"
#include "DisplayFormatters.h"

#if !MESHTASTIC_EXCLUDE_SIGNALROUTING

#include "ChannelQoS.h"
#include "MeshService.h"
#include "MeshTypes.h"
#include "NodeDB.h"
#include "NodeRateLimiter.h"
#include "RTC.h"
#include "Router.h"
#include "configuration.h"
#include "graph/NeighborGraph.h"
#include "memGet.h"
#include "modules/RoutingModule.h"
#include "pb_decode.h"
#include <Arduino.h>
#include <algorithm>
#include <cmath>

SignalRoutingModule *signalRoutingModule;

SignalRoutingModule::TopologyVersionEntry *SignalRoutingModule::findTopologyEntry(TopologyVersionEntry *table, uint8_t count,
                                                                                  NodeNum nodeId)
{
    for (uint8_t i = 0; i < count; i++) {
        if (table[i].nodeId == nodeId)
            return &table[i];
    }
    return nullptr;
}

uint8_t SignalRoutingModule::getTopologyVersion(const TopologyVersionEntry *table, uint8_t count, NodeNum nodeId,
                                                uint32_t *lastAcceptMs) const
{
    const TopologyVersionEntry *e = findTopologyEntry(const_cast<TopologyVersionEntry *>(table), count, nodeId);
    if (lastAcceptMs)
        *lastAcceptMs = e ? e->lastAcceptMs : 0;
    return e ? e->version : 0;
}

bool SignalRoutingModule::getTopologyStale(const TopologyVersionEntry *table, uint8_t count, NodeNum nodeId,
                                           uint8_t *staleVersion) const
{
    const TopologyVersionEntry *e = findTopologyEntry(const_cast<TopologyVersionEntry *>(table), count, nodeId);
    if (!e)
        return false;
    if (staleVersion)
        *staleVersion = e->staleVersion;
    return e->staleValid;
}

void SignalRoutingModule::noteTopologyStale(TopologyVersionEntry *table, uint8_t count, NodeNum nodeId, uint8_t received)
{
    TopologyVersionEntry *e = findTopologyEntry(table, count, nodeId);
    if (!e)
        return;
    e->staleVersion = received;
    e->staleValid = true;
}

void SignalRoutingModule::setTopologyVersion(TopologyVersionEntry *table, uint8_t &count, NodeNum nodeId, uint8_t version,
                                             uint32_t nowMs, PacketId packetId, uint32_t chunkBit)
{
    if (TopologyVersionEntry *e = findTopologyEntry(table, count, nodeId)) {
        // Delayed copies and continuation chunks must not erase evidence of a restarted counter.
        const bool sameVersion = (e->version == version);
        e->version = version;
        e->packetId = packetId;
        e->processedChunks = chunkBit ? ((sameVersion ? e->processedChunks : 0) | chunkBit) : 0;
        if (nowMs)
            e->lastAcceptMs = nowMs;
        if (!sameVersion)
            e->staleValid = false;
        return;
    }
    TopologyVersionEntry *slot;
    if (count < MAX_TOPOLOGY_VERSION_ENTRIES) {
        slot = &table[count];
        count++;
    } else {
        slot = &table[0];
        for (uint8_t i = 1; i < count; i++) {
            if (nowMs - table[i].lastAcceptMs > nowMs - slot->lastAcceptMs) {
                slot = &table[i];
            }
        }
    }
    *slot = {};
    slot->nodeId = nodeId;
    slot->version = version;
    slot->packetId = packetId;
    slot->processedChunks = chunkBit;
    slot->lastAcceptMs = nowMs;
    slot->staleValid = false;
}

SrTopologyVerdict SignalRoutingModule::gateTopologyVersion(NodeNum from, uint8_t receivedVersion, bool bootBroadcast,
                                                           bool directCompleteList, uint32_t nowMs)
{
    uint32_t lastAcceptMs = 0;
    uint8_t lastProcessedVersion = getTopologyVersion(lastTopologyVersion, lastTopologyVersionCount, from, &lastAcceptMs);
    uint8_t staleVersion = 0;
    bool staleValid = getTopologyStale(lastTopologyVersion, lastTopologyVersionCount, from, &staleVersion);
    SrTopologyVerdict verdict =
        srTopologyVersionVerdict(receivedVersion, lastProcessedVersion, lastAcceptMs, nowMs, topologyResyncMs(), bootBroadcast,
                                 staleValid, staleVersion, directCompleteList);
    if (verdict == SrTopologyVerdict::Stale) {
        noteTopologyStale(lastTopologyVersion, lastTopologyVersionCount, from, receivedVersion);
        LOG_INFO("[SR] Ignoring stale topology from %08x (v%u, last %u)", from, receivedVersion, lastProcessedVersion);
        return verdict;
    }
    if (verdict == SrTopologyVerdict::BootReset || verdict == SrTopologyVerdict::SilenceResync ||
        verdict == SrTopologyVerdict::RestartClimb || verdict == SrTopologyVerdict::DirectResync ||
        verdict == SrTopologyVerdict::CounterReset) {
        const char *why = "versions climbing after a lost boot broadcast";
        if (verdict == SrTopologyVerdict::BootReset) {
            why = "boot broadcast";
        } else if (verdict == SrTopologyVerdict::SilenceResync) {
            why = "silent peer";
        } else if (verdict == SrTopologyVerdict::DirectResync) {
            why = "direct originator list";
        } else if (verdict == SrTopologyVerdict::CounterReset) {
            why = "counter restarted";
        }
        LOG_INFO("[SR] Topology version resync from %08x: received %u, last %u (%s)", from, receivedVersion, lastProcessedVersion,
                 why);
    }
    setTopologyVersion(lastTopologyVersion, lastTopologyVersionCount, from, bootBroadcast ? 0 : receivedVersion, nowMs);
    return verdict;
}

NeighborGraph::CoveragePolicy SignalRoutingModule::coveragePolicy() const
{
    NeighborGraph::CoveragePolicy p;
    p.ctx = const_cast<SignalRoutingModule *>(this);
    p.publishesTopology = [](void *c, NodeNum n) { return static_cast<const SignalRoutingModule *>(c)->publishesTopology(n); };
    p.isStockRelayRouter = [](void *c, NodeNum n) {
        return static_cast<const SignalRoutingModule *>(c)->isImmediateRelayRouter(n);
    };
    p.isSrActive = [](void *c, NodeNum n) {
        return static_cast<const SignalRoutingModule *>(c)->getCapabilityStatus(n) == CapabilityStatus::SRactive;
    };
    p.isConfiguredRouter = [](void *, NodeNum n) {
        if (!nodeDB) {
            return false;
        }
        const meshtastic_NodeInfoLite *info = nodeDB->getMeshNode(n);
        return info && info->has_user && info->user.role == meshtastic_Config_DeviceConfig_Role_ROUTER;
    };
    p.me = nodeDB ? nodeDB->getNodeNum() : 0;
    p.meRelays = isActiveRoutingRole();
    p.poorLinkEtx = cfgPoorLinkEtxThreshold;
    p.nowSecs = nowSeconds();
    p.publisherSilenceSecs = PUBLISHER_SILENCE_SECS;
    p.isDroppedCoverageTarget = [](void *, NodeNum n) { return nodeRateLimiter && nodeRateLimiter->isDroppedCoverageTarget(n); };
    return p;
}

NeighborGraph::RoutePolicy SignalRoutingModule::routePolicy() const
{
    NeighborGraph::RoutePolicy p;
    p.ctx = const_cast<SignalRoutingModule *>(this);
    p.routable = [](void *ctx, NodeNum n) { return static_cast<const SignalRoutingModule *>(ctx)->isNodeRoutable(n); };
    p.publishes = [](void *ctx, NodeNum n) { return static_cast<const SignalRoutingModule *>(ctx)->publishesTopology(n); };
    return p;
}

bool SignalRoutingModule::isHandOffDuplicate(const meshtastic_MeshPacket *p)
{
    if (!p || !nodeDB || !routingGraph) {
        return false;
    }
    if (isBroadcast(p->to) || isToUs(p) || isFromUs(p)) {
        return false;
    }
    if (p->hop_limit == 0) {
        return false;
    }
    uint8_t ourByte = nodeDB->getLastByteOfNodeNum(nodeDB->getNodeNum());
    if (p->next_hop != ourByte) {
        return false;
    }
    if (!isActiveRoutingRole() || !shouldUseSignalBasedRouting(p)) {
        return false;
    }
    if (routingGraph->hasNodeTransmitted(nodeDB->getNodeNum(), p->id, nowSeconds())) {
        return false;
    }
    return true;
}

bool SignalRoutingModule::isOriginatorRetryReplan(const meshtastic_MeshPacket *p)
{
    if (!p || !nodeDB || !routingGraph) {
        return false;
    }
    if (isBroadcast(p->to) || isToUs(p) || isFromUs(p)) {
        return false;
    }
    if (p->hop_limit == 0 || !isDirectPacket(*p)) {
        return false;
    }
    uint8_t ourByte = nodeDB->getLastByteOfNodeNum(nodeDB->getNodeNum());
    if (p->next_hop != NO_NEXT_HOP_PREFERENCE && p->next_hop != ourByte) {
        return false;
    }
    if (routingGraph->hasNodeTransmitted(nodeDB->getNodeNum(), p->id, nowSeconds())) {
        return false;
    }
    if (isCommittedRelay(p->id)) {
        return false;
    }
    return true;
}

NodeNum SignalRoutingModule::alternateNextHop(NodeNum dest, const NodeNum *excluded, uint8_t n, uint8_t nominatedByte)
{
    if (!routingGraph || !nodeDB || dest == 0) {
        return 0;
    }
    NeighborGraph::RoutePolicy policy = routePolicy();
    policy.excluded = excluded;
    policy.excludedCount = n;
    uint32_t currentTime = nowSeconds();
    Route route = routingGraph->calculateRoute(dest, currentTime, policy);
    if (route.nextHop == 0) {
        return 0;
    }
    // Stock peers remember the first designation, so only an SR alternate can accept a changed one.
    if (nodeDB->getLastByteOfNodeNum(route.nextHop) == nominatedByte ||
        getCapabilityStatus(route.nextHop) != CapabilityStatus::SRactive) {
        return 0;
    }
    bool stampable = route.verified;
    if (!stampable) {
        NodeNum me = nodeDB->getNodeNum();
        stampable = me != 0 && routingGraph->downstreamChainEgress(dest, me).node == route.nextHop;
    }
    return stampable ? route.nextHop : 0;
}

NodeNum SignalRoutingModule::nominatedHopFor(NodeNum flagged, uint8_t nextHopByte) const
{
    if (flagged != 0 && nodeDB && nodeDB->getLastByteOfNodeNum(flagged) == nextHopByte) {
        return flagged;
    }
    return resolveRelayIdentity(nextHopByte);
}

void SignalRoutingModule::recordHopMiss(NodeNum destination, NodeNum nextHop)
{
    if (hopHealth.recordMiss(destination, nextHop, millis())) {
        LOG_INFO("[SR] Next hop !%08x suspect for !%08x (misses=%u)", nextHop, destination,
                 hopHealth.missCount(destination, nextHop));
    }
}

void SignalRoutingModule::recordHopSuccess(NodeNum destination, NodeNum nextHop)
{
    if (hopHealth.recordSuccess(destination, nextHop)) {
        LOG_INFO("[SR] Next hop !%08x healthy for !%08x", nextHop, destination);
    }
}

static void getNodeDisplayName(NodeNum nodeId, char *buf, size_t bufSize)
{
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

static int32_t computeAgeSecs(uint32_t last, uint32_t now)
{
    static constexpr uint32_t MAX_AGE_DISPLAY_SEC = 30 * 24 * 60 * 60;
    if (!last)
        return -1;

    if (last > now + 86400)
        return -1;
    int32_t age = static_cast<int32_t>(now - last);
    if (age < 0)
        age = 0;
    if (static_cast<uint32_t>(age) > MAX_AGE_DISPLAY_SEC)
        return -1;
    return age;
}

bool SignalRoutingModule::isDirectPacket(const meshtastic_MeshPacket &mp)
{
    if (mp.hop_start != mp.hop_limit)
        return false;
    // Some stock relays preserve hop_limit, so hop counts alone cannot prove direct reception.
    if (mp.relay_node != 0 && mp.relay_node != (mp.from & 0xFF))
        return false;
    return true;
}

SignalRoutingModule::SignalRoutingModule()
    : ProtobufModule("SignalRouting", meshtastic_PortNum_SIGNAL_ROUTING_APP, &meshtastic_SignalRoutingInfo_msg),
      concurrency::OSThread("SignalRouting")
{
#ifdef ARCH_STM32WL

    LOG_INFO("[SR] Disabled on STM32WL (insufficient RAM)");
    routingGraph = nullptr;
    disable();
    return;
#endif

#ifdef ARCH_RP2040

    uint32_t freeHeap = memGet.getFreeHeap();
    if (freeHeap < 30 * 1024) {
        LOG_WARN("[SR] RP2040 RAM low (%u free), SR disabled", freeHeap);
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

    if (moduleConfig.has_signal_routing) {
        const auto &srCfg = moduleConfig.signal_routing;
        signalBasedRoutingEnabled = srCfg.enabled;
        t1RetransmitEnabled = srCfg.t1_retransmit_enabled;
        if (srCfg.topology_broadcast_secs != 0) {
            cfgBroadcastSecs = std::min<uint32_t>(srCfg.topology_broadcast_secs, (INT32_MAX - 20000u) / 2000u);
        }
        if (srCfg.dirty_broadcast_secs != 0) {
            cfgDirtyBroadcastSecs = std::min<uint32_t>(srCfg.dirty_broadcast_secs, INT32_MAX / 1000u);
        }
        if (srCfg.node_ttl_secs != 0) {
            cfgNodeTtlSecs = srCfg.node_ttl_secs;
        }
        if (srCfg.broadcast_max_hops != 0) {
            cfgBroadcastMaxHops = std::min<uint32_t>(srCfg.broadcast_max_hops, HOP_MAX);
        }
        if (std::isfinite(srCfg.poor_link_etx_threshold) && srCfg.poor_link_etx_threshold > 0.0f) {
            cfgPoorLinkEtxThreshold = srCfg.poor_link_etx_threshold;
        }
        if (std::isfinite(srCfg.etx_change_threshold) && srCfg.etx_change_threshold > 0.0f) {
            routingGraph->setEtxChangeThreshold(srCfg.etx_change_threshold);
        }
        LOG_INFO("[SR] Cfg: en=%d t1=%d bcast=%us dirty=%us ttl=%us hops=%u etx=%.1f d=%.2f", signalBasedRoutingEnabled,
                 t1RetransmitEnabled, cfgBroadcastSecs, cfgDirtyBroadcastSecs, cfgNodeTtlSecs, cfgBroadcastMaxHops,
                 cfgPoorLinkEtxThreshold, routingGraph->getEtxChangeThreshold());
    }

    trackNodeCapability(nodeDB->getNodeNum(), CapabilityStatus::SRactive);

    isPromiscuous = true;
    // Undecryptable RF frames still supply direct-link evidence from their clear headers.
    encryptedOk = true;

    // Caching the initial preset ensures the first live change invalidates the old graph.
    operatingPresetKnown = true;
    operatingPreset = static_cast<uint8_t>(config.lora.modem_preset);

    needsBootBroadcast = true;
    setIntervalFromNow(5 * 1000);

    LOG_INFO("[SR] Module initialized (version %d)", SIGNAL_ROUTING_VERSION);
}

SignalRoutingModule::~SignalRoutingModule()
{
    for (auto &pending : pendingRetransmits) {
        if (pending.packet) {
            packetPool.release(pending.packet);
        }
    }
    delete routingGraph;
    if (signalRoutingModule == this) {
        signalRoutingModule = nullptr;
    }
}

uint32_t SignalRoutingModule::nowSeconds() const
{
    return routingGraph ? routingGraph->nowSeconds() : 0;
}

void SignalRoutingModule::maintainGraph(uint32_t currentTime)
{
    // RF silence must age the graph just as incoming traffic does.

    if (routingGraph && canSendTopology()) {
        if (currentTime - lastGraphUpdate >= GRAPH_MAINTENANCE_INTERVAL_SECS) {
            uint32_t nodeCountBefore = routingGraph->getNodeCount();

            uint8_t directBefore = routingGraph->countDirectNeighbors();
            routingGraph->ageEdges(currentTime, cfgNodeTtlSecs);
            NeighborGraph::CoveragePolicy livenessPolicy = coveragePolicy();
            if (routingGraph->pruneSilentPublishers(nodeDB->getNodeNum(), currentTime, PUBLISHER_SILENCE_SECS, &livenessPolicy) >
                0) {
                markTopologyDirty();
            }

            uint32_t nodeCountAfter = routingGraph->getNodeCount();
            lastGraphUpdate = currentTime;

            if (nodeCountBefore != nodeCountAfter) {
                LOG_INFO("[SR] Graph aged: %u -> %u nodes", nodeCountBefore, nodeCountAfter);
            } else {
                LOG_INFO("[SR] Graph aged (no node count change)");
            }

            if (routingGraph->countDirectNeighbors() != directBefore) {
                markTopologyDirty();
            }
            syncDirectSignalsWithGraph();

            if (!routingGraph->getEdgesFrom(nodeDB->getNodeNum())) {
                LOG_WARN("[SR] Graph aging removed local node edges - topology unstable");
            }
        }
    }
}

void SignalRoutingModule::wakeWithin(uint32_t delayMs)
{
    // A later request must not postpone an already scheduled bootstrap or insurance copy.
    const int32_t remaining = static_cast<int32_t>(static_cast<uint32_t>(_cached_next_run) - millis());
    if (remaining > 0 && delayMs < static_cast<uint32_t>(remaining)) {
        setIntervalFromNow(delayMs);
    }
}

void SignalRoutingModule::markTopologyDirty()
{
    if (!topologyDirty) {
        topologyDirty = true;

        int32_t minDelay = (int32_t)(cfgDirtyBroadcastSecs * 1000);
        int32_t age = sinceLastBroadcast(millis());
        uint32_t wakeIn = (age >= minDelay) ? 0 : (uint32_t)(minDelay - (age > 0 ? age : 0));
        wakeWithin(wakeIn);
    }
}

void SignalRoutingModule::commitTopologyTxTimestamp()
{
    lastBroadcast = millis();
}

void SignalRoutingModule::purgeGraphForPresetChange()
{
    if (routingGraph) {
        routingGraph->purgeForPresetChange();
    }
    directSignalCount = 0;
    capabilityRecordCount = 0;
    relayIdentityCacheCount = 0;
    lastTopologyVersionCount = 0;
    lastPreProcessedVersionCount = 0;
    for (uint8_t i = 0; i < PENDING_LISTED_SLOTS; i++) {
        pendingListed[i].valid = false;
    }
    currentTopologyVersion = 0;
    lastTopologyListMs = 0;
    lastBroadcast = 0;
    topologyDirty = false;
    pendingTopologyReply.active = false;
    needsBootBroadcast = true;
    committedRelayCount = 0;
    pendingRelayDelayMs = 0;
    pendingUnicastNextHop = 0;
    pendingUnicastFlags = {};
    for (uint8_t i = 0; i < MAX_COMMITTED_RELAYS; i++) {
        committedRelays[i] = CommittedRelay();
    }
    for (uint8_t i = 0; i < MAX_PENDING_RETRANSMITS; i++) {
        if (pendingRetransmits[i].packet) {
            packetPool.release(pendingRetransmits[i].packet);
            pendingRetransmits[i].packet = nullptr;
        }
        pendingRetransmits[i] = PendingRetransmit();
    }
    setIntervalFromNow(0);
}

void SignalRoutingModule::radioReconfigured()
{
    if (srObserveModemPreset(operatingPresetKnown, operatingPreset, static_cast<uint8_t>(config.lora.modem_preset))) {
        LOG_INFO("[SR] Modem preset changed — purging graph");
        purgeGraphForPresetChange();
    }
}

void SignalRoutingModule::scheduleEmptyTopologyReply(NodeNum senderNodeId, PacketId packetId)
{
    if (!nodeDB || !canSendTopology()) {
        return;
    }

    if (lastBootstrapReplyMs != 0 && millis() - lastBootstrapReplyMs < BOOTSTRAP_REPLY_MIN_INTERVAL_MS) {
        LOG_INFO("[SR] Bootstrap reply rate-limited (%ums, %08x)", millis() - lastBootstrapReplyMs, senderNodeId);
        return;
    }
    NodeNum ourNode = nodeDB->getNodeNum();
    uint32_t delayMs = computeEmptyTopologyReplyDelayMs(senderNodeId, packetId, ourNode);
    uint32_t fireAfter = millis() + delayMs;

    if (!pendingTopologyReply.active || (int32_t)(fireAfter - pendingTopologyReply.fireAfterMs) < 0) {
        pendingTopologyReply.active = true;
        pendingTopologyReply.fireAfterMs = fireAfter;
        pendingTopologyReply.requestedMs = millis();
        LOG_INFO("[SR] Topology reply in %ums (bootstrap from %08x)", delayMs, senderNodeId);
        wakeWithin(std::max<uint32_t>(20, delayMs));
    }
}

bool SignalRoutingModule::hasReportedDirectEdge(NodeNum neighborId) const
{
    if (!routingGraph || !nodeDB) {
        return false;
    }
    return hasReportedDirectEdgeTo(routingGraph, nodeDB->getNodeNum(), neighborId);
}

int SignalRoutingModule::refreshReportedDirectNeighbor(NodeNum nodeId, int32_t rssi, float snr, uint32_t nowSecs)
{
    if (!routingGraph || !nodeDB) {
        return EDGE_NO_CHANGE;
    }
    return refreshReportedDirectNeighborObservation(routingGraph, directSignals, directSignalCount,
                                                    NEIGHBOR_GRAPH_MAX_EDGES_PER_NODE, nodeDB->getNodeNum(), nodeId, rssi, snr,
                                                    nowSecs);
}

int32_t SignalRoutingModule::runOnce()
{
    if (!routingGraph || !nodeDB) {
        return disable();
    }
    uint32_t nowMs = millis();
    uint32_t nowSecs = nowSeconds();

    if (!loggedOwnRole) {
        loggedOwnRole = true;
        LOG_INFO("[SR] Own role=%s (%d) preset=%s node=0x%08x", DisplayFormatters::getDeviceRole(config.device.role),
                 (int)config.device.role,
                 DisplayFormatters::getModemPresetDisplayName(config.lora.modem_preset, false, config.lora.use_preset),
                 nodeDB->getNodeNum());
    }

    pruneCapabilityCache(nowSecs);
    pruneRelayIdentityCache(nowMs);
    maintainGraph(nowSecs);
    pruneDirectSignals(nowSecs);

    for (uint8_t i = 0; i < MAX_PENDING_RETRANSMITS; i++) {
        PendingRetransmit &pr = pendingRetransmits[i];
        if (pr.canceled || pr.packet == nullptr || pr.packetId == 0) {
            continue;
        }
        if ((int32_t)(nowMs - pr.fireAfterMs) >= 0) {
            LOG_INFO("[SR] T1 firing 0x%08x: no relay heard, retransmitting", pr.packetId);
            meshtastic_MeshPacket *toSend = pr.packet;
            pr.packet = nullptr;
            pr.canceled = true;
            const bool originated = isFromUs(toSend);
            if (!originated &&
                (!isActiveRoutingRole() ||
                 config.device.rebroadcast_mode == meshtastic_Config_DeviceConfig_RebroadcastMode_NONE ||
                 (nodeRateLimiter && nodeRateLimiter->wouldDrop(toSend)) || (channelQoS && !channelQoS->canRelay(toSend)))) {
                packetPool.release(toSend);
                continue;
            }
            // An expired transmit deadline would bypass normal radio contention for the delayed copy.
            toSend->tx_after = 0;
            isRetransmitting = true;
            if (router) {
                router->send(toSend);
            } else {
                packetPool.release(toSend);
            }
            isRetransmitting = false;
        }
    }

    if (routingGraph && signalBasedRoutingEnabled) {
        // A send can advance lastBroadcast beyond this pass's timestamp; a second list would duplicate relay work.
        bool sentTopologyThisPass = false;
        if (pendingTopologyReply.active && (int32_t)(nowMs - pendingTopologyReply.fireAfterMs) >= 0) {
            // A periodic list can already have answered the bootstrap request.
            bool answered = lastTopologyListMs != 0 && (int32_t)(lastTopologyListMs - pendingTopologyReply.requestedMs) > 0;
            pendingTopologyReply.active = false;
            if (answered) {
                LOG_INFO("[SR] Bootstrap reply dropped: list sent %ums ago",
                         lastTopologyListMs - pendingTopologyReply.requestedMs);
            } else if (!topologyBroadcastActive && canSendTopology()) {
                LOG_INFO("[SR] Firing jittered reply to bootstrap");
                sendSignalRoutingInfo();
                commitTopologyTxTimestamp();
                lastBootstrapReplyMs = nowMs;
                sentTopologyThisPass = true;
            }
        }

        if (needsBootBroadcast && !sentTopologyThisPass && canSendTopology()) {
            needsBootBroadcast = false;
            LOG_INFO("[SR] Sending empty boot broadcast");
            uint8_t bootBuf[PACKED_NEIGHBOR_HEADER_SIZE];
            // Reusing the boot version for the first real list would hide the counter restart from peers that missed boot.
            uint8_t bootVersion = currentTopologyVersion++;
            writePackedTopologyHeader(bootBuf, bootVersion, isActiveRoutingRole());
            sendTopologyPacket(NODENUM_BROADCAST, bootBuf, PACKED_NEIGHBOR_HEADER_SIZE);
            commitTopologyTxTimestamp();
            sentTopologyThisPass = true;
        } else if (!sentTopologyThisPass && sinceLastBroadcast(nowMs) >= (int32_t)(cfgBroadcastSecs * 1000)) {
            sendSignalRoutingInfo();
            topologyDirty = false;
            commitTopologyTxTimestamp();
            sentTopologyThisPass = true;
        } else if (!sentTopologyThisPass && topologyDirty &&
                   sinceLastBroadcast(nowMs) >= (int32_t)(cfgDirtyBroadcastSecs * 1000)) {
            LOG_INFO("[SR] Topology dirty — sending early broadcast");
            sendSignalRoutingInfo();
            topologyDirty = false;
            commitTopologyTxTimestamp();
        }

        static uint32_t lastTopologyLog = 0;
        if (topologyDirty || nowMs - lastTopologyLog >= GRAPH_MAINTENANCE_INTERVAL_SECS * 1000) {
            logNetworkTopology();
            lastTopologyLog = nowMs;
        }
    }

    uint32_t broadcastCycle = topologyDirty ? cfgDirtyBroadcastSecs * 1000 : cfgBroadcastSecs * 1000;

    int32_t age = sinceLastBroadcast(nowMs);
    uint32_t elapsed = age > 0 ? (uint32_t)age : 0;
    uint32_t timeToBroadcast = (elapsed < broadcastCycle) ? (broadcastCycle - elapsed) : 0;

    uint32_t nextDelay = signalBasedRoutingEnabled && canSendTopology()
                             ? std::min<uint32_t>(timeToBroadcast, GRAPH_MAINTENANCE_INTERVAL_SECS * 1000)
                             : GRAPH_MAINTENANCE_INTERVAL_SECS * 1000;

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

    if (pendingTopologyReply.active) {
        int32_t remaining = (int32_t)(pendingTopologyReply.fireAfterMs - nowMs);
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
    if (!signalBasedRoutingEnabled || !routingGraph || !nodeDB || !service || !canSendTopology() || topologyBroadcastActive) {
        return;
    }

    topologyBroadcastActive = true;

    static constexpr size_t MAX_TOTAL_PACKED =
        NEIGHBOR_GRAPH_MAX_EDGES_PER_NODE * PACKED_NEIGHBOR_ENTRY_SIZE + PACKED_NEIGHBOR_HEADER_SIZE;
    // The nRF52 loop has a 4 KB stack; serialized, nonrecursive sends can safely share this buffer.
    static uint8_t allPacked[MAX_TOTAL_PACKED];
    uint8_t totalNeighbors = packNeighborsForBroadcast(allPacked, sizeof(allPacked));

    uint8_t topologyVersion = currentTopologyVersion++;
    bool srActive = isActiveRoutingRole();
    if (totalNeighbors > 0) {
        lastTopologyListMs = millis();
    }

    if (totalNeighbors == 0) {
        uint8_t emptyBuf[PACKED_NEIGHBOR_HEADER_SIZE];
        writePackedTopologyHeader(emptyBuf, topologyVersion, srActive);
        sendTopologyPacket(dest, emptyBuf, PACKED_NEIGHBOR_HEADER_SIZE);
        topologyBroadcastActive = false;
        return;
    }

    uint8_t packetsNeeded = (totalNeighbors + MAX_SIGNAL_ROUTING_NEIGHBORS - 1) / MAX_SIGNAL_ROUTING_NEIGHBORS;

    char ourName[48];
    getNodeDisplayName(nodeDB->getNodeNum(), ourName, sizeof(ourName));

    LOG_INFO("[SR] SENDING: %u neighbors in %u packet(s) from %s (v%u)", totalNeighbors, packetsNeeded, ourName, topologyVersion);

    // Relayers need time to finish one chunk before receiving the next.
    uint32_t packetSpacingMs = 300;
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

        static uint8_t chunkBuf[PACKED_NEIGHBOR_HEADER_SIZE + MAX_SIGNAL_ROUTING_NEIGHBORS * PACKED_NEIGHBOR_ENTRY_SIZE];
        writePackedTopologyHeader(chunkBuf, topologyVersion, srActive, packetIndex + 1 < packetsNeeded, packetIndex > 0,
                                  packetIndex);
        size_t dataOffset = PACKED_NEIGHBOR_HEADER_SIZE + startNeighbor * PACKED_NEIGHBOR_ENTRY_SIZE;
        memcpy(&chunkBuf[PACKED_NEIGHBOR_HEADER_SIZE], &allPacked[dataOffset], count * PACKED_NEIGHBOR_ENTRY_SIZE);
        size_t chunkLen = PACKED_NEIGHBOR_HEADER_SIZE + count * PACKED_NEIGHBOR_ENTRY_SIZE;

        uint32_t txAfter = packetIndex > 0 ? millis() + packetIndex * packetSpacingMs : 0;
        sendTopologyPacket(dest, chunkBuf, chunkLen, txAfter);
    }

    trackNodeCapability(nodeDB->getNodeNum(), isActiveRoutingRole() ? CapabilityStatus::SRactive : CapabilityStatus::Passive);
    topologyBroadcastActive = false;
}

uint8_t SignalRoutingModule::packNeighborsForBroadcast(uint8_t *outBuf, size_t bufSize)
{
    if (!outBuf || bufSize < PACKED_NEIGHBOR_HEADER_SIZE || !routingGraph || !nodeDB) {
        return 0;
    }

    const NodeEdges *nodeEdges = routingGraph->getEdgesFrom(nodeDB->getNodeNum());
    if (!nodeEdges || nodeEdges->edgeCount == 0) {
        return 0;
    }

    const Edge *edgePtrs[NEIGHBOR_GRAPH_MAX_EDGES_PER_NODE];
    uint8_t edgeCount = 0;
    for (uint8_t i = 0; i < nodeEdges->edgeCount && edgeCount < NEIGHBOR_GRAPH_MAX_EDGES_PER_NODE; i++) {
        if (!isPlaceholderNode(nodeEdges->edges[i].to)) {
            edgePtrs[edgeCount++] = &nodeEdges->edges[i];
        }
    }

    // Dense hubs must preserve reachable SR parents when the packed list fills.
    std::sort(edgePtrs, edgePtrs + edgeCount, [this](const Edge *a, const Edge *b) {
        if (a->source != b->source) {
            return a->source == Edge::Source::Reported;
        }
        if (a->hearsUs != b->hearsUs) {
            return a->hearsUs;
        }
        const bool aSr = getCapabilityStatus(a->to) == CapabilityStatus::SRactive;
        const bool bSr = getCapabilityStatus(b->to) == CapabilityStatus::SRactive;
        if (aSr != bSr) {
            return aSr;
        }
        return a->getEtx() < b->getEtx();
    });

    memset(outBuf, 0, PACKED_NEIGHBOR_HEADER_SIZE);

    uint8_t count = 0;
    size_t maxEntries = (bufSize - PACKED_NEIGHBOR_HEADER_SIZE) / PACKED_NEIGHBOR_ENTRY_SIZE;
    for (uint8_t i = 0; i < edgeCount && count < maxEntries; i++) {
        const Edge *edge = edgePtrs[i];
        if (edge->source != Edge::Source::Reported) {
            continue;
        }

        const DirectNeighborSignal *sig = lookupDirectSignal(edge->to);
        if (!sig) {
            continue;
        }

        uint8_t *entry = &outBuf[PACKED_NEIGHBOR_HEADER_SIZE + count * PACKED_NEIGHBOR_ENTRY_SIZE];

        CapabilityStatus neighborStatus = getCapabilityStatus(edge->to);
        encodePackedNeighborEntry(entry, edge->to, sig->rssi, sig->snr, neighborStatus == CapabilityStatus::SRactive,
                                  edge->hearsUs, edge->etxVariance);

        count++;
    }

    return count;
}

void SignalRoutingModule::sendTopologyPacket(NodeNum dest, const uint8_t *packedData, size_t packedLen, uint32_t txAfterMs)
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

    // sendToMesh transfers ownership and can release the packet synchronously.
    const PacketId packetId = p->id;
    service->sendToMesh(p);

    if (routingGraph) {
        uint32_t currentTime = nowSeconds();
        routingGraph->recordNodeTransmission(nodeDB->getNodeNum(), packetId, currentTime);
    }
}

void SignalRoutingModule::updateGraphWithNeighbor(NodeNum sender, NodeNum neighborId, int8_t rssi, int8_t snr, bool hearsUs)
{
    if (routingGraph) {
        float etx = (rssi != 0 || snr != 0) ? NeighborGraph::calculateETX(rssi, snr, currentCostingSpreadingFactor()) : 1.0f;
        uint32_t currentTime = nowSeconds();

        // A peer repeatedly mentioning a dead node must not keep its link alive.
        routingGraph->updateEdge(sender, neighborId, etx, currentTime, Edge::Source::Mirrored, false);

        routingGraph->setEdgeHearsUs(sender, neighborId, hearsUs);

        noteTopologySenderHearsUs(sender, neighborId);
    }
}

void SignalRoutingModule::noteTopologySenderHearsUs(NodeNum sender, NodeNum listedNeighbor)
{
    if (!nodeDB) {
        return;
    }
    NodeNum myNode = nodeDB->getNodeNum();
    if (listedNeighbor == myNode) {
        if (confirmTopologySenderHearsUs(routingGraph, myNode, sender, listedNeighbor)) {
            char senderName[64];
            getNodeDisplayName(sender, senderName, sizeof(senderName));
            LOG_INFO("[SR] %s lists us: hearsUs", senderName);
        }
    } else if (confirmTopologySenderHearsNeighbor(routingGraph, sender, listedNeighbor)) {
        LOG_INFO("[SR] %08x lists %08x: hearsUs", sender, listedNeighbor);
    }
}

void SignalRoutingModule::preProcessSignalRoutingPacket(const meshtastic_MeshPacket *p)
{
    if (!p || p->which_payload_variant != meshtastic_MeshPacket_decoded_tag ||
        p->decoded.portnum != meshtastic_PortNum_SIGNAL_ROUTING_APP ||
        p->decoded.payload.size > sizeof(p->decoded.payload.bytes)) {
        return;
    }
    meshtastic_SignalRoutingInfo info = meshtastic_SignalRoutingInfo_init_zero;
    if (!pb_decode_from_bytes(p->decoded.payload.bytes, p->decoded.payload.size, &meshtastic_SignalRoutingInfo_msg, &info)) {
        LOG_WARN("[SR] Bad SignalRoutingInfo from %08x (size=%u)", p->from, p->decoded.payload.size);
        return;
    }

    processTopology(*p, info);
}

void SignalRoutingModule::processTopology(const meshtastic_MeshPacket &mp, const meshtastic_SignalRoutingInfo &info)
{
    const auto *p = &mp;
    if (!routingGraph || !nodeDB || !p->from || isPlaceholderNode(p->from) || p->via_mqtt || isFromUs(p)) {
        return;
    }
    if (!isActiveRoutingRole() && !isDirectPacket(*p)) {
        return;
    }
    const NodeNum ourNode = nodeDB->getNodeNum();
    if (routingGraph->hasNodeTransmitted(ourNode, p->id, nowSeconds())) {
        return;
    }
    if (info.packed_neighbors.size > sizeof(info.packed_neighbors.bytes)) {
        return;
    }
    PackedHeader hdr = {};
    PackedNeighborEntry neighbors[MAX_SIGNAL_ROUTING_NEIGHBORS];
    uint8_t neighborCount = decodePackedNeighbors(info.packed_neighbors.bytes, info.packed_neighbors.size, neighbors,
                                                  MAX_SIGNAL_ROUTING_NEIGHBORS, &hdr);

    if (hdr.formatVersion != PACKED_NEIGHBOR_FORMAT_VERSION) {
        return;
    }

    const TopologyVersionEntry *processed = findTopologyEntry(lastPreProcessedVersion, lastPreProcessedVersionCount, p->from);
    // Indexed chunks can arrive again after a different chunk. Remember every index in this version;
    // legacy continuations have index zero and must still use packet identity instead.
    const uint32_t chunkBit = hdr.continuation ? (hdr.chunkIndex ? (uint32_t{1} << hdr.chunkIndex) : 0)
                                             : (hdr.moreChunks ? 1 : 0);
    if (processed && processed->version == hdr.topologyVersion &&
        (processed->packetId == p->id || (processed->processedChunks & chunkBit))) {
        return;
    }
    uint8_t receivedVersion = hdr.topologyVersion;
    uint8_t lastProcessedVersion = getTopologyVersion(lastTopologyVersion, lastTopologyVersionCount, p->from);
    uint32_t nowMs = millis();
    if (nowMs == 0)
        nowMs = 1;

    // A relayed boot notice still proves the originator restarted its version counter.
    bool bootBroadcast =
        info.packed_neighbors.size == PACKED_NEIGHBOR_HEADER_SIZE && hdr.isCompleteList() && receivedVersion == 0;
    bool directCompleteList = hdr.isCompleteList() && neighborCount > 0 && isDirectPacket(*p);
    if (gateTopologyVersion(p->from, receivedVersion, bootBroadcast, directCompleteList, nowMs) == SrTopologyVerdict::Stale) {
        return;
    }

    bool isNewVersion = (receivedVersion != lastProcessedVersion);

    CapabilityStatus newStatus = hdr.signalRoutingActive ? CapabilityStatus::SRactive : CapabilityStatus::Passive;
    CapabilityStatus oldStatus = getCapabilityStatus(p->from);
    trackNodeCapability(p->from, newStatus);

    if (oldStatus != newStatus) {
        char senderName[64];
        getNodeDisplayName(p->from, senderName, sizeof(senderName));
        LOG_INFO("[SR] Capability update: %s changed from %d to %d", senderName, (int)oldStatus, (int)newStatus);
    }

    char senderNameForTopo[48];
    getNodeDisplayName(p->from, senderNameForTopo, sizeof(senderNameForTopo));
    if (isActiveRoutingRole() && !topologySenderHorizonOk(p->from, isDirectPacket(*p), neighbors, neighborCount)) {
        LOG_INFO("[SR] Horizon: ignore topology from %s (unqualified far publisher)", senderNameForTopo);
        setTopologyVersion(lastPreProcessedVersion, lastPreProcessedVersionCount, p->from, receivedVersion, nowMs, p->id, chunkBit);
        return;
    }
    LOG_INFO("[SR] Processing topology from %s: %d neighbors (v%u, %s, relay=0x%02x)", senderNameForTopo, neighborCount,
             receivedVersion, isNewVersion ? "new version" : "continuation", p->relay_node);

    if (neighborCount == 0 && isDirectPacket(*p) && hdr.signalRoutingActive) {
        LOG_INFO("[SR] Empty broadcast from %s: topology reply scheduled", senderNameForTopo);
        scheduleEmptyTopologyReply(p->from, p->id);
    }

    if (!isActiveRoutingRole()) {
        setTopologyVersion(lastPreProcessedVersion, lastPreProcessedVersionCount, p->from, receivedVersion, nowMs, p->id, chunkBit);
        return;
    }

    const bool senderIsDirect = hasReportedDirectEdge(p->from);
    const uint32_t nowSecs = nowSeconds();

    if (routingGraph && !senderIsDirect) {
        bool namesL1 = false;
        bool namesL2 = false;
        NodeNum parent = 0;
        for (uint8_t i = 0; i < neighborCount; i++) {
            NodeNum nid = neighbors[i].nodeId;
            if (hasReportedDirectEdge(nid)) {
                namesL1 = true;
                if (parent == 0) {
                    parent = nid;
                }
            }
            if (routingGraph->getNodeClass(nid) == NodeClass::L2) {
                namesL2 = true;
                if (parent == 0) {
                    parent = nid;
                }
            }
        }
        const bool alreadyInBall = routingGraph->getEdgesFrom(p->from) != nullptr;
        if (alreadyInBall || namesL1 || namesL2) {
            NodeClass cls = routingGraph->classifyCandidate(p->from);
            if (cls == NodeClass::Unknown) {
                cls = (namesL2 && !namesL1 && GRAPH_MAX_DEPTH >= 3) ? NodeClass::L3 : NodeClass::L2;
            }
            if (cls == NodeClass::L2 || cls == NodeClass::L3) {
                routingGraph->ensurePublisher(p->from, cls, parent, nowSecs);
            }
        }
    }
    for (uint8_t i = 0; i < neighborCount; i++) {
        const PackedNeighborEntry &neighbor = neighbors[i];

        if (neighbor.nodeId == 0 || isPlaceholderNode(neighbor.nodeId)) {
            LOG_WARN("[SR] Skipping invalid neighbor node ID: %08x", neighbor.nodeId);
            continue;
        }

        // Inferred and mirrored reverse links do not prove local RF reception.
        const bool hasDirectConnection = neighbor.nodeId == ourNode || hasReportedDirectEdge(neighbor.nodeId);

        char neighborName[48];
        getNodeDisplayName(neighbor.nodeId, neighborName, sizeof(neighborName));

        // An old list must not invent a last hop through us after its sender moves behind a relay.
        if (neighbor.nodeId != ourNode || senderIsDirect) {
            updateGraphWithNeighbor(p->from, neighbor.nodeId, neighbor.rssi, neighbor.snr, neighbor.hearsUs);
        }

        if (hasDirectConnection) {
            if (neighbor.nodeId != ourNode && routingGraph) {
                routingGraph->clearDownstreamForDestination(neighbor.nodeId);
            }
            LOG_INFO("[SR]   -> %s: HAS direct connection, sender confirms reachability", neighborName);
        } else if (neighbor.hearsUs) {
            float etxForDownstream = NeighborGraph::calculateETX(neighbor.rssi, neighbor.snr, currentCostingSpreadingFactor());
            bool admitted = false;
            if (routingGraph) {
                admitted = routingGraph->tryAdmitListedPublisher(p->from, neighbor.nodeId, senderIsDirect, nowSecs);
            }
            if (admitted) {
                if (routingGraph) {
                    routingGraph->clearDownstreamForDestination(neighbor.nodeId);
                }
                LOG_INFO("[SR]   -> %s: admitted to edge ball via %s", neighborName, senderNameForTopo);
            } else if (routingGraph) {
                LOG_INFO("[SR]   -> %s: no direct connection, list-downstream of %s", neighborName, senderNameForTopo);

                routingGraph->updateDownstreamListed(neighbor.nodeId, p->from, etxForDownstream, nowSecs, true);
            }
        } else {
            LOG_INFO("[SR]   -> %s: asymmetric (hearsUs=false), not downstream of %s", neighborName, senderNameForTopo);
        }
    }

    // Only a complete, contiguous list justifies retracting links omitted by the sender.
    // Legacy chunks lack sequence numbers, so missing chunks must leave prior evidence intact.
    PendingListed &pl = pendingListed[pendingListedSlot(p->from)];
    if (!hdr.continuation) {
        pl.valid = true;
        pl.sender = p->from;
        pl.version = receivedVersion;
        pl.count = 0;
        pl.nextChunk = 1;
    }
    const NodeNum *listedIds = nullptr;
    uint8_t listedCount = 0;
    if (hdr.continuation && pl.valid && pl.sender == p->from &&
        (pl.version != receivedVersion || hdr.chunkIndex != pl.nextChunk)) {
        pl.valid = false;
    }
    if (pl.valid && pl.sender == p->from && pl.version == receivedVersion) {
        if (hdr.continuation) {
            pl.nextChunk++;
        }
        for (uint8_t i = 0; i < neighborCount; i++) {
            if (pl.count < NEIGHBOR_GRAPH_MAX_EDGES_PER_NODE) {
                pl.ids[pl.count++] = neighbors[i].nodeId;
            } else {
                pl.valid = false;
            }
        }
        if (!hdr.moreChunks) {
            if (pl.valid) {
                listedIds = pl.ids;
                listedCount = pl.count;
            }
            pl.valid = false;
        }
    } else if (hdr.continuation) {
        LOG_INFO("[SR] Chunk from %08x (v%u) without its first packet: hearsUs kept", p->from, receivedVersion);
        if (pl.sender == p->from) {
            pl.valid = false;
        }
    }
    if (routingGraph && listedIds && !bootBroadcast) {
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
                    bool foundInNeighborList = false;
                    for (uint8_t i = 0; i < listedCount; i++) {
                        if (listedIds[i] == nodeId) {
                            foundInNeighborList = true;
                            break;
                        }
                    }
                    if (!foundInNeighborList) {
                        char nodeName[48];
                        getNodeDisplayName(nodeId, nodeName, sizeof(nodeName));
                        LOG_INFO("[SR] hearsUs cleared on %s -> %s: not in %s's list", nodeName, senderNameForTopo,
                                 senderNameForTopo);
                        routingGraph->setEdgeHearsUs(nodeId, p->from, false);
                    }
                }
            }
        }
        // Omitted neighbors otherwise retain false coverage until the much longer graph TTL.
        if (routingGraph->retainListedEdges(p->from, listedIds, listedCount)) {
            LOG_INFO("[SR] %08x dropped entries from its list: stale edges removed", p->from);
            routingGraph->pruneUnreachableFromRoot(nowSeconds());
        }
    }

    setTopologyVersion(lastPreProcessedVersion, lastPreProcessedVersionCount, p->from, receivedVersion, nowMs, p->id, chunkBit);
}
meshtastic_MeshPacket *SignalRoutingModule::allocReply()
{
    // Unicast replies to SR broadcasts trigger NO_RESPONSE floods on stock nodes.
    ignoreRequest = true;
    return nullptr;
}

bool SignalRoutingModule::handleReceivedProtobuf(const meshtastic_MeshPacket &mp, meshtastic_SignalRoutingInfo *p)
{
    if (p) {
        processTopology(mp, *p);
    }
    return false;
}

#define PLACEHOLDER_BASE Edge::PLACEHOLDER_NODE_BASE

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
        return false;
    }

    if (isPlaceholderNode(realNodeId)) {
        return false;
    }

    uint8_t relayId = placeholderId & 0xFF;
    NodeNum alreadyResolved = resolveRelayIdentity(relayId);
    if (alreadyResolved != 0) {
        if (alreadyResolved == realNodeId) {
            return false;
        }
        LOG_WARN("[SR] Placeholder %08x already %08x, not %08x", placeholderId, alreadyResolved, realNodeId);
        return false;
    }

    rememberRelayIdentity(realNodeId, relayId);

    replaceGatewayNode(placeholderId, realNodeId);

    if (routingGraph) {
        NodeNum ourNode = nodeDB->getNodeNum();

        const NodeEdges *ourEdges = routingGraph->getEdgesFrom(ourNode);
        if (ourEdges) {
            for (uint8_t i = 0; i < ourEdges->edgeCount; i++) {
                if (ourEdges->edges[i].to == placeholderId) {
                    float etx = ourEdges->edges[i].getEtx();

                    routingGraph->updateEdge(ourNode, realNodeId, etx, nowSeconds());
                }
            }
        }

        routingGraph->removeNode(placeholderId);

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
    if (oldNode == newNode || !routingGraph)
        return;

    size_t transferred = routingGraph->transferDownstream(oldNode, newNode);
    if (transferred > 0) {
        LOG_INFO("[SR] Transferred %u downstream entries from %08x to %08x", (unsigned)transferred, oldNode, newNode);
    }
    routingGraph->clearDownstreamForDestination(oldNode);
}

bool SignalRoutingModule::isPlaceholderConnectedToUs(NodeNum placeholderId) const
{
    if (!routingGraph || !isPlaceholderNode(placeholderId)) {
        return false;
    }

    NodeNum ourNode = nodeDB->getNodeNum();

    const NodeEdges *edges = routingGraph->getEdgesFrom(placeholderId);
    if (edges) {
        for (uint8_t i = 0; i < edges->edgeCount; i++) {
            if (edges->edges[i].to == ourNode) {
                return true;
            }
        }
    }

    return false;
}

bool SignalRoutingModule::hasBetterPositionedSRNeighbor(NodeNum myNode, NodeNum heardFrom, NodeNum destination, uint32_t packetId)
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

        CapabilityStatus status = getCapabilityStatus(neighbor);
        if (status != CapabilityStatus::SRactive)
            continue;

        // We may be the sole hearer until another relay actually holds this copy.
        if (destination != 0 && !routingGraph->hasNodeTransmitted(neighbor, packetId, nowSeconds()))
            continue;

        const NodeEdges *neighborEdges = routingGraph->getEdgesFrom(neighbor);
        if (!neighborEdges)
            continue;

        bool neighborHearsSource = false;
        for (uint8_t j = 0; j < neighborEdges->edgeCount; j++) {
            if (neighborEdges->edges[j].to == heardFrom) {
                neighborHearsSource = true;
                break;
            }
        }
        if (!neighborHearsSource)
            continue;

        if (destination != 0) {
            bool canReachDest = false;
            for (uint8_t j = 0; j < neighborEdges->edgeCount; j++) {
                if (neighborEdges->edges[j].to == destination) {
                    canReachDest = true;
                    break;
                }
            }
            if (!canReachDest) {
                canReachDest =
                    routingGraph->isDownstream(destination) && routingGraph->getDownstreamRelay(destination) == neighbor;
            }
            if (!canReachDest) {
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

        if (destination != 0) {
            char neighborName[64];
            getNodeDisplayName(neighbor, neighborName, sizeof(neighborName));
            LOG_INFO("[SR] SR neighbor %s hears %08x, reaches dest", neighborName, heardFrom);
            return true;
        }

        NodeSet coveredNodes;
        coveredNodes.insert(heardFrom);
        coveredNodes.insert(neighbor);

        const NodeEdges *transmitterEdges = routingGraph->getEdgesFrom(heardFrom);
        if (transmitterEdges) {
            for (uint8_t j = 0; j < transmitterEdges->edgeCount; j++)
                coveredNodes.insert(transmitterEdges->edges[j].to);
        }

        for (uint8_t j = 0; j < neighborEdges->edgeCount; j++)
            coveredNodes.insert(neighborEdges->edges[j].to);

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
            LOG_INFO("[SR] SR neighbor %s hears %08x and covers all our unique nodes", neighborName, heardFrom);
            return true;
        }
    }
    return false;
}

static const char *horizonClassTag(NodeClass c)
{
    switch (c) {
    case NodeClass::L1:
        return "[L1] ";
    case NodeClass::L2:
        return "[L2] ";
    case NodeClass::L3:
        return "[L3] ";
    case NodeClass::Unknown:
    default:
        return "[L?] ";
    }
}

void SignalRoutingModule::logNetworkTopology()
{
#ifdef DEBUG_MUTE
    return;
#else
    if (!routingGraph)
        return;

    NodeNum nodeBuf[NEIGHBOR_GRAPH_MAX_NEIGHBORS];
    size_t rawNodeCount = routingGraph->getAllNodeIds(nodeBuf, NEIGHBOR_GRAPH_MAX_NEIGHBORS);

    size_t nodeCount = rawNodeCount;
    if (!isActiveRoutingRole()) {
        size_t filteredCount = 0;
        for (size_t i = 0; i < rawNodeCount; i++) {
            const NodeEdges *edges = routingGraph->getEdgesFrom(nodeBuf[i]);
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
    // Nested diagnostic loops otherwise consume scarce loop-thread stack space.
    char nameBuf[80];
    getNodeDisplayName(ourNode, nameBuf, sizeof(nameBuf));

    const NodeEdges *ourEdges = routingGraph->getEdgesFrom(ourNode);
    uint8_t directCount = ourEdges ? ourEdges->edgeCount : 0;

    uint8_t l1Count = 0, l2Count = 0, l3Count = 0;
    for (size_t i = 0; i < rawNodeCount; i++) {
        switch (routingGraph->getNodeClass(nodeBuf[i])) {
        case NodeClass::L1:
            l1Count++;
            break;
        case NodeClass::L2:
            l2Count++;
            break;
        case NodeClass::L3:
            l3Count++;
            break;
        case NodeClass::Unknown:
            break;
        }
    }

    LOG_INFO("[SR] Network Topology: %d nodes, %u direct neighbors (L1=%u L2=%u L3=%u)", nodeCount, directCount,
             static_cast<unsigned>(l1Count), static_cast<unsigned>(l2Count), static_cast<unsigned>(l3Count));
    LOG_INFO("[SR] %s (us)", nameBuf);

    if (!ourEdges || ourEdges->edgeCount == 0) {
        LOG_INFO("[SR]   (no direct neighbors)");
    } else {
        for (uint8_t i = 0; i < ourEdges->edgeCount; i++) {
            const Edge &edge = ourEdges->edges[i];

            getNodeDisplayName(edge.to, nameBuf, sizeof(nameBuf));

            CapabilityStatus neighborStatus = getCapabilityStatus(edge.to);
            const char *nprefix = "";
            if (neighborStatus == CapabilityStatus::SRactive) {
                nprefix = "[SR-active] ";
            } else if (neighborStatus == CapabilityStatus::Passive) {
                nprefix = "[SR-passive] ";
            }
            const char *nclass = horizonClassTag(routingGraph->getNodeClass(edge.to));

            float etx = edge.getEtx();
            const char *quality;
            if (etx < 2.0f)
                quality = "excellent";
            else if (etx < 4.0f)
                quality = "good";
            else if (etx < 8.0f)
                quality = "fair";
            else
                quality = "poor";

            int32_t age = computeAgeSecs(edge.lastUpdate, nowSeconds());
            char ageBuf[16];
            if (age < 0) {
                snprintf(ageBuf, sizeof(ageBuf), "-");
            } else {
                snprintf(ageBuf, sizeof(ageBuf), "%d", age);
            }

            bool isLast = (i == ourEdges->edgeCount - 1);
            const char *branch = isLast ? "\\-" : "+-";
            const char *cont = isLast ? " " : "|";

            const NodeEdges *neighborEdges = routingGraph->getEdgesFrom(edge.to);
            uint8_t listenerCount = neighborEdges ? neighborEdges->edgeCount : 0;

            static constexpr size_t MAX_DS_DISPLAY = 16;
            NodeNum dsBuf[MAX_DS_DISPLAY];
            uint16_t dsCosts[MAX_DS_DISPLAY];
            size_t totalDsCount = routingGraph->getDownstreamCountForRelay(edge.to);
            size_t dsCount = routingGraph->getDownstreamNodesForRelay(edge.to, dsBuf, dsCosts, MAX_DS_DISPLAY);

            const char *bidir = edge.hearsUs ? ", hearsUs" : "";
            LOG_INFO("[SR]   %s %s%s%s: %s (ETX=%.1f, %ss, covers %u, relay for %u down%s)", branch, nclass, nprefix, nameBuf,
                     quality, etx, ageBuf, static_cast<unsigned int>(listenerCount), static_cast<unsigned int>(totalDsCount),
                     bidir);

            if (listenerCount > 0) {
                for (uint8_t n = 0; n < listenerCount; n++) {
                    const Edge &nEdge = neighborEdges->edges[n];
                    getNodeDisplayName(nEdge.to, nameBuf, sizeof(nameBuf));

                    CapabilityStatus lStatus = getCapabilityStatus(nEdge.to);
                    const char *lprefix = "";
                    if (lStatus == CapabilityStatus::SRactive)
                        lprefix = "[SR-active] ";
                    else if (lStatus == CapabilityStatus::Passive)
                        lprefix = "[SR-passive] ";
                    const char *lclass = horizonClassTag(routingGraph->getNodeClass(nEdge.to));

                    bool lLast = (n == listenerCount - 1) && (totalDsCount == 0);
                    const char *lBranch = lLast ? "\\-" : "+-";
                    LOG_INFO("[SR]   %s    %s %s%s%s (ETX=%.1f)", cont, lBranch, lclass, lprefix, nameBuf, nEdge.getEtx());
                }
            }

            if (totalDsCount > 0) {
                size_t printed = 0;
                while (printed < totalDsCount) {
                    dsCount = routingGraph->getDownstreamNodesForRelay(edge.to, dsBuf, dsCosts, MAX_DS_DISPLAY, printed);
                    if (dsCount == 0)
                        break;
                    for (size_t d = 0; d < dsCount; d++) {
                        getNodeDisplayName(dsBuf[d], nameBuf, sizeof(nameBuf));

                        CapabilityStatus dsStatus = getCapabilityStatus(dsBuf[d]);
                        const char *dsprefix = "";
                        if (dsStatus == CapabilityStatus::SRactive)
                            dsprefix = "[SR-active] ";
                        else if (dsStatus == CapabilityStatus::Passive)
                            dsprefix = "[SR-passive] ";

                        bool dsLast = (printed + 1 == totalDsCount);
                        const char *dsBranch = dsLast ? "\\-" : "+-";
                        float dsCost = dsCosts[d] / 100.0f;

                        Route dsRoute = routingGraph->calculateRoute(dsBuf[d], nowSeconds(), routePolicy());
                        if (dsRoute.nextHop != 0 && dsRoute.costFixed < 0xFFFF) {
                            float routeCost = dsRoute.getCost();
                            LOG_INFO("[SR]   %s    %s [downstream] %s%s (routeETX=%.1f, dsETX=%.1f)", cont, dsBranch, dsprefix,
                                     nameBuf, routeCost, dsCost);
                        } else {
                            LOG_INFO("[SR]   %s    %s [downstream] %s%s (dsETX=%.1f)", cont, dsBranch, dsprefix, nameBuf, dsCost);
                        }
                        printed++;
                    }
                }
            }
        }
    }

    LOG_INFO("[SR] ETX map: 1.0~-60dB/10dB, 2.0~-90dB/0dB, 4.0~-110dB/-5dB");
    LOG_INFO("[SR] Topology logging complete");
#endif
}

void SignalRoutingModule::observeRelayedPacket(const meshtastic_MeshPacket &mp)
{
    // Passive publishers also need measured relayer links; omitting them creates false unique coverage.
    if (!routingGraph || !nodeDB || mp.via_mqtt || isDirectPacket(mp) || mp.relay_node == 0) {
        return;
    }
    int32_t rxRssi = mp.rx_rssi;
    const float rxSnr = mp.rx_snr;
    const bool hasSignalData = NeighborGraph::normalizeRxSignal(rxRssi, rxSnr);
    if (!canSendTopology()) {
        LOG_INFO("[SR] Publishes no topology: skipping relayed packet observation");
    } else {
        const bool activeRouting = isActiveRoutingRole();
        NodeNum inferredRelayer = resolveRelayIdentity(mp.relay_node, rxRssi, rxSnr);

        // A relay may be known only through topology; ambiguous bytes cannot justify measured coverage.
        if (inferredRelayer == 0 && routingGraph && nodeDB) {
            const NodeEdges *myEdges = routingGraph->getEdgesFrom(nodeDB->getNodeNum());
            if (myEdges) {
                NodeNum onlyMatch = 0;
                bool ambiguous = false;
                for (uint8_t i = 0; i < myEdges->edgeCount; i++) {
                    NodeNum neighbor = myEdges->edges[i].to;
                    if ((neighbor & 0xFF) == mp.relay_node && !isPlaceholderNode(neighbor)) {
                        if (onlyMatch != 0 && onlyMatch != neighbor) {
                            ambiguous = true;
                            break;
                        }
                        onlyMatch = neighbor;
                    }
                }
                if (ambiguous) {
                    LOG_INFO("[SR] Relay 0x%02x matches more than one neighbour: not resolved", mp.relay_node);
                } else if (onlyMatch != 0) {
                    inferredRelayer = onlyMatch;

                    rememberRelayIdentity(onlyMatch, mp.relay_node);
                    LOG_INFO("[SR] Resolved relay 0x%02x to known node %08x", mp.relay_node, onlyMatch);
                }
            }
        }

        if (inferredRelayer == 0) {
            inferredRelayer = createPlaceholderNode(mp.relay_node);

            if (!routingGraph->getEdgesFrom(inferredRelayer)) {
                LOG_INFO("[SR] Created placeholder %08x for unknown relay 0x%02x", inferredRelayer, mp.relay_node);
            }
        }

        // Treating our echo as downstream evidence invents a route through ourselves and causes forwarding loops.
        bool ownEcho = routingGraph && nodeDB && routingGraph->hasNodeTransmitted(nodeDB->getNodeNum(), mp.id, nowSeconds());
        if (ownEcho) {
            LOG_INFO("[SR] Own echo 0x%08x via 0x%02x: no inference", mp.id, mp.relay_node);
        }
        if (!ownEcho && inferredRelayer != 0 && inferredRelayer != mp.from) {
            if (!isPlaceholderNode(inferredRelayer)) {
                rememberRelayIdentity(inferredRelayer, mp.relay_node);
            }

            LOG_INFO("[SR] Inferred gateway relationship: %08x relayed by %08x", mp.from, inferredRelayer);

            trackNodeCapability(mp.from, CapabilityStatus::Unknown);
            trackNodeCapability(inferredRelayer, CapabilityStatus::Unknown);

            bool hasDirectConnectionToRelay = hasReportedDirectEdge(inferredRelayer);
            bool wasDirectNeighbor = hasReportedDirectEdge(mp.from);
            uint32_t nowSecs = nowSeconds();
            bool alreadyHeardDirect = mp.id != 0 && routingGraph && routingGraph->hasNodeTransmitted(mp.from, mp.id, nowSecs);
            bool travelling = wasDirectNeighbor && !alreadyHeardDirect;
            if (travelling && nodeDB) {
                if (routingGraph->retractDirectLink(nodeDB->getNodeNum(), mp.from)) {
                    removeDirectSignal(mp.from);
                    markTopologyDirty();
                    LOG_INFO("[SR] %08x heard only via %08x: direct link retracted", mp.from, inferredRelayer);
                }
            }

            // Hearing an originator through a relay does not prove a usable return path.
            bool singleHopRelay = (mp.hop_start - mp.hop_limit) == 1;
            CapabilityStatus sourceStatus = getCapabilityStatus(mp.from);
            bool sourcePublishes = (sourceStatus == CapabilityStatus::SRactive || sourceStatus == CapabilityStatus::Passive);

            bool relayHearsUs = false;
            if (nodeDB) {
                const NodeEdges *mine = routingGraph->getEdgesFrom(nodeDB->getNodeNum());
                if (mine) {
                    for (uint8_t i = 0; i < mine->edgeCount; i++) {
                        if (mine->edges[i].to == inferredRelayer && mine->edges[i].hearsUs) {
                            relayHearsUs = true;
                            break;
                        }
                    }
                }
            }
            bool destinationListsRelay = false;
            const NodeEdges *originEdges = routingGraph->getEdgesFrom(mp.from);
            if (originEdges) {
                for (uint8_t i = 0; i < originEdges->edgeCount; i++) {
                    if (originEdges->edges[i].to == inferredRelayer) {
                        destinationListsRelay = true;
                        break;
                    }
                }
            }
            NodeNum existingParent = routingGraph->getDownstreamRelay(mp.from);
            // A live parent needs stronger replacement evidence than a forwarded copy.
            bool stealsWithoutList =
                existingParent != 0 && existingParent != inferredRelayer && !destinationListsRelay && !travelling;

            const bool destInBall = routingGraph->getEdgesFrom(mp.from) != nullptr;
            NodeNum listParent = 0;
            uint16_t listCost = 0;
            const bool listParked = routingGraph->listDownstream(mp.from, listParent, listCost);
            const bool stealOk = travelling || (!destInBall && !listParked);
            if (activeRouting && hasDirectConnectionToRelay && relayHearsUs && (destinationListsRelay || !sourcePublishes) &&
                (travelling || singleHopRelay || !sourcePublishes) && !stealsWithoutList && stealOk) {
                // A multihop inferred path must not be priced as one measured good link.
                uint8_t hopsUsed = mp.hop_start > mp.hop_limit ? mp.hop_start - mp.hop_limit : 1;
                float inferredEtx = NeighborGraph::calculateETX(-70, 5.0f, currentCostingSpreadingFactor()) * hopsUsed;
                routingGraph->updateDownstreamExclusive(mp.from, inferredRelayer, inferredEtx, nowSeconds(), travelling);
                if (!singleHopRelay && !travelling) {
                    LOG_INFO("[SR] Downstream: %08x via %08x (%d hops, stock)", mp.from, inferredRelayer,
                             mp.hop_start - mp.hop_limit);
                } else {
                    LOG_INFO("[SR] Downstream: %08x via %08x", mp.from, inferredRelayer);
                }
            } else if (activeRouting && hasDirectConnectionToRelay && stealsWithoutList) {
                LOG_INFO("[SR] No downstream %08x via %08x: keeping parent %08x (no list claim)", mp.from, inferredRelayer,
                         existingParent);
            } else if (activeRouting && hasDirectConnectionToRelay && (travelling || singleHopRelay || !sourcePublishes) &&
                       (!relayHearsUs || (sourcePublishes && !destinationListsRelay))) {
                LOG_INFO("[SR] No downstream %08x via %08x: relay hears us %d, originator lists relay %d", mp.from,
                         inferredRelayer, relayHearsUs, destinationListsRelay);
            } else if (activeRouting && hasDirectConnectionToRelay && !singleHopRelay) {
                LOG_INFO("[SR] No downstream %08x via %08x: %d hops, SR", mp.from, inferredRelayer, mp.hop_start - mp.hop_limit);
            }

            // A stock relay supplies no topology; its forwarding proves only that it heard the originator.
            bool relayerIsLegacy = getCapabilityStatus(inferredRelayer) == CapabilityStatus::Legacy;
            if (activeRouting && relayerIsLegacy) {
                LOG_INFO("[SR] Inferred: stock %08x hears %08x (relay seen)", inferredRelayer, mp.from);

                uint32_t monotonicTimestamp = nowSeconds();
                int32_t defaultRssi = -70;
                float defaultSnr = 5.0f;

                routingGraph->updateEdge(inferredRelayer, mp.from,
                                         NeighborGraph::calculateETX(defaultRssi, defaultSnr, currentCostingSpreadingFactor()),
                                         monotonicTimestamp, Edge::Source::Inferred);
            } else {
                LOG_INFO("[SR] No inference: %08x not Legacy (%d)", inferredRelayer, (int)getCapabilityStatus(inferredRelayer));
            }

            // Relays that rarely originate still supply direct-link measurements whenever they forward a frame.
            if (hasSignalData && !isPlaceholderNode(inferredRelayer)) {
                uint32_t monotonicTimestamp = nowSeconds();
                int changeType = refreshReportedDirectNeighbor(inferredRelayer, rxRssi, rxSnr, monotonicTimestamp);
                if (changeType == EDGE_SIGNIFICANT_CHANGE) {
                    markTopologyDirty();
                }
            }

            if (routingGraph) {
                uint32_t currentTime = nowSeconds();

                if (activeRouting) {
                    routingGraph->recordNodeTransmission(mp.from, mp.id, currentTime);
                    routingGraph->recordNodeTransmission(inferredRelayer, mp.id, currentTime);
                }
            }
        }

        if (nodeDB && inferredRelayer != 0 && !isPlaceholderNode(inferredRelayer)) {
            NodeNum ourNode = nodeDB->getNodeNum();
            if (inferredRelayer != ourNode && (mp.from == ourNode || isCommittedRelay(mp.id))) {
                markStockNodeRelayedOurPacket(inferredRelayer);
            }
        }
    }
}

ProcessMessage SignalRoutingModule::handleReceived(const meshtastic_MeshPacket &mp)
{
    if (!routingGraph || !nodeDB) {
        return ProcessMessage::CONTINUE;
    }

    if (mp.which_payload_variant == meshtastic_MeshPacket_decoded_tag &&
        mp.decoded.payload.size > meshtastic_Constants_DATA_PAYLOAD_LEN) {
        LOG_WARN("[SR] Bad payload size: %u bytes (max %u)", mp.decoded.payload.size, meshtastic_Constants_DATA_PAYLOAD_LEN);
        return ProcessMessage::CONTINUE;
    }

    int32_t rxRssi = mp.rx_rssi;
    const float rxSnr = mp.rx_snr;
    bool hasSignalData = NeighborGraph::normalizeRxSignal(rxRssi, rxSnr);
    bool notViaMqtt = !mp.via_mqtt;

    bool isDirectFromSender = isDirectPacket(mp);
    uint8_t fromLastByte = mp.from & 0xFF;

    if (hasSignalData && notViaMqtt) {
        LOG_INFO("[SR] Packet from 0x%08x: relay=0x%02x hs=%d hl=%d direct=%d", mp.from, mp.relay_node, mp.hop_start,
                 mp.hop_limit, isDirectFromSender);
    }

    if (isActiveRoutingRole() || (hasSignalData && notViaMqtt && isDirectFromSender)) {
        updateNodeActivityForPacketAndRelay(&mp);
    }

    if (routingGraph && notViaMqtt) {
        if (hasSignalData && isDirectFromSender) {
            updateNeighborInfo(mp.from, rxRssi, rxSnr);
            routingGraph->clearDownstreamForDestination(mp.from);
        }
    }

    if (hasSignalData && notViaMqtt && isDirectFromSender) {
        NodeNum placeholderId = getPlaceholderForRelay(fromLastByte);
        if (isPlaceholderNode(placeholderId)) {
            if (resolvePlaceholder(placeholderId, mp.from)) {
                LOG_INFO("[SR] Direct contact: resolved placeholder %08x with node %08x", placeholderId, mp.from);
            }
        }

        rememberRelayIdentity(mp.from, fromLastByte);
        trackNodeCapability(mp.from, CapabilityStatus::Unknown);

        // Naming us as next hop proves the sender hears us before its next topology list arrives.
        if (mp.to != NODENUM_BROADCAST && mp.next_hop != 0 && mp.next_hop == static_cast<uint8_t>(nodeDB->getNodeNum() & 0xFF)) {
            if (confirmSenderHearsUs(routingGraph, nodeDB->getNodeNum(), mp.from)) {
                LOG_INFO("[SR] %08x routes through us: hearsUs confirmed", mp.from);
            }
        }

        char senderName[64];
        getNodeDisplayName(mp.from, senderName, sizeof(senderName));

        float etx = NeighborGraph::calculateETX(rxRssi, rxSnr, currentCostingSpreadingFactor());

        LOG_INFO("[SR] Direct neighbor %s: RSSI=%d, SNR=%.1f, ETX=%.2f", senderName, rxRssi, rxSnr, etx);

        if (routingGraph) {
            uint32_t currentTime = nowSeconds();
            routingGraph->recordNodeTransmission(mp.from, mp.id, currentTime);
        }
    } else if (notViaMqtt && !isDirectFromSender && mp.relay_node != 0) {
        observeRelayedPacket(mp);
    }

    if (mp.which_payload_variant == meshtastic_MeshPacket_decoded_tag) {
        handleSniffedPayload(mp);
        if (mp.decoded.portnum == meshtastic_PortNum_SIGNAL_ROUTING_APP) {
            preProcessSignalRoutingPacket(&mp);
        }
    }

    maintainGraph(nowSeconds());

    return ProcessMessage::CONTINUE;
}

// Repeating the failed designation or clearing it cannot establish a distinct backup path.
static bool namedBackupStampOk(NodeNum pendingStamp, NodeNum heardFrom, uint8_t designatedByte, NodeNum destination,
                               NodeDB *nodeDB)
{
    if (!nodeDB || designatedByte == NO_NEXT_HOP_PREFERENCE) {
        return true;
    }
    if (designatedByte == nodeDB->getLastByteOfNodeNum(destination)) {
        return true;
    }
    uint8_t stampByte = pendingStamp != 0 ? nodeDB->getLastByteOfNodeNum(pendingStamp) : NO_NEXT_HOP_PREFERENCE;
    if (stampByte == NO_NEXT_HOP_PREFERENCE || stampByte == designatedByte) {
        return false;
    }
    if (heardFrom != 0 && pendingStamp == heardFrom) {
        return false;
    }
    return true;
}

bool SignalRoutingModule::shouldRelayUnicastForCoordination(const meshtastic_MeshPacket *p)
{
    pendingRelayDelayMs = 0;
    pendingUnicastNextHop = 0;
    pendingUnicastFlags = {};
    if (!p || !routingGraph || !nodeDB) {
        return false;
    }

    NodeNum myNode = nodeDB->getNodeNum();
    NodeNum destination = p->to;
    NodeNum sourceNode = p->from;
    NodeNum heardFrom = resolveHeardFrom(p, sourceNode);
    uint32_t currentTime = nowSeconds();

    char destName[64], heardFromName[64], srcName[64];
    getNodeDisplayName(destination, destName, sizeof(destName));
    getNodeDisplayName(heardFrom, heardFromName, sizeof(heardFromName));
    getNodeDisplayName(sourceNode, srcName, sizeof(srcName));

    // Better-placed candidates can still fail; a designated hop needs a backup.
    bool relayerNamed = p->next_hop != NO_NEXT_HOP_PREFERENCE && p->next_hop != nodeDB->getLastByteOfNodeNum(destination);
    // A sender explicitly waiting on us cannot recover through candidates we assume are better placed.
    bool weAreDesignatedHop = p->next_hop != NO_NEXT_HOP_PREFERENCE && p->next_hop == nodeDB->getLastByteOfNodeNum(myNode);
    const char *suppressReason = nullptr;

    // Shared gateway geometry alone does not prove the gateway received this copy.
    NodeNum sourceRelay = routingGraph->getDownstreamRelay(sourceNode);
    NodeNum destRelay = routingGraph->getDownstreamRelay(destination);
    if (!weAreDesignatedHop && sourceRelay != 0 && sourceRelay == destRelay && sourceRelay != myNode &&
        (sourceRelay == heardFrom || routingGraph->hasNodeTransmitted(sourceRelay, p->id, currentTime))) {
        char relayName[64];
        getNodeDisplayName(sourceRelay, relayName, sizeof(relayName));
        LOG_INFO("[SR-DEC] UNICAST SUPPRESS 0x%08x %s->%s: src and dst downstream of %s", p->id, srcName, destName, relayName);
        if (!relayerNamed) {
            return false;
        }
        suppressReason = "src and dst share a relay";
    }

    if (heardFrom != 0 && heardFrom != myNode && heardFrom != sourceNode && !suppressReason && !weAreDesignatedHop) {
        bool heardFromCanReachDest = routingGraph->unicastCanFinish(heardFrom, destination, routePolicy());
        if (heardFromCanReachDest) {
            LOG_INFO("[SR-DEC] UNICAST SUPPRESS 0x%08x %s->%s: heardFrom %s reaches dst", p->id, srcName, destName,
                     heardFromName);
            if (!relayerNamed) {
                return false;
            }
            suppressReason = "heardFrom can finish";
        } else if (hasBetterPositionedSRNeighbor(myNode, heardFrom, destination, p->id)) {
            LOG_INFO("[SR-DEC] UNICAST SUPPRESS 0x%08x %s->%s: neighbor covering %s reaches dst", p->id, srcName, destName,
                     heardFromName);
            if (!relayerNamed) {
                return false;
            }
            suppressReason = "better positioned SR neighbour";
        }
    }

    bool routeVerified = false;
    NodeNum myNextHop = getNextHop(destination, sourceNode, heardFrom, false, &routeVerified);
    // Treating an unknown nonpublisher as a final hop would bypass flood coverage for NodeDB-only destinations.
    const bool canFinish = hasDirectConnectivity(myNode, destination);

    const bool hasCostPath = canFinish || myNextHop != 0;
    if (weAreDesignatedHop && myNextHop == 0 && !canFinish) {
        LOG_INFO("[SR-DEC] UNICAST NACK 0x%08x %s->%s: designated with no route", p->id, srcName, destName);
        if (routingModule) {
            routingModule->sendAckNak(meshtastic_Routing_Error_NO_ROUTE, sourceNode, p->id, p->channel, 0);
        }
        return false;
    }
    if (!hasCostPath && !weAreDesignatedHop) {
        if (!relayerNamed) {
            LOG_INFO("[SR-DEC] UNICAST FLOOD-COVER 0x%08x %s->%s: no path, unique coverage", p->id, srcName, destName);
            return shouldRelayBroadcast(p, true);
        }
        LOG_INFO("[SR-DEC] UNICAST SUPPRESS 0x%08x %s->%s: no route via SR topology", p->id, srcName, destName);
        suppressReason = "no route of our own";
    }

    uint32_t halfAirtime = SR_FALLBACK_HALF_AIRTIME_MS;
    uint32_t airtimeMs = 300;
    if (router && router->getRadioInterface()) {
        airtimeMs = router->getRadioInterface()->getPacketTime(p);
        halfAirtime = std::max(airtimeMs / 2, SR_MIN_RUNG_SPACING_MS);
    }

    // Stamping our own byte makes stock peers refuse the copy and SR peers wait for us again.
    pendingUnicastNextHop = (myNextHop == myNode) ? 0 : myNextHop;

    const NodeNum onwardHop = pendingUnicastNextHop;
    pendingUnicastFlags = {};
    // An unconfirmed designation makes every backup wait for a hop that may be unable to deliver.
    if (!routeVerified && pendingUnicastNextHop != 0) {
        if (pendingUnicastNextHop == heardFrom && !weAreDesignatedHop) {
            LOG_INFO("[SR-DEC] UNICAST SUPPRESS 0x%08x %s->%s: guessed route runs back", p->id, srcName, destName);
            return false;
        }
        LOG_INFO("[SR] Guessed route to %s: no next hop stamped", destName);
        pendingUnicastNextHop = 0;
    }
    // The previous transmitter already routed through us; designating it again creates a forwarding loop.
    if (weAreDesignatedHop && pendingUnicastNextHop == heardFrom) {
        LOG_INFO("[SR] Designated hop 0x%08x: route back to %s, no next hop", p->id, heardFromName);
        pendingUnicastNextHop = 0;
    }
    // A weak hop needs flood assistance, while its intended peer remains the only valid cancellation witness.
    if (pendingUnicastNextHop != 0) {
        const NodeNum intended = pendingUnicastNextHop;
        pendingUnicastFlags.nominatedNextHop = intended;
        const bool strong = (intended == destination) ? hasStrongDeliveryHop(destination) : hasStrongHopTo(intended);
        if (!strong) {
            LOG_INFO("[SR] Weak hop to %08x: flooding (no next_hop stamp)", intended);
            pendingUnicastNextHop = 0;
        }
    }
    // A repeated designation gives the originator an implicit ACK without providing a new delivery path.

    // Relaying local receipts adds contention; remote receipts still need a path to their originator.
    uint32_t destAckWaitMs = 0;
    uint32_t destAckDurationMs = 0;
    {
        uint32_t contention = 2 * halfAirtime;
        if (router && router->getRadioInterface()) {
            contention = router->getRadioInterface()->getTxDelayMsecMaxAtUtil();
        }
        destAckDurationMs = SR_PEER_TURNAROUND_MS + 2 * contention + airtimeMs;
    }
    if (heardFrom == sourceNode) {
        const NodeEdges *srcEdges = routingGraph->getEdgesFrom(sourceNode);
        const Edge *srcToDest = nullptr;
        if (srcEdges) {
            for (uint8_t i = 0; i < srcEdges->edgeCount; i++) {
                if (srcEdges->edges[i].to == destination) {
                    srcToDest = &srcEdges->edges[i];
                    break;
                }
            }
        }
        if (srcToDest) {
            if (p->decoded.portnum == meshtastic_PortNum_ROUTING_APP && p->decoded.request_id != 0) {
                const bool localMeasured = Edge::isMeasured(srcToDest->source) && srcToDest->getEtx() <= cfgPoorLinkEtxThreshold;
                const bool noUsefulOnward = onwardHop == 0 || onwardHop == heardFrom;
                const bool destAlreadyHasIt = srcToDest->hearsUs && hasReportedDirectEdge(destination);
                if (localMeasured && (noUsefulOnward || destAlreadyHasIt)) {
                    LOG_INFO("[SR-DEC] UNICAST SUPPRESS 0x%08x %s->%s: routing ACK retraces the link", p->id, srcName, destName);
                    return false;
                }
            }
            if (srcToDest->hearsUs) {
                destAckWaitMs = destAckDurationMs;
                LOG_INFO("[SR] Unicast 0x%08x: %s hears %s directly, waiting %ums for ACK", p->id, destName, srcName,
                         destAckWaitMs);
            }
        }
    }

    // Quantization prevents small measurement differences from reversing candidate order at colocated peers.
    auto getCandidateCost = [&](NodeNum node) -> uint16_t { return unicastCandidateCost(node, destination, myNextHop); };

    const NodeEdges *myEdges = routingGraph->getEdgesFrom(myNode);

    // These waits share the same origin; adding them would pay twice for the same contention window.
    const uint32_t earliestMs = std::max(ladderTransitionMs(), destAckWaitMs);

    uint32_t slotDelay = 0;
    bool shouldRelay = false;
    uint32_t myDelay = 0;

    if (relayerNamed) {
        uint8_t ourLastByte = nodeDB->getLastByteOfNodeNum(myNode);
        if (ourLastByte == p->next_hop) {
            LOG_INFO("[SR-DEC] UNICAST RELAY 0x%08x %s->%s: designated next_hop (slot 0, %ums)", p->id, srcName, destName,
                     earliestMs);

            const NodeNum keepNominated = pendingUnicastFlags.nominatedNextHop;
            pendingUnicastFlags = {};
            if (getCandidateCost(myNode) < UNICAST_DOWNSTREAM_TIER) {
                pendingUnicastFlags.nominatedNextHop = destination;
                pendingUnicastNextHop = hasStrongDeliveryHop(destination) ? destination : 0;
            } else {
                pendingUnicastFlags.nominatedNextHop = keepNominated;
            }
            pendingRelayDelayMs = earliestMs;
            routingGraph->recordNodeTransmission(myNode, p->id, currentTime);
            return true;
        }

        // Backups must allow the designated relay to contend and finish its frame before taking over.
        NodeNum nextHopNode = resolveRelayIdentity(p->next_hop);
        bool nextHopIsSR = (nextHopNode != 0 && getCapabilityStatus(nextHopNode) == CapabilityStatus::SRactive);
        // The stock and SR timing models anchor contention differently; their waits cannot be combined identically.
        uint32_t reserve = halfAirtime;
        bool haveRadio = router && router->getRadioInterface();
        if (haveRadio) {
            RadioInterface *radio = router->getRadioInterface();
            reserve =
                airtimeMs + (nextHopIsSR ? radio->getTxDelayMsecMaxAtUtil() : radio->getTxDelayMsecWeightedWorst(p->rx_snr));
        }
        slotDelay = (nextHopIsSR || !haveRadio) ? earliestMs + reserve : std::max(earliestMs, reserve);
        LOG_INFO("[SR] Unicast slot 0ms: %s next_hop 0x%02x (%ums reserved)", nextHopIsSR ? "SR" : "stock/unknown", p->next_hop,
                 reserve);
    }

    if (suppressReason) {
        if (relayerNamed && !weAreDesignatedHop &&
            !namedBackupStampOk(pendingUnicastNextHop, heardFrom, p->next_hop, destination, nodeDB)) {
            LOG_INFO("[SR-DEC] UNICAST SUPPRESS 0x%08x %s->%s: backup has no next hop past 0x%02x (%s)", p->id, srcName, destName,
                     p->next_hop, suppressReason);
            pendingUnicastNextHop = 0;
            pendingUnicastFlags = {};
            return false;
        }
        uint8_t rank = 0;
        if (myEdges) {
            for (uint8_t i = 0; i < myEdges->edgeCount; i++) {
                NodeNum nb = myEdges->edges[i].to;
                if (nb == myNode || nb == heardFrom || nb == sourceNode) {
                    continue;
                }
                if (getCapabilityStatus(nb) != CapabilityStatus::SRactive) {
                    continue;
                }
                if (!routingGraph->unicastHeardTransmitter(heardFrom, nb, myNode)) {
                    continue;
                }
                if (((p->id & 1) != 0) ? (nb > myNode) : (nb < myNode)) {
                    rank++;
                }
            }
        }
        uint32_t backupDelay = std::max(slotDelay, earliestMs) + rank * halfAirtime;
        LOG_INFO("[SR-DEC] UNICAST RELAY 0x%08x %s->%s: backup for next_hop 0x%02x "
                 "(%s, delay=%ums)",
                 p->id, srcName, destName, p->next_hop, suppressReason, backupDelay);
        pendingUnicastFlags.designatedNextHop = p->next_hop;
        pendingUnicastFlags.armedHopLimit = p->hop_limit;
        pendingRelayDelayMs = backupDelay;
        routingGraph->recordNodeTransmission(myNode, p->id, currentTime);
        return true;
    }

    UnicastCandidate srCandidates[8];
    uint8_t srCount = 0;

    uint16_t myCost = getCandidateCost(myNode);

    // The self-relay fallback has no self-loop cost but must remain available as a last resort.
    if (myCost == UINT16_MAX && myNextHop == myNode) {
        myCost = 0xFFFEu;
        LOG_INFO("[SR] Best-effort self relay: no coordinated next hop");
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
            if (!routingGraph->unicastHeardTransmitter(heardFrom, nb, myNode)) {
                LOG_INFO("[SR] Unicast candidate %08x skipped (does not hear transmitter)", nb);
                continue;
            }
            uint16_t cost = getCandidateCost(nb);
            if (cost != UINT16_MAX && srCount < 8) {
                srCandidates[srCount++] = {nb, cost};
            }
        }
    }

    // A shared parity tie-break keeps independently computed relay orders consistent while rotating the burden.
    bool preferHighNodeId = (p->id & 1) != 0;
    for (uint8_t i = 1; i < srCount; i++) {
        UnicastCandidate key = srCandidates[i];
        int8_t j = (int8_t)i - 1;
        while (j >= 0) {
            bool costHigher = srCandidates[j].cost > key.cost;
            bool costEqual = srCandidates[j].cost == key.cost;
            bool tiebreakLoses =
                costEqual && (preferHighNodeId ? srCandidates[j].nodeId < key.nodeId : srCandidates[j].nodeId > key.nodeId);
            if (!costHigher && !tiebreakLoses) {
                break;
            }
            srCandidates[j + 1] = srCandidates[j];
            j--;
        }
        srCandidates[j + 1] = key;
    }

    const bool destSr = publishesTopology(destination);
    if (!unicastKeepLastHopSlots(srCandidates, srCount, myNode, destSr)) {
        LOG_INFO("[SR-DEC] UNICAST SUPPRESS 0x%08x %s->%s: last hop reserved", p->id, srcName, destName);
        pendingUnicastNextHop = 0;
        pendingUnicastFlags = {};
        return false;
    }

    // The leader must finish after its actual contention floor; using its nominal wait can undercut slow presets.
    uint32_t leaderWait = 2 * halfAirtime;
    if (router && router->getRadioInterface()) {
        leaderWait = airtimeMs + router->getRadioInterface()->getTxDelayMsecMaxAtUtil();
    }
    leaderWait += SR_PEER_TURNAROUND_MS;
    if (earliestMs + airtimeMs > leaderWait) {
        leaderWait = earliestMs + airtimeMs;
    }
    const uint32_t jitterRange = std::max(halfAirtime / 2, SR_MIN_TIE_BREAK_RANGE_MS);
    const uint32_t jitter = (((uint32_t)(myNode ^ p->id)) % jitterRange) + 1;

    LOG_INFO("[SR] Uni slots 0x%08x to %s: half=%ums %u cands leader=%ums j=%dms", p->id, destName, halfAirtime, srCount,
             leaderWait, (int)jitter);

    uint8_t slotIndex = 0;
    int16_t mySlot = -1;
    bool myDirect = false;
    uint8_t lastNonDirectSlot = 0;
    uint8_t nonDirectSlots = 0;
    bool canReachDestDirectly = hasDirectConnectivity(myNode, destination);
    for (uint8_t i = 0; i < srCount; i++) {
        NodeNum candidate = srCandidates[i].nodeId;
        if (routingGraph->hasNodeTransmitted(candidate, p->id, currentTime)) {
            LOG_INFO("[SR] Unicast slot --: SR node %08x (already transmitted)", candidate);
            continue;
        }
        if (candidate == myNode && p->hop_limit <= 1 && !canReachDestDirectly) {
            LOG_INFO("[SR] Uni slot --: US (%08x) hl=%u no direct edge to dest", myNode, p->hop_limit);
            continue;
        }
        const bool direct = srCandidates[i].cost < UNICAST_DOWNSTREAM_TIER;
        if (!direct) {
            lastNonDirectSlot = slotIndex;
            nonDirectSlots++;
        }
        if (candidate == myNode) {
            mySlot = slotIndex;
            myDirect = direct;
        } else {
            LOG_INFO("[SR] Unicast slot %u: SR node %08x ahead of us (cost=%.2f%s)", slotIndex, candidate,
                     (srCandidates[i].cost & 0x7FFFu) / 100.0f, (srCandidates[i].cost & 0x8000u) ? ", indirect" : "");
        }
        slotIndex++;
    }

    if (mySlot >= 0) {
        const bool lastHopBackup = myDirect && destSr && mySlot == 1;
        const bool nonfinalFlood = !myDirect && nonDirectSlots >= 2 && lastNonDirectSlot == (uint8_t)mySlot;
        pendingUnicastFlags.lastHopBackup = lastHopBackup;
        pendingUnicastFlags.nonfinalFlood = nonfinalFlood;

        int64_t totalDelay;
        if (lastHopBackup) {
            totalDelay = (int64_t)earliestMs + (int64_t)airtimeMs + (int64_t)destAckDurationMs;
        } else if (nonfinalFlood) {
            uint8_t namedSlot = mySlot > 0 ? (uint8_t)(mySlot - 1) : 0;
            totalDelay = (int64_t)srUnicastSlotDelayMs(slotDelay, namedSlot, earliestMs, leaderWait, halfAirtime);
            totalDelay += (int64_t)airtimeMs;
            const NodeNum carryPeer = pendingUnicastNextHop != 0 ? pendingUnicastNextHop : pendingUnicastFlags.nominatedNextHop;
            totalDelay += (int64_t)nextHopCarryWaitMs(carryPeer, airtimeMs, p->rx_snr);
        } else {
            totalDelay = (int64_t)srUnicastSlotDelayMs(slotDelay, (uint8_t)mySlot, earliestMs, leaderWait, halfAirtime);
        }
        totalDelay += (int64_t)jitter;
        if (totalDelay < 0) {
            totalDelay = 0;
        }
        shouldRelay = true;
        myDelay = (uint32_t)totalDelay;
        LOG_INFO("[SR] Unicast slot %ums: US (%08x) — rank %u, cost=%.2f%s%s", myDelay, myNode, (unsigned)mySlot,
                 (getCandidateCost(myNode) & 0x7FFFu) / 100.0f, lastHopBackup ? ", dest-ACK backup" : "",
                 nonfinalFlood ? ", flood salvage" : "");

        if (nonfinalFlood) {
            pendingUnicastNextHop = 0;
        } else if (getCandidateCost(myNode) < UNICAST_DOWNSTREAM_TIER) {
            pendingUnicastFlags.nominatedNextHop = destination;
            pendingUnicastNextHop = hasStrongDeliveryHop(destination) ? destination : 0;
        }
    }

    if (shouldRelay && relayerNamed && !weAreDesignatedHop && !pendingUnicastFlags.nonfinalFlood) {
        if (!namedBackupStampOk(pendingUnicastNextHop, heardFrom, p->next_hop, destination, nodeDB)) {
            LOG_INFO("[SR-DEC] UNICAST SUPPRESS 0x%08x %s->%s: backup has no next hop past 0x%02x", p->id, srcName, destName,
                     p->next_hop);
            pendingUnicastNextHop = 0;
            pendingUnicastFlags = {};
            return false;
        }
        pendingUnicastFlags.designatedNextHop = p->next_hop;
        pendingUnicastFlags.armedHopLimit = p->hop_limit;
    }

    LOG_INFO("[SR-DEC] UNICAST %s 0x%08x %s->%s via %s (delay=%ums)", shouldRelay ? "RELAY" : "SUPPRESS", p->id, srcName,
             destName, heardFromName, shouldRelay ? myDelay : 0u);

    if (shouldRelay) {
        pendingRelayDelayMs = myDelay;
        routingGraph->recordNodeTransmission(myNode, p->id, currentTime);
    }

    return shouldRelay;
}

bool SignalRoutingModule::capsLastHop(const meshtastic_MeshPacket *p)
{
    if (!p || !routingGraph || !nodeDB) {
        return false;
    }
    if (isBroadcast(p->to)) {
        return false;
    }

    NodeNum myNode = nodeDB->getNodeNum();
    NodeNum destination = p->to;

    const NodeEdges *myEdges = routingGraph->getEdgesFrom(myNode);
    if (!myEdges) {
        return false;
    }

    bool destIsDirectAndHearsUs = false;
    for (uint8_t i = 0; i < myEdges->edgeCount; i++) {
        if (myEdges->edges[i].to == destination && myEdges->edges[i].hearsUs) {
            destIsDirectAndHearsUs = true;
            break;
        }
    }
    if (!destIsDirectAndHearsUs) {
        return false;
    }

    // SR peers coordinate themselves; the reduced last-hop budget is needed only to restrain stock relays.
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
    return hasStockNeighbor;
}

bool SignalRoutingModule::shouldUseSignalBasedRouting(const meshtastic_MeshPacket *p)
{
    if (!p || !signalBasedRoutingEnabled || !routingGraph || !nodeDB) {
        return false;
    }

    updateNodeActivityForPacketAndRelay(p);

    if (!isBroadcast(p->to) && p->to == nodeDB->getNodeNum()) {
        return false;
    }

    if (isBroadcast(p->to)) {
        if (!isActiveRoutingRole()) {
            return true;
        }
        return topologyHealthyForBroadcast();
    }

    if (!isActiveRoutingRole()) {
        return false;
    }

    return topologyHealthyForBroadcast();
}

void SignalRoutingModule::commitRelay(PacketId packetId, NodeNum originalHeardFrom, uint32_t txDelayMs)
{
    for (uint8_t i = 0; i < committedRelayCount; i++) {
        if (committedRelays[i].packetId == packetId)
            return;
    }
    CommittedRelay entry;
    entry.packetId = packetId;
    entry.originalHeardFrom = originalHeardFrom;
    entry.txDelayMs = txDelayMs;
    entry.unicastFlags = pendingUnicastFlags;
    if (committedRelayCount < MAX_COMMITTED_RELAYS) {
        committedRelays[committedRelayCount++] = entry;
    } else {
        memmove(committedRelays, committedRelays + 1, (MAX_COMMITTED_RELAYS - 1) * sizeof(CommittedRelay));
        committedRelays[MAX_COMMITTED_RELAYS - 1] = entry;
    }
    LOG_INFO("[SR] Committed relay 0x%08x (from 0x%08x, delay %ums)", packetId, originalHeardFrom, txDelayMs);
}

void SignalRoutingModule::observeForGraph(const meshtastic_MeshPacket &mp)
{
    if (!routingGraph || mp.via_mqtt) {
        return;
    }

    int32_t rxRssi = mp.rx_rssi;
    if (!NeighborGraph::normalizeRxSignal(rxRssi, mp.rx_snr)) {
        return;
    }
    if (!isDirectPacket(mp)) {
        return;
    }
    updateNeighborInfo(mp.from, rxRssi, mp.rx_snr);
    routingGraph->clearDownstreamForDestination(mp.from);
}

bool SignalRoutingModule::hasEstablishedGraph() const
{
    return routingGraph && routingGraph->countDirectNeighbors() > 0;
}

uint32_t SignalRoutingModule::ladderTransitionMs() const
{
    if (router && router->getRadioInterface()) {
        const uint32_t relayFloorMs = router->getRadioInterface()->getRelayFloorMsec();
        if (relayFloorMs > 0) {
            return relayFloorMs;
        }
    }
    return SR_SLOT_ORIGIN_MS;
}

uint32_t SignalRoutingModule::expiredRelayReanchorMs(PacketId packetId, uint32_t slotTimeMs) const
{
    return srExpiredRelayReanchorMs(getCommittedRelayDelay(packetId), ladderTransitionMs(), slotTimeMs);
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

void SignalRoutingModule::maybeScheduleBroadcastRetransmit(const meshtastic_MeshPacket *p)
{
    // An originator has not built its peers' ladder, so the stock worst case is its available insurance bound.
    scheduleT1Broadcast(p, 0, false, 0);
}

void SignalRoutingModule::armDeferredBroadcastRetransmit(const meshtastic_MeshPacket *p, uint32_t staggerMs, uint8_t ladderRungs)
{
    scheduleT1Broadcast(p, staggerMs, true, ladderRungs);
}

void SignalRoutingModule::scheduleT1Broadcast(const meshtastic_MeshPacket *p, uint32_t staggerMs, bool deferred,
                                              uint8_t ladderRungs)
{
    if (!routingGraph || !nodeDB || !p) {
        return;
    }
    if (!isBroadcast(p->to)) {
        return;
    }

    if (p->which_payload_variant == meshtastic_MeshPacket_decoded_tag &&
        p->decoded.portnum == meshtastic_PortNum_SIGNAL_ROUTING_APP) {
        return;
    }

    NodeNum myNode = nodeDB->getNodeNum();
    bool isOriginated = (p->from == 0 || p->from == myNode);
    if (!deferred && !isOriginated) {
        return;
    }

    if (deferred && p->hop_limit == 0) {
        return;
    }

    if (isRetransmitting) {
        return;
    }

    if (!signalBasedRoutingEnabled || !t1RetransmitEnabled) {
        return;
    }

    if (!hasAnyHearsUsNeighbor()) {
        LOG_DEBUG("[SR] No hearsUs neighbor: no T1 for 0x%08x", p->id);
        return;
    }

    for (uint8_t i = 0; i < MAX_PENDING_RETRANSMITS; i++) {
        if (pendingRetransmits[i].packetId == p->id) {
            return;
        }
    }

    uint32_t latestRelayWindowMs = 0;
    uint32_t airtimeMs = 0;
    if (router && router->getRadioInterface()) {
        latestRelayWindowMs = router->getRadioInterface()->getTxDelayMsecWeightedWorst(10.0f);
        airtimeMs = router->getRadioInterface()->getPacketTime(p);
    } else {
        latestRelayWindowMs = 10000;
        airtimeMs = 500;
    }
    // Slow-preset relay ladders can outlast stock contention; insurance must wait for both.
    const uint32_t halfAirtimeMs = std::max(airtimeMs / 2, SR_MIN_RUNG_SPACING_MS);
    const uint32_t ladderSpanMs = ladderTransitionMs() + static_cast<uint32_t>(ladderRungs) * halfAirtimeMs;
    if (ladderSpanMs > latestRelayWindowMs) {
        latestRelayWindowMs = ladderSpanMs;
    }
    uint32_t fireDelayMs = latestRelayWindowMs + airtimeMs + staggerMs;

    meshtastic_MeshPacket *copy = packetPool.allocCopy(*p);
    if (!copy) {
        LOG_WARN("[SR] Pool exhausted: no T1 for 0x%08x", p->id);
        return;
    }
    // An ACK-requesting insurance copy would restart ReliableRouter retries and multiply broadcasts.
    copy->want_ack = false;
    if (deferred) {
        // Deferred copies have not yet spent their relay hop.
        copy->hop_limit--;
        copy->next_hop = NO_NEXT_HOP_PREFERENCE;
    }

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
        LOG_WARN("[SR] Retransmit slots full: no T1 for 0x%08x", p->id);
        packetPool.release(copy);
        return;
    }

    slot->packetId = p->id;
    slot->packet = copy;
    slot->fireAfterMs = millis() + fireDelayMs;
    slot->canceled = false;

    LOG_INFO("[SR] T1 for 0x%08x (%s) in %ums (win %ums + air %ums + stag %ums)", p->id, deferred ? "deferred" : "originated",
             fireDelayMs, latestRelayWindowMs, airtimeMs, staggerMs);

    wakeWithin(fireDelayMs);
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

bool SignalRoutingModule::unicastCanFinish(NodeNum node, NodeNum destination) const
{
    if (!routingGraph) {
        return false;
    }
    return routingGraph->unicastCanFinish(node, destination, routePolicy());
}

uint16_t SignalRoutingModule::unicastCandidateCost(NodeNum node, NodeNum destination, NodeNum myNextHop) const
{
    if (!routingGraph || !nodeDB) {
        return UINT16_MAX;
    }
    return routingGraph->unicastCandidateCost(node, destination, nodeDB->getNodeNum(), myNextHop, routePolicy());
}

bool SignalRoutingModule::unicastDupeCancels(const meshtastic_MeshPacket *p, NodeNum dupeRelayer)
{
    if (!routingGraph || !nodeDB || !p) {
        return false;
    }
    NodeNum myNextHop = getNextHop(p->to, p->from, 0, false);
    UnicastSlotFlags flags = unicastCommitFlags(p->id);
    return routingGraph->unicastDupeCancels(nodeDB->getNodeNum(), p->to, p->id, dupeRelayer, myNextHop, routePolicy(), flags,
                                            p->next_hop, p->relay_node, p->hop_limit);
}

UnicastSlotFlags SignalRoutingModule::unicastCommitFlags(PacketId packetId) const
{
    for (uint8_t i = 0; i < committedRelayCount; i++) {
        if (committedRelays[i].packetId == packetId) {
            return committedRelays[i].unicastFlags;
        }
    }
    return {};
}

bool SignalRoutingModule::hasPricedDeliveryHop(NodeNum dest) const
{
    if (!routingGraph || !nodeDB) {
        return false;
    }
    return unicastCandidateCost(nodeDB->getNodeNum(), dest, 0) < UNICAST_DOWNSTREAM_TIER;
}

bool SignalRoutingModule::hasStrongHopTo(NodeNum peer) const
{
    if (!routingGraph || !nodeDB || peer == 0) {
        return false;
    }
    float cost = routingGraph->hopCost(nodeDB->getNodeNum(), peer);
    return cost > 0.0f && cost <= cfgPoorLinkEtxThreshold;
}

bool SignalRoutingModule::hasStrongDeliveryHop(NodeNum dest) const
{
    return hasPricedDeliveryHop(dest) && hasStrongHopTo(dest);
}

bool SignalRoutingModule::deliveringRelayerIsSR(const meshtastic_MeshPacket *p) const
{
    if (!p) {
        return false;
    }
    NodeNum node = 0;
    if (getHopsAway(*p) == 0) {
        node = p->from;
    } else if (p->relay_node == 0) {
        return false;
    } else {
        node = resolveRelayIdentity(p->relay_node);
    }
    if (node == 0) {
        return false;
    }
    return publishesTopology(node);
}

uint32_t SignalRoutingModule::nextHopCarryWaitMs(NodeNum nextHop, uint32_t airtimeMs, float rxSnr) const
{
    if (nextHop != 0 && publishesTopology(nextHop)) {
        return ladderTransitionMs() + airtimeMs;
    }
    uint32_t worst = 2 * std::max(airtimeMs / 2, SR_MIN_RUNG_SPACING_MS);
    if (router && router->getRadioInterface()) {
        worst = router->getRadioInterface()->getTxDelayMsecWeightedWorst(rxSnr);
    }
    return worst + airtimeMs;
}

uint32_t SignalRoutingModule::namedForwardFollowupDelayMs(NodeNum nextHop, uint32_t airtimeMs, float rxSnr) const
{
    uint32_t carry = nextHopCarryWaitMs(nextHop, airtimeMs, rxSnr);
    uint32_t halfAirtime = std::max(airtimeMs / 2, SR_MIN_RUNG_SPACING_MS);

    uint32_t leaderWait = 2 * halfAirtime;
    if (router && router->getRadioInterface()) {
        leaderWait = airtimeMs + router->getRadioInterface()->getTxDelayMsecMaxAtUtil();
    }
    leaderWait += SR_PEER_TURNAROUND_MS;
    const uint32_t ownTx = leaderWait;
    uint32_t earliestMs = ladderTransitionMs();

    uint32_t slot1 = srUnicastSlotDelayMs(0, 1, earliestMs, leaderWait, halfAirtime);
    uint32_t slot1End = slot1 + std::max(halfAirtime / 2, SR_MIN_TIE_BREAK_RANGE_MS) + airtimeMs;
    return ownTx + std::max(carry, slot1End);
}

bool SignalRoutingModule::areAllNeighborsCovered(const meshtastic_MeshPacket *p, NodeNum *uniqueNeighbor)
{
    if (uniqueNeighbor) {
        *uniqueNeighbor = 0;
    }
    if (!routingGraph || !nodeDB || !p) {
        return false;
    }

    NodeNum dupeRelayer = 0;
    if (p->relay_node != 0) {
        dupeRelayer = resolveRelayIdentity(p->relay_node, p->rx_rssi, p->rx_snr);
        if (dupeRelayer == 0) {
            dupeRelayer = resolveHeardFrom(p, p->from);
            if (isPlaceholderNode(dupeRelayer)) {
                dupeRelayer = 0;
            }
        }
    }

    NodeNum myNode = nodeDB->getNodeNum();

    if (!isBroadcast(p->to)) {
        bool cancel = unicastDupeCancels(p, dupeRelayer);
        char destName[64];
        getNodeDisplayName(p->to, destName, sizeof(destName));
        if (dupeRelayer == 0 || dupeRelayer == myNode) {
            LOG_INFO("[SR] Uni dupe 0x%08x to %s: relay 0x%02x %s → %s", p->id, destName, p->relay_node,
                     dupeRelayer == myNode ? "is us" : "unresolved", cancel ? "cancel" : "keep");
        } else {
            char relayerName[64];
            getNodeDisplayName(dupeRelayer, relayerName, sizeof(relayerName));
            LOG_INFO("[SR] Uni dupe 0x%08x to %s from %s: %s", p->id, destName, relayerName,
                     cancel ? "canceling relay" : "keeping relay");
        }
        return cancel;
    }

    if (dupeRelayer == 0) {
        LOG_INFO("[SR] Coverage 0x%08x: relay 0x%02x unresolved", p->id, p->relay_node);
        return false;
    }
    if (dupeRelayer == myNode) {
        LOG_INFO("[SR] Coverage 0x%08x: dupe is our own, keeping relay", p->id);
        return false;
    }

    char relayerName[64];
    getNodeDisplayName(dupeRelayer, relayerName, sizeof(relayerName));

    CommittedRelay *relay = nullptr;
    for (uint8_t i = 0; i < committedRelayCount; i++) {
        if (committedRelays[i].packetId == p->id) {
            relay = &committedRelays[i];
            break;
        }
    }

    NodeNum originalHeardFrom = relay ? relay->originalHeardFrom : 0;

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
            LOG_INFO("[SR] pkt=0x%08x: accumulated transmitter %s (%u total)", p->id, relayerName, relay->heardTransmitterCount);
        } else if (!alreadyTracked) {
            LOG_INFO("[SR] pkt=0x%08x: transmitter table full (%u), cannot add %s", p->id, relay->heardTransmitterCount,
                     relayerName);
        }
    }

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

    NeighborGraph::CoveragePolicy policy = coveragePolicy();
    NodeNum uniqueFor = routingGraph->uniqueCoverageNeighbor(myNode, coveredBy, coveredByCount, cfgPoorLinkEtxThreshold, &policy);
    bool unique = uniqueFor != 0;
    if (uniqueNeighbor) {
        *uniqueNeighbor = uniqueFor;
    }

    if (unique) {
        LOG_INFO("[SR] Bcast dupe 0x%08x from %s: %u TX, keeping", p->id, relayerName, coveredByCount);
    } else {
        LOG_INFO("[SR] Bcast dupe 0x%08x from %s: %u TX cover all", p->id, relayerName, coveredByCount);
    }

    return !unique;
}

bool SignalRoutingModule::shouldRelay(const meshtastic_MeshPacket *p)
{
    pendingRelayDelayMs = 0;
    pendingUnicastNextHop = 0;
    pendingUnicastFlags = {};
    if (!p) {
        return false;
    }
    if (!routingGraph || !nodeDB) {
        return true;
    }

    if (isBroadcast(p->to)) {
        return shouldRelayBroadcast(p);
    }

    char destName[64], senderName[64];
    getNodeDisplayName(p->to, destName, sizeof(destName));
    getNodeDisplayName(p->from, senderName, sizeof(senderName));
    LOG_INFO("[SR] Considering unicast relay 0x%08x %s to %s (hl=%d)", p->id, senderName, destName, p->hop_limit);

    if (!topologyHealthyForUnicast(p->to)) {
        const uint8_t ourByte = nodeDB->getLastByteOfNodeNum(nodeDB->getNodeNum());
        const uint8_t destByte = nodeDB->getLastByteOfNodeNum(p->to);
        const bool namedForeignNextHop =
            p->next_hop != NO_NEXT_HOP_PREFERENCE && p->next_hop != destByte && p->next_hop != ourByte;
        const bool weAreDesignated = p->next_hop != NO_NEXT_HOP_PREFERENCE && p->next_hop == ourByte;
        const bool knownInNodeDb = nodeDB->getMeshNode(p->to) != nullptr;
        const bool knownDownstream = routingGraph->isDownstream(p->to);

        if (namedForeignNextHop && (knownInNodeDb || knownDownstream)) {
            LOG_INFO("[SR-DEC] UNICAST DEFER 0x%08x %s->%s: named next_hop 0x%02x, no SR route yet", p->id, senderName, destName,
                     p->next_hop);
        } else if (weAreDesignated) {
        } else if (knownInNodeDb || knownDownstream) {
            LOG_INFO("[SR-DEC] UNICAST FLOOD-COVER 0x%08x: %s to %s, no stampable route yet", p->id, senderName, destName);
            return shouldRelayBroadcast(p, true);
        } else {
            LOG_INFO("[SR-DEC] UNICAST SUPPRESS 0x%08x %s->%s: unknown destination", p->id, senderName, destName);
            return false;
        }
    }

    NodeNum sourceNode = p->from;

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

    if (heardFrom == sourceNode) {
        NodeNum nextHop = getNextHop(p->to, sourceNode, heardFrom, false);
        if (nextHop != 0 && nextHop != nodeDB->getNodeNum()) {
            bool nextHopHeardFromSource = false;
            const NodeEdges *sourceEdges = routingGraph->getEdgesFrom(sourceNode);
            if (sourceEdges) {
                for (uint8_t i = 0; i < sourceEdges->edgeCount; i++) {
                    if (sourceEdges->edges[i].to == nextHop) {
                        nextHopHeardFromSource = true;
                        break;
                    }
                }
            }
            if (!nextHopHeardFromSource) {
                const NodeEdges *nextHopEdges = routingGraph->getEdgesFrom(nextHop);
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
                // Topology cannot prove receipt of this frame; marginal links still require a delayed backup.
                bool canReachDestDirectly = hasDirectConnectivity(nodeDB->getNodeNum(), p->to);
                if (p->hop_limit == 0 || (p->hop_limit <= 1 && !canReachDestDirectly)) {
                    LOG_INFO("[SR-DEC] UNICAST SUPPRESS 0x%08x %s->%s: %s heard source, no headroom (hl=%u, dir=%d)", p->id,
                             senderName, destName, nextHopName, p->hop_limit, canReachDestDirectly);
                    return false;
                }
                LOG_INFO("[SR-DEC] UNICAST DEFER 0x%08x %s->%s: %s likely has it, backup scheduled", p->id, senderName, destName,
                         nextHopName);
            }
        }
    }

    return shouldRelayUnicastForCoordination(p);
}

bool SignalRoutingModule::shouldRelayBroadcast(const meshtastic_MeshPacket *p, bool unicastFloodCoverage)
{
    pendingRelayDelayMs = 0;
    pendingUnicastNextHop = 0;
    pendingUnicastFlags = {};
    if (!p) {
        return false;
    }
    if (!p || !routingGraph || !nodeDB) {
        return p != nullptr;
    }

    NodeNum ourNode = nodeDB->getNodeNum();
    uint32_t currentTime = nowSeconds();
    if (routingGraph->hasNodeTransmitted(ourNode, p->id, currentTime)) {
        LOG_INFO("[SR] Skipping relay decision for rebroadcast of our packet %08x", p->id);
        return false;
    }

    if (!isBroadcast(p->to) && !unicastFloodCoverage) {
        return shouldRelayUnicastForCoordination(p);
    }

    if (!isActiveRoutingRole()) {
        return false;
    }

    uint32_t packetReceivedTimestamp = nowSeconds();

    if (p->which_payload_variant == meshtastic_MeshPacket_decoded_tag &&
        p->decoded.portnum == meshtastic_PortNum_SIGNAL_ROUTING_APP) {
        preProcessSignalRoutingPacket(p);
    }

    // Thin topology cannot justify suppressing a copy needed for delivery.
    if (!topologyHealthyForBroadcast()) {
        LOG_INFO("[SR] Relay 0x%08x unranked: topology too thin to judge, direct=%u", p->from,
                 (unsigned)(routingGraph ? routingGraph->countDirectNeighbors() : 0));
        return true;
    }

    NodeNum myNode = nodeDB->getNodeNum();
    NodeNum sourceNode = p->from;
    NodeNum heardFrom = resolveHeardFrom(p, sourceNode);

    NodeNum relayForSource = routingGraph->getDownstreamRelay(sourceNode);
    NodeNum relayForDest = routingGraph->getDownstreamRelay(p->to);
    bool weAreRelayForSource = (relayForSource != 0 && relayForSource == myNode);
    bool weAreRelayForDest = (relayForDest != 0 && relayForDest == myNode);
    size_t downstreamCount = (weAreRelayForSource || weAreRelayForDest) ? routingGraph->getDownstreamCountForRelay(myNode) : 0;

    uint32_t relayDecisionTime = packetReceivedTimestamp;

    uint32_t halfAirtime = SR_FALLBACK_HALF_AIRTIME_MS;
    if (router && router->getRadioInterface()) {
        uint32_t airtime = router->getRadioInterface()->getPacketTime(p);
        halfAirtime = std::max(airtime / 2, SR_MIN_RUNG_SPACING_MS);
    }

    NeighborGraph::CoveragePolicy coveragePolicy = this->coveragePolicy();

    // Static scratch sets protect the 4 KB loop stack; relay decisions are serialized and nonrecursive.
    static NodeSet alreadyCovered;
    alreadyCovered.clear();
    // Ownership predicts a future relay; it does not prove receipt of a frame already on air.
    alreadyCovered.insert(sourceNode);
    alreadyCovered.insert(heardFrom);
    {
        static NodeNum reached[NODE_SET_MAX];
        size_t n =
            routingGraph->getCoverageIfRelays(heardFrom, reached, NODE_SET_MAX, nullptr, 0, myNode, &coveragePolicy, false);
        for (size_t i = 0; i < n; i++) {
            alreadyCovered.insert(reached[i]);
        }
    }

    static NodeSet candidates;
    candidates.clear();
    const NodeEdges *myEdges = routingGraph->getEdgesFrom(myNode);

    const NodeEdges *heardFromEdges = routingGraph->getEdgesFrom(heardFrom);
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
                    LOG_INFO("[SR] Pre-coverage: %s excluded (ETX=%.1f >= %.1f)", neighborName, heardFromEdges->edges[j].getEtx(),
                             cfgPoorLinkEtxThreshold);
                    break;
                }
            }
        }
    }
    if (myEdges) {
        for (uint8_t i = 0; i < myEdges->edgeCount; i++) {
            NodeNum neighbor = myEdges->edges[i].to;
            if (neighbor == heardFrom || neighbor == sourceNode || !myEdges->edges[i].hearsUs) {
                continue;
            }

            if (getCapabilityStatus(neighbor) != CapabilityStatus::SRactive && !isImmediateRelayRouter(neighbor)) {
                continue;
            }
            candidates.insert(neighbor);
        }
    }

    candidates.insert(myNode);

    bool preferHighNodeId = (p->id & 1) != 0;
    // Stock reservations estimate hardware-seeded timing; SR rungs schedule actual transmissions.
    uint32_t slotTimeMs = 0;
    uint32_t relayFloorMs = 0;
    if (router && router->getRadioInterface()) {
        slotTimeMs = router->getRadioInterface()->getSlotTimeMsec();
        relayFloorMs = router->getRadioInterface()->getRelayFloorMsec();
    }
    const uint8_t windowPositions =
        (slotTimeMs > 0 && relayFloorMs > slotTimeMs) ? static_cast<uint8_t>(relayFloorMs / slotTimeMs - 1) : 0;
    SrPositionAllocator positions(slotTimeMs, halfAirtime, ladderTransitionMs(), windowPositions);
    bool shouldRelay = false;
    uint32_t myDelay = 0;
    const char *decisionReason = "no unique coverage";

    uint8_t slotsGiven = 0;
    // Insurance ordering must include earlier stock positions or different insurers can share a rung.
    uint8_t stockCandidates = 0;

    uint8_t reservedSlots = 0;

    uint16_t absorbedTotal = 0;
    NodeNum absorbedBy[4] = {0, 0, 0, 0};
    uint8_t absorbedCredit[4] = {0, 0, 0, 0};
    uint8_t absorbedCount = 0;
    auto noteAbsorbed = [&](NodeNum relay, uint16_t before) {
        uint16_t credit = alreadyCovered.count > before ? (uint16_t)(alreadyCovered.count - before) : 0;
        absorbedTotal += credit;
        if (absorbedCount < 4) {
            absorbedBy[absorbedCount] = relay;
            absorbedCredit[absorbedCount] = (uint8_t)(credit > 255 ? 255 : credit);
            absorbedCount++;
        }
    };

    // Peers can compute the same rung; positive jitter separates them without advancing either ahead of its floor.
    const uint32_t rungJitterRange = std::max(halfAirtime / 2, SR_MIN_TIE_BREAK_RANGE_MS);
    const uint32_t rungJitter = (((uint32_t)(myNode ^ p->id)) % rungJitterRange) + 1;

    uint16_t initialCandidates = candidates.count;

    if (myEdges) {
        for (uint8_t i = 0; i < myEdges->edgeCount; i++) {
            NodeNum neighbor = myEdges->edges[i].to;
            if (neighbor == heardFrom || neighbor == sourceNode) {
                continue;
            }
            if (!isImmediateRelayRouter(neighbor)) {
                continue;
            }

            // A stock router that missed this frame cannot justify delaying our copy.
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
                LOG_INFO("[SR] Skip stock %08x: no evidence it hears %08x", neighbor, heardFrom);
                candidates.erase(neighbor);
                continue;
            }

            candidates.erase(neighbor);
            stockCandidates++;

            const uint32_t reservedAt = positions.takeReserved();
            if (routingGraph->hasNodeTransmitted(neighbor, p->id, currentTime)) {
                uint16_t before = alreadyCovered.count;
                static NodeNum stockReached[NODE_SET_MAX];
                size_t n = routingGraph->getCoverageIfRelays(neighbor, stockReached, NODE_SET_MAX, nullptr, 0, myNode,
                                                             &coveragePolicy, false);
                for (size_t i = 0; i < n; i++) {
                    alreadyCovered.insert(stockReached[i]);
                }
                alreadyCovered.insert(neighbor);
                noteAbsorbed(neighbor, before);
                LOG_INFO("[SR] Slot %ums: stock %08x (already TX, absorbed %u)", reservedAt, neighbor,
                         (unsigned)absorbedCredit[absorbedCount ? absorbedCount - 1 : 0]);
            } else {
                LOG_INFO("[SR] Slot %ums: stock %08x (expected)", reservedAt, neighbor);
                slotsGiven++;
                reservedSlots++;
            }
        }
    }

    static NodeSet srPeers;
    srPeers = candidates;

    auto absorbRelayCoverage = [&](NodeNum relay) {
        uint16_t before = alreadyCovered.count;

        static NodeNum cov[NODE_SET_MAX];
        size_t n = routingGraph->getCoverageIfRelays(relay, cov, NODE_SET_MAX, nullptr, 0, myNode, &coveragePolicy);
        for (size_t i = 0; i < n; i++) {
            alreadyCovered.insert(cov[i]);
        }
        alreadyCovered.insert(relay);
        noteAbsorbed(relay, before);
    };

    static constexpr uint8_t UNCOVERED_LOG = 6;
    static constexpr uint8_t EVALUATED_LOG = 4;
    NodeNum uncovered[UNCOVERED_LOG];
    uint8_t uncoveredLen = 0;
    {
        static NodeNum mine[NODE_SET_MAX];
        size_t n = routingGraph->getCoverageIfRelays(myNode, mine, NODE_SET_MAX, nullptr, 0, myNode, &coveragePolicy);
        for (size_t i = 0; i < n && uncoveredLen < UNCOVERED_LOG; i++) {
            if (!alreadyCovered.contains(mine[i])) {
                uncovered[uncoveredLen++] = mine[i];
            }
        }
    }
    struct EvalCand {
        NodeNum id;
        uint8_t uniqueCov;
        uint8_t totalCov;
        uint16_t costFixed;
    };
    EvalCand evaluated[EVALUATED_LOG];
    uint8_t evaluatedLen = 0;
    for (uint16_t ci = 0; ci < candidates.count && evaluatedLen < EVALUATED_LOG; ci++) {
        NodeNum cand = candidates.nodes[ci];
        if (routingGraph->hasNodeTransmitted(cand, p->id, currentTime)) {
            continue;
        }
        static NodeNum cov[NODE_SET_MAX];
        size_t total = routingGraph->getCoverageIfRelays(cand, cov, NODE_SET_MAX, nullptr, 0, myNode, &coveragePolicy);
        size_t unique = 0;
        float costSum = 0.0f;
        size_t validCosts = 0;
        for (size_t i = 0; i < total; i++) {
            if (alreadyCovered.contains(cov[i])) {
                continue;
            }
            unique++;
            float c = routingGraph->hopCost(cand, cov[i]);
            if (c > 0.0f) {
                costSum += c;
                validCosts++;
            }
        }
        uint16_t costFixed = 0;
        if (validCosts > 0) {
            costFixed = static_cast<uint16_t>((costSum / validCosts) * 100.0f);
            costFixed = static_cast<uint16_t>(costFixed / SR_COST_BUCKET_FIXED * SR_COST_BUCKET_FIXED);
        }
        evaluated[evaluatedLen++] = {cand, static_cast<uint8_t>(unique > 255 ? 255 : unique),
                                     static_cast<uint8_t>(total > 255 ? 255 : total), costFixed};
    }
    uint8_t mySlotIndex = 0;
    uint16_t preCoveredAtRank = alreadyCovered.count;
    uint16_t candidatesAtRank = candidates.count;

    static NodeSet lateCandidates;
    lateCandidates.clear();
    for (uint16_t i = 0; i < candidates.count;) {
        NodeNum id = candidates.nodes[i];
        const bool earlyOk = coveragePolicy.isConfiguredRouter && coveragePolicy.isConfiguredRouter(coveragePolicy.ctx, id);
        if (earlyOk) {
            i++;
        } else {
            candidates.erase(id);
            lateCandidates.insert(id);
        }
    }

    auto placePhase = [&](NodeSet &pool, bool early) {
        while (!pool.empty()) {
            RelayCandidate best = routingGraph->findBestRelayCandidate(pool, alreadyCovered, currentTime, p->id, preferHighNodeId,
                                                                       sourceNode, myNode, &coveragePolicy);
            if (best.nodeId == 0) {
                break;
            }

            pool.erase(best.nodeId);

            if (routingGraph->hasNodeTransmitted(best.nodeId, p->id, currentTime)) {
                absorbRelayCoverage(best.nodeId);
                LOG_INFO("[SR] Slot --: SR %08x (already TX, coverage absorbed)", best.nodeId);
                continue;
            }

            const uint32_t rung = early ? positions.takeRung() : positions.takeLateRung();
            if (best.nodeId == myNode) {
                shouldRelay = true;
                myDelay = rung + rungJitter;
                mySlotIndex = slotsGiven;
                decisionReason = "SR slot assignment";
                LOG_INFO("[SR] Slot %ums: US (%08x)%s j=%ums", myDelay, myNode, best.tier > 0 ? " (bidi)" : "", rungJitter);
                break;
            }

            LOG_INFO("[SR] Slot %ums: SR node %08x (coverage=%u/%u, cost=%.2f%s)", rung, best.nodeId, best.coverageCount,
                     best.totalCoverage, best.getAvgCost(), best.tier > 0 ? ", bidi" : "");
            slotsGiven++;
            // An assigned slot is no proof of coverage; our later rung must survive a silent candidate.
        }
    };

    placePhase(candidates, true);
    if (!shouldRelay) {
        placePhase(lateCandidates, false);
    }

    if (!shouldRelay && (weAreRelayForSource || weAreRelayForDest)) {
        NodeNum forcedFor = weAreRelayForSource ? sourceNode : p->to;
        LOG_INFO("[SR-DEC] BROADCAST RELAY (forced) 0x%08x: for %08x (down=%u)", p->id, forcedFor,
                 static_cast<unsigned int>(downstreamCount));
        shouldRelay = true;
        myDelay = positions.firstFreeMs() + rungJitter;
        decisionReason = "downstream relay override";
    }

    // The sole broadcast copy is also the sender's implicit delivery acknowledgement.
    if (!unicastFloodCoverage && !shouldRelay && initialCandidates <= 1) {
        shouldRelay = true;
        // Other nodes may also believe they are alone, so separation is still necessary.
        myDelay = positions.firstFreeMs() + rungJitter;
        decisionReason = "sole candidate";
    }

    // Direct broadcast text still needs an implicit ACK even when every neighbor is already covered.
    if (!unicastFloodCoverage && !shouldRelay && slotsGiven == 0 &&
        p->which_payload_variant == meshtastic_MeshPacket_decoded_tag &&
        p->decoded.portnum == meshtastic_PortNum_TEXT_MESSAGE_APP && isDirectPacket(*p)) {
        uint32_t ackDelay = 0;
        uint8_t ackAhead = 0;
        if (planAcknowledgement(p, sourceNode, heardFrom, halfAirtime, &ackDelay, &ackAhead)) {
            shouldRelay = true;
            myDelay = ackDelay + rungJitter;
            slotsGiven = ackAhead;
            decisionReason = "acknowledgement";
        }
    }

    char sourceName[64], heardFromName[64];
    getNodeDisplayName(sourceNode, sourceName, sizeof(sourceName));
    getNodeDisplayName(heardFrom, heardFromName, sizeof(heardFromName));

    if (!shouldRelay && reservedSlots > 0) {
        decisionReason = "stock router expected";
    }

    // These diagnostics distinguish graph disagreement from scheduling disagreement.
    const char *viaTag = "";
    if (strcmp(decisionReason, "SR slot assignment") == 0) {
        viaTag = ", via=rank";
    } else if (strcmp(decisionReason, "downstream relay override") == 0) {
        viaTag = ", via=downstream";
    } else if (strcmp(decisionReason, "sole candidate") == 0) {
        viaTag = ", via=sparse";
    } else if (strcmp(decisionReason, "acknowledgement") == 0) {
        viaTag = ", via=ack";
    }
    char sched[544];
    int sp = snprintf(sched, sizeof(sched), "[SR] Slot scheduling 0x%08x: halfAirtime=%ums, candidates=%u, pre=%u", p->id,
                      halfAirtime, (unsigned)candidatesAtRank, (unsigned)preCoveredAtRank);
    if (sp > 0 && uncoveredLen > 0 && (size_t)sp < sizeof(sched)) {
        sp += snprintf(sched + sp, sizeof(sched) - (size_t)sp, ", unc=");
        for (uint8_t i = 0; i < uncoveredLen && sp > 0 && (size_t)sp < sizeof(sched); i++) {
            sp += snprintf(sched + sp, sizeof(sched) - (size_t)sp, "%s!%08x", i ? "," : "", uncovered[i]);
        }
    }
    if (sp > 0 && (size_t)sp < sizeof(sched)) {
        sp +=
            snprintf(sched + sp, sizeof(sched) - (size_t)sp, ", slot=%u, slots=%u", (unsigned)mySlotIndex, (unsigned)slotsGiven);
        if (reservedSlots > 0) {
            sp += snprintf(sched + sp, sizeof(sched) - (size_t)sp, "(res=%u)", (unsigned)reservedSlots);
        }
        if (sp > 0 && (size_t)sp < sizeof(sched)) {
            sp += snprintf(sched + sp, sizeof(sched) - (size_t)sp, "%s", viaTag);
        }
    }
    if (sp > 0 && evaluatedLen > 0 && (size_t)sp < sizeof(sched)) {
        sp += snprintf(sched + sp, sizeof(sched) - (size_t)sp, ", cand=");
        for (uint8_t i = 0; i < evaluatedLen && sp > 0 && (size_t)sp < sizeof(sched); i++) {
            sp += snprintf(sched + sp, sizeof(sched) - (size_t)sp, "%s!%08x(%u/%u,%u)", i ? "," : "", evaluated[i].id,
                           (unsigned)evaluated[i].uniqueCov, (unsigned)evaluated[i].totalCov, (unsigned)evaluated[i].costFixed);
        }
    }
    if (sp > 0) {
        LOG_INFO("%s", sched);
    }

    LOG_INFO("[SR-DEC] BROADCAST %s 0x%08x: from %s via %s (%s, delay=%ums, slots=%u/res=%u, abs=%u)",
             shouldRelay ? "RELAY" : "SUPPRESS", p->id, sourceName, heardFromName, decisionReason, myDelay, (unsigned)slotsGiven,
             (unsigned)reservedSlots, (unsigned)absorbedTotal);
    for (uint8_t i = 0; i < absorbedCount; i++) {
        LOG_INFO("[SR] Absorbed 0x%08x: %08x covered %u", p->id, absorbedBy[i], (unsigned)absorbedCredit[i]);
    }

    if (shouldRelay) {
        pendingRelayDelayMs = myDelay;
        routingGraph->recordNodeTransmission(myNode, p->id, currentTime);
    } else if (slotsGiven > 0) {
        // Insurance replaces silent ranked relays; stock windows and completed transmissions must not add extra delay.
        uint8_t rank = 0;
        const uint32_t nowSecs = nowSeconds();
        for (uint16_t i = 0; i < srPeers.count; i++) {
            NodeNum peer = srPeers.nodes[i];
            if (peer == myNode) {
                continue;
            }
            if (routingGraph && routingGraph->hasNodeTransmitted(peer, p->id, nowSecs)) {
                continue;
            }
            if (preferHighNodeId ? (peer > myNode) : (peer < myNode)) {
                rank++;
            }
        }

        const uint8_t ladderRungs = srPeers.count > 0 ? static_cast<uint8_t>(srPeers.count - 1) : 0;
        armDeferredBroadcastRetransmit(p, rank * halfAirtime, ladderRungs);
    }

    return shouldRelay;
}

NodeNum SignalRoutingModule::getNextHop(NodeNum destination, NodeNum sourceNode, NodeNum heardFrom, bool allowOpportunistic,
                                        bool *verified)
{
    if (verified)
        *verified = false;
    if (!routingGraph) {
        LOG_WARN("[SR] No graph available for routing");
        return 0;
    }

    uint32_t currentTime = nowSeconds();

    char destName[64];
    getNodeDisplayName(destination, destName, sizeof(destName));

    Route route = routingGraph->calculateRoute(destination, currentTime, routePolicy());

    float routeCost = route.getCost();

    if (route.nextHop != 0) {
        // A bare inbound gateway is not proof that a designated hop can deliver.
        auto stampable = [&]() -> bool {
            if (route.verified) {
                return true;
            }
            NodeNum me = nodeDB ? nodeDB->getNodeNum() : 0;
            return me != 0 && routingGraph->downstreamChainEgress(destination, me).node == route.nextHop;
        };

        if (hopHealth.isSuspect(destination, route.nextHop, millis())) {
            NodeNum excluded[1] = {route.nextHop};
            NeighborGraph::RoutePolicy exclPolicy = routePolicy();
            exclPolicy.excluded = excluded;
            exclPolicy.excludedCount = 1;
            Route alt = routingGraph->calculateRoute(destination, currentTime, exclPolicy);
            bool altStampable =
                alt.nextHop != 0 &&
                (alt.verified || (nodeDB && nodeDB->getNodeNum() != 0 &&
                                  routingGraph->downstreamChainEgress(destination, nodeDB->getNodeNum()).node == alt.nextHop));
            if (altStampable) {
                route = alt;
                routeCost = route.getCost();
            }
        }
        char nextHopName[64];
        getNodeDisplayName(route.nextHop, nextHopName, sizeof(nextHopName));

        LOG_INFO("[SR] Route to %s via %s (cost: %.2f, hops: %u%s)", destName, nextHopName, routeCost, route.hops,
                 route.verified ? "" : ", unverified");

        if (routeCost > 10.0f) {
            LOG_WARN("[SR] High-cost route to %s (%.2f) - poor link quality expected", destName, routeCost);
        }

        bool nextHopCanHearTransmitter = true;
        bool connectivityUnknown = false;

        if (heardFrom != 0 && route.nextHop != heardFrom) {
            nextHopCanHearTransmitter = hasVerifiedConnectivity(heardFrom, route.nextHop, &connectivityUnknown);

            if (!nextHopCanHearTransmitter) {
                char heardFromName[64];
                getNodeDisplayName(heardFrom, heardFromName, sizeof(heardFromName));

                if (connectivityUnknown) {
                    LOG_INFO("[SR] Route via %s rejected: %s unverifiable", nextHopName, heardFromName);
                } else {
                    LOG_INFO("[SR] Route via %s rejected - no connectivity to transmitter %s", nextHopName, heardFromName);
                }
            }
        }

        // The next hop must hear our radio; connectivity to the previous transmitter cannot deliver this copy.
        NodeNum myNodeForStamp = nodeDB ? nodeDB->getNodeNum() : 0;
        bool nextHopHearsUs = (route.nextHop == destination || route.nextHop == myNodeForStamp);
        if (!nextHopHearsUs && myNodeForStamp != 0) {
            const NodeEdges *stampEdges = routingGraph->getEdgesFrom(myNodeForStamp);
            if (stampEdges) {
                for (uint8_t i = 0; i < stampEdges->edgeCount; i++) {
                    if (stampEdges->edges[i].to == route.nextHop && stampEdges->edges[i].hearsUs) {
                        nextHopHearsUs = true;
                        break;
                    }
                }
            }
        }
        if (nextHopCanHearTransmitter && !nextHopHearsUs && route.nextHop != destination) {
            LOG_INFO("[SR] Route via %s rejected: does not hear us", nextHopName);
            nextHopCanHearTransmitter = false;
        }

        if (nextHopCanHearTransmitter) {
            if (allowOpportunistic && routeCost > 2.0f) {
                NodeNum betterNeighbor = findBetterPositionedNeighbor(destination, sourceNode, heardFrom, routeCost, currentTime);
                if (betterNeighbor != 0) {
                    return betterNeighbor;
                }
            }

            if (verified)
                *verified = stampable();
            return route.nextHop;
        }

        NodeNum myNode = nodeDB ? nodeDB->getNodeNum() : 0;
        if (myNode != 0) {
            const NodeEdges *myEdges = routingGraph->getEdgesFrom(myNode);
            if (myEdges) {
                for (uint8_t i = 0; i < myEdges->edgeCount; i++) {
                    if (myEdges->edges[i].to == route.nextHop && myEdges->edges[i].hearsUs) {
                        LOG_INFO("[SR] Route via %s kept: unverified but hearsUs", nextHopName);
                        if (verified)
                            *verified = stampable();
                        return route.nextHop;
                    }
                }
            }
        }

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

        if (myNode != 0) {
            LOG_INFO("[SR] No verified next hop, no hearsUs: self relay");
            return myNode;
        }
    }

    NodeNum relayForDest = routingGraph->downstreamChainEgress(destination, nodeDB ? nodeDB->getNodeNum() : 0).node;
    if (relayForDest != 0 && nodeDB) {
        bool relayCanHearTransmitter = true;
        bool connectivityUnknown = false;
        if (heardFrom != 0 && relayForDest != heardFrom) {
            relayCanHearTransmitter = hasVerifiedConnectivity(heardFrom, relayForDest, &connectivityUnknown);
        }

        // An opportunistic hop needs confirmed delivery from our radio, not just from the previous transmitter.
        bool relayHearsUs = false;
        if (relayCanHearTransmitter && !connectivityUnknown) {
            const NodeEdges *myEdges = routingGraph->getEdgesFrom(nodeDB->getNodeNum());
            if (myEdges) {
                for (uint8_t i = 0; i < myEdges->edgeCount; i++) {
                    if (myEdges->edges[i].to == relayForDest && myEdges->edges[i].hearsUs) {
                        relayHearsUs = true;
                    }
                    if (myEdges->edges[i].to == relayForDest && relayHearsUs) {
                        char gwName[64];
                        getNodeDisplayName(relayForDest, gwName, sizeof(gwName));
                        LOG_INFO("[SR] No direct route to %s, but forwarding to relay %s", destName, gwName);
                        if (verified)
                            *verified = true;
                        return relayForDest;
                    }
                }
            }
            if (!relayHearsUs) {
                char gwName[64];
                getNodeDisplayName(relayForDest, gwName, sizeof(gwName));
                LOG_INFO("[SR] Relay %s skipped: does not hear us", gwName);
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

    if (allowOpportunistic) {
        NodeNum betterNeighbor =
            findBetterPositionedNeighbor(destination, sourceNode, heardFrom, std::numeric_limits<float>::infinity(), currentTime);
        if (betterNeighbor != 0) {
            return betterNeighbor;
        }
    }

    NodeNum myNode = nodeDB ? nodeDB->getNodeNum() : 0;
    if (routingGraph && nodeDB && myNode != 0 && heardFrom != sourceNode) {
        bool isDirectNeighbor = false;
        float directEtx = 1e9f;

        const NodeEdges *myEdges = routingGraph->getEdgesFrom(myNode);
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
            LOG_INFO("[SR] Deliver direct to %s (ETX=%.2f)", destName, directEtx);
            return destination;
        }
    }

    if (myNode != 0 && routingGraph->getDownstreamRelay(destination) == myNode) {
        LOG_INFO("[SR] We are the designated relay for %s - delivering directly", destName);

        routingGraph->updateDownstream(destination, myNode, 1.0f, nowSeconds());
        return destination;
    }

    if (routingGraph && nodeDB) {
        const NodeEdges *destEdges = routingGraph->getEdgesFrom(destination);
        if (destEdges && destEdges->edgeCount == 1 && destEdges->edges[0].to == myNode) {
            LOG_INFO("[SR] %s connects only via us: direct", destName);

            routingGraph->updateDownstream(destination, myNode, 1.0f, nowSeconds());
            return destination;
        }
    }

    LOG_INFO("[SR] No route found to %s", destName);
    return 0;
}

NodeNum SignalRoutingModule::findBetterPositionedNeighbor(NodeNum destination, NodeNum sourceNode, NodeNum heardFrom,
                                                          float ourRouteCost, uint32_t currentTime)
{
    if (!routingGraph || !nodeDB) {
        return 0;
    }

    NodeNum myNode = nodeDB->getNodeNum();
    NodeNum bestNeighbor = 0;
    float bestNeighborRouteCost = ourRouteCost;

    const NodeEdges *myEdges = routingGraph->getEdgesFrom(myNode);
    if (!myEdges) {
        return 0;
    }

    for (uint8_t i = 0; i < myEdges->edgeCount; i++) {
        NodeNum neighbor = myEdges->edges[i].to;

        if (neighbor == sourceNode || neighbor == heardFrom || !myEdges->edges[i].hearsUs || !isNodeRoutable(neighbor)) {
            continue;
        }

        if (heardFrom != 0) {
            bool connectivityUnknown = false;
            bool canHearTransmitter = hasVerifiedConnectivity(heardFrom, neighbor, &connectivityUnknown);
            if (!canHearTransmitter) {
                continue;
            }
        }

        const NodeEdges *neighborEdges = routingGraph->getEdgesFrom(neighbor);
        if (neighborEdges) {
            for (uint8_t j = 0; j < neighborEdges->edgeCount; j++) {
                if (neighborEdges->edges[j].to == destination) {
                    float directEtx =
                        routingGraph->deliveryHopCost(neighbor, destination, publishesTopology(destination), currentTime);
                    directEtx += myEdges->edges[i].getEtx();

                    if (routingGraph->canDeliver(neighbor, destination, routePolicy()) && directEtx < ourRouteCost - 1.0f &&
                        directEtx < bestNeighborRouteCost) {
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
        LOG_INFO("[SR] Better neighbor %s for %s (cost %.2f, its ETX %.2f)", nhName, destName, ourRouteCost,
                 bestNeighborRouteCost);
    }

    return bestNeighbor;
}

const DirectNeighborSignal *SignalRoutingModule::lookupDirectSignal(NodeNum nodeId) const
{
    return lookupDirectNeighborSignal(directSignals, directSignalCount, nodeId);
}

void SignalRoutingModule::removeDirectSignal(NodeNum nodeId)
{
    removeDirectNeighborSignal(directSignals, directSignalCount, nodeId);
}

void SignalRoutingModule::pruneDirectSignals(uint32_t nowSecs)
{
    pruneDirectNeighborSignals(directSignals, directSignalCount, nowSecs, cfgNodeTtlSecs);
}

void SignalRoutingModule::syncDirectSignalsWithGraph()
{
    if (!routingGraph || !nodeDB) {
        return;
    }

    NodeNum myNode = nodeDB->getNodeNum();
    const NodeEdges *nodeEdges = routingGraph->getEdgesFrom(myNode);

    for (uint8_t i = 0; i < directSignalCount;) {
        NodeNum nodeId = directSignals[i].nodeId;
        bool hasReportedEdge = false;
        if (nodeEdges) {
            for (uint8_t e = 0; e < nodeEdges->edgeCount; e++) {
                const Edge &edge = nodeEdges->edges[e];
                if (edge.to == nodeId && edge.source == Edge::Source::Reported) {
                    hasReportedEdge = true;
                    break;
                }
            }
        }

        if (!hasReportedEdge) {
            removeDirectSignal(nodeId);
        } else {
            i++;
        }
    }
}

void SignalRoutingModule::updateNeighborInfo(NodeNum nodeId, int32_t rssi, float snr)
{
    if (!routingGraph || !nodeDB)
        return;

    uint32_t monotonicTimestamp = nowSeconds();

    int changeType = refreshReportedDirectNeighbor(nodeId, rssi, snr, monotonicTimestamp);

    if (changeType != EDGE_NO_CHANGE) {
        char neighborName[64];
        getNodeDisplayName(nodeId, neighborName, sizeof(neighborName));

        if (changeType == EDGE_NEW) {
            routingGraph->clearDownstreamForDestination(nodeId);

            LOG_INFO("[SR] Topology changed: new neighbor %s (total nodes: %u)", neighborName,
                     static_cast<unsigned int>(routingGraph->getNodeCount()));
            markTopologyDirty();
        } else if (changeType == EDGE_SIGNIFICANT_CHANGE) {
            LOG_INFO("[SR] Topology changed: ETX change for %s (total nodes: %u)", neighborName,
                     static_cast<unsigned int>(routingGraph->getNodeCount()));
            markTopologyDirty();
        }
    }
}

void SignalRoutingModule::updateNodeActivityForPacketAndRelay(const meshtastic_MeshPacket *p)
{
    if (!p || !routingGraph || !nodeDB || p->via_mqtt)
        return;

    uint32_t currentTime = nowSeconds();
    NodeNum ourNodeId = nodeDB->getNodeNum();

    routingGraph->updateNodeActivity(p->from, currentTime);

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

    // NodeInfo has no SR capability evidence and must not override a proved topology capability.
    CapabilityStatus currentStatus = getCapabilityStatus(mp.from);
    if (currentStatus != CapabilityStatus::SRactive && currentStatus != CapabilityStatus::Passive) {
        CapabilityStatus status = capabilityFromRole(user.role);
        if (status != CapabilityStatus::Unknown) {
            trackNodeCapability(mp.from, status);
        }
    }

    if (user.has_is_unmessagable && user.is_unmessagable) {
        // Refusing user messages does not imply inability to relay.
        CapabilityStatus current = getCapabilityStatus(mp.from);
        if (current != CapabilityStatus::SRactive && current != CapabilityStatus::Passive) {
            trackNodeCapability(mp.from, CapabilityStatus::Legacy);
        }
    }
}

void SignalRoutingModule::handleSniffedPayload(const meshtastic_MeshPacket &mp)
{
    switch (mp.decoded.portnum) {
    case meshtastic_PortNum_NODEINFO_APP:
        handleNodeInfoPacket(mp);
        break;
    case meshtastic_PortNum_ROUTING_APP:
        handleRoutingControlPacket(mp);
        break;
    default:
        break;
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

    bool isDirectRoutingPacket = isDirectPacket(mp);

    switch (routing.which_variant) {
    case meshtastic_Routing_route_request_tag:
        LOG_INFO("[SR] Routing request from %s with %u hops recorded", senderName, routing.route_request.route_count);

        if (isDirectRoutingPacket) {
            for (size_t i = 0; i < routing.route_request.route_count; i++) {
                NodeNum hopNode = routing.route_request.route[i];
                uint8_t hopLastByte = hopNode & 0xFF;
                NodeNum placeholderId = getPlaceholderForRelay(hopLastByte);
                if (isPlaceholderNode(placeholderId) && isPlaceholderConnectedToUs(placeholderId)) {
                    bool isDirectNeighbor = false;
                    NodeNum ourNode = nodeDB->getNodeNum();
                    const NodeEdges *ourEdges = routingGraph->getEdgesFrom(ourNode);
                    if (ourEdges) {
                        for (uint8_t j = 0; j < ourEdges->edgeCount; j++) {
                            if (ourEdges->edges[j].to == hopNode && ourEdges->edges[j].source == Edge::Source::Reported) {
                                isDirectNeighbor = true;
                                break;
                            }
                        }
                    }
                    if (isDirectNeighbor) {
                        LOG_INFO("[SR] Placeholder %08x -> %08x (direct in route_request)", placeholderId, hopNode);
                        resolvePlaceholder(placeholderId, hopNode);
                    } else {
                        LOG_INFO("[SR] Traceroute skipped: %08x not direct", hopNode);
                    }
                }
            }
        } else {
            LOG_INFO("[SR] Placeholder skipped: route_request relayed");
        }
        break;
    case meshtastic_Routing_route_reply_tag:
        LOG_INFO("[SR] Routing reply from %s for %u hops", senderName, routing.route_reply.route_back_count);

        if (isDirectRoutingPacket) {
            for (size_t i = 0; i < routing.route_reply.route_back_count; i++) {
                NodeNum hopNode = routing.route_reply.route_back[i];
                uint8_t hopLastByte = hopNode & 0xFF;
                NodeNum placeholderId = getPlaceholderForRelay(hopLastByte);
                if (isPlaceholderNode(placeholderId) && isPlaceholderConnectedToUs(placeholderId)) {
                    bool isDirectNeighbor = false;
                    NodeNum ourNode = nodeDB->getNodeNum();
                    const NodeEdges *ourEdges = routingGraph->getEdgesFrom(ourNode);
                    if (ourEdges) {
                        for (uint8_t j = 0; j < ourEdges->edgeCount; j++) {
                            if (ourEdges->edges[j].to == hopNode && ourEdges->edges[j].source == Edge::Source::Reported) {
                                isDirectNeighbor = true;
                                break;
                            }
                        }
                    }
                    if (isDirectNeighbor) {
                        LOG_INFO("[SR] Placeholder %08x -> %08x (direct in route_reply)", placeholderId, hopNode);
                        resolvePlaceholder(placeholderId, hopNode);
                    } else {
                        LOG_INFO("[SR] Traceroute skipped: %08x not direct", hopNode);
                    }
                }
            }
        } else {
            LOG_INFO("[SR] Placeholder skipped: route_reply relayed");
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

bool SignalRoutingModule::topologySenderHorizonOk(NodeNum sender, bool heardDirectFromSender,
                                                  const PackedNeighborEntry *neighbors, uint8_t neighborCount) const
{
    if (!routingGraph) {
        return false;
    }
    NodeNum listed[MAX_SIGNAL_ROUTING_NEIGHBORS];
    uint8_t n = neighborCount > MAX_SIGNAL_ROUTING_NEIGHBORS ? MAX_SIGNAL_ROUTING_NEIGHBORS : neighborCount;
    for (uint8_t i = 0; i < n; i++) {
        listed[i] = neighbors ? neighbors[i].nodeId : 0;
    }
    return routingGraph->senderHorizonOk(sender, hasReportedDirectEdge(sender), heardDirectFromSender, listed, n);
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
    switch (config.device.role) {
    case meshtastic_Config_DeviceConfig_Role_ROUTER:
    case meshtastic_Config_DeviceConfig_Role_ROUTER_LATE:
    case meshtastic_Config_DeviceConfig_Role_ROUTER_CLIENT:
    case meshtastic_Config_DeviceConfig_Role_REPEATER:
    case meshtastic_Config_DeviceConfig_Role_CLIENT:
    case meshtastic_Config_DeviceConfig_Role_CLIENT_BASE:

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

SignalRoutingModule::CapabilityStatus SignalRoutingModule::capabilityFromRole(meshtastic_Config_DeviceConfig_Role role) const
{
    // Stock firmware uses the same roles, so a role cannot prove SR capability.
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

    uint32_t now = nowSeconds();

    for (uint8_t i = 0; i < capabilityRecordCount; i++) {
        if (capabilityRecords[i].nodeId == nodeId) {
            // Ordinary traffic cannot prove that a peer still runs SR firmware.
            if (status == CapabilityStatus::Unknown) {
                return;
            }
            capabilityRecords[i].record.lastUpdated = now;
            if (status == CapabilityStatus::SRactive || status == CapabilityStatus::Passive) {
                capabilityRecords[i].record.status = status;
            } else if (status == CapabilityStatus::Legacy) {
                capabilityRecords[i].record.status = CapabilityStatus::Legacy;
            }
            return;
        }
    }

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

    for (uint8_t i = 0; i < capabilityRecordCount;) {
        if (capabilityRecords[i].nodeId == myNode) {
            i++;
            continue;
        }

        if ((nowSecs - capabilityRecords[i].record.lastUpdated) > CAPABILITY_TTL_SECS) {
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

SignalRoutingModule::CapabilityStatus SignalRoutingModule::getCapabilityStatus(NodeNum nodeId) const
{
    uint32_t now = nowSeconds();

    if (nodeDB && nodeId == nodeDB->getNodeNum()) {
        if (!signalBasedRoutingEnabled) {
            return CapabilityStatus::Legacy;
        }

        if (isActiveRoutingRole()) {
            return CapabilityStatus::SRactive;
        } else if (canSendTopology()) {
            return CapabilityStatus::Passive;
        } else {
            return CapabilityStatus::Legacy;
        }
    }

    NodeNum myNode = nodeDB ? nodeDB->getNodeNum() : 0;

    for (uint8_t i = 0; i < capabilityRecordCount; i++) {
        if (capabilityRecords[i].nodeId == nodeId) {
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

// Stock ROUTER_LATE intentionally delays, so reserving an early position for it would slow SR peers.
bool SignalRoutingModule::isImmediateRelayRouter(NodeNum nodeId) const
{
    if (!nodeDB)
        return false;
    if (publishesTopology(nodeId)) {
        return false;
    }
    const meshtastic_NodeInfoLite *node = nodeDB->getMeshNode(nodeId);
    if (!node || !node->has_user)
        return false;
    auto role = node->user.role;
    return role == meshtastic_Config_DeviceConfig_Role_ROUTER || role == meshtastic_Config_DeviceConfig_Role_REPEATER ||
           role == meshtastic_Config_DeviceConfig_Role_ROUTER_CLIENT;
}

bool SignalRoutingModule::willNotCancelForUs(NodeNum nodeId) const
{
    if (!nodeDB)
        return false;

    if (getCapabilityStatus(nodeId) == CapabilityStatus::SRactive)
        return false;
    const meshtastic_NodeInfoLite *node = nodeDB->getMeshNode(nodeId);
    if (!node || !node->has_user)
        return false;
    auto role = node->user.role;
    return role == meshtastic_Config_DeviceConfig_Role_ROUTER || role == meshtastic_Config_DeviceConfig_Role_ROUTER_LATE;
}

bool SignalRoutingModule::planAcknowledgement(const meshtastic_MeshPacket *p, NodeNum sourceNode, NodeNum heardFrom,
                                              uint32_t halfAirtime, uint32_t *delayMsOut, uint8_t *slotsAheadOut)
{
    if (!p || !routingGraph || !nodeDB)
        return false;
    NodeNum myNode = nodeDB->getNodeNum();
    if (!isActiveRoutingRole())
        return false;
    const NodeEdges *myEdges = routingGraph->getEdgesFrom(myNode);
    if (!myEdges)
        return false;

    NeighborGraph::CoveragePolicy policy = coveragePolicy();

    // A noncanceling stock relay already supplies the ACK; an extra SR copy would only add contention.
    for (uint8_t i = 0; i < myEdges->edgeCount; i++) {
        NodeNum neighbor = myEdges->edges[i].to;
        if (neighbor == 0 || neighbor == sourceNode)
            continue;
        if (!willNotCancelForUs(neighbor))
            continue;
        const NodeEdges *theirs = routingGraph->getEdgesFrom(neighbor);
        if (!theirs)
            continue;
        for (uint8_t j = 0; j < theirs->edgeCount; j++) {
            if (theirs->edges[j].to == heardFrom)
                return false;
        }
    }

    // An acknowledgement we cannot hear will not cancel our own rung.
    struct Entry {
        NodeNum node;
        uint16_t cost;
    };
    Entry entries[NODE_SET_MAX];
    uint8_t count = 0;
    uint16_t cost = 0;
    if (routingGraph->acknowledgementPrice(myNode, sourceNode, policy, &cost) && count < NODE_SET_MAX) {
        entries[count++] = {myNode, cost};
    }
    for (uint8_t i = 0; i < myEdges->edgeCount && count < NODE_SET_MAX; i++) {
        NodeNum neighbor = myEdges->edges[i].to;
        if (neighbor == 0 || neighbor == sourceNode || neighbor == myNode)
            continue;
        bool usable = getCapabilityStatus(neighbor) == CapabilityStatus::SRactive || isImmediateRelayRouter(neighbor);
        if (!usable)
            continue;
        if (routingGraph->acknowledgementPrice(neighbor, sourceNode, policy, &cost)) {
            entries[count++] = {neighbor, cost};
        }
    }
    if (count == 0)
        return false;

    // A parity tie-break rotates acknowledgement work without destabilizing cost ordering.
    bool preferHighNodeId = (p->id & 1) != 0;
    for (uint8_t i = 1; i < count; i++) {
        Entry key = entries[i];
        int j = (int)i - 1;
        while (j >= 0) {
            uint16_t a = key.cost / SR_OWNER_COST_BUCKET;
            uint16_t b = entries[j].cost / SR_OWNER_COST_BUCKET;
            bool better = a != b ? a < b : (preferHighNodeId ? key.node > entries[j].node : key.node < entries[j].node);
            if (!better)
                break;
            entries[j + 1] = entries[j];
            j--;
        }
        entries[j + 1] = key;
    }

    uint32_t delay = ladderTransitionMs();
    uint8_t ahead = 0;
    for (uint8_t i = 0; i < count; i++) {
        if (entries[i].node == myNode) {
            if (delayMsOut)
                *delayMsOut = delay;
            if (slotsAheadOut)
                *slotsAheadOut = ahead;
            LOG_INFO("[SR] Acknowledging 0x%08x for %08x at %ums (%u ahead of us)", p->id, sourceNode, delay, (unsigned)ahead);
            return true;
        }

        ahead++;
        delay += halfAirtime;
    }
    return false;
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

void SignalRoutingModule::markStockNodeRelayedOurPacket(NodeNum stockNode)
{
    if (!routingGraph || !nodeDB || stockNode == 0)
        return;

    NodeNum myNode = nodeDB->getNodeNum();
    NodeEdges *myEdges = const_cast<NodeEdges *>(routingGraph->getEdgesFrom(myNode));
    if (!myEdges)
        return;

    for (uint8_t i = 0; i < myEdges->edgeCount; i++) {
        if (myEdges->edges[i].to == stockNode) {
            if (!myEdges->edges[i].hearsUs) {
                myEdges->edges[i].hearsUs = true;
                char nodeName[64];
                getNodeDisplayName(stockNode, nodeName, sizeof(nodeName));
                LOG_INFO("[SR] Stock %s relayed ours: link confirmed both ways", nodeName);
            }
            return;
        }
    }
}

bool SignalRoutingModule::isNodeRoutable(NodeNum nodeId) const
{
    if (config.device.role == meshtastic_Config_DeviceConfig_Role_CLIENT_MUTE && nodeId == nodeDB->getNodeNum()) {
        return false;
    }

    CapabilityStatus status = getCapabilityStatus(nodeId);
    // A passive publisher can be a destination but cannot deliver another node's traffic.
    if (status == CapabilityStatus::Passive) {
        return false;
    }
    if (status == CapabilityStatus::Legacy) {
        return isLegacyRouter(nodeId);
    }

    return true;
}

bool SignalRoutingModule::topologyHealthyForBroadcast() const
{
    if (!routingGraph || !nodeDB) {
        LOG_WARN("[SR] routingGraph or nodeDB is null, returning false");
        return false;
    }

    const NodeEdges *nodeEdges = routingGraph->getEdgesFrom(nodeDB->getNodeNum());
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
        } else if (status == CapabilityStatus::Legacy && isLegacyRouter(to)) {
            capableNeighbors++;
        }
    }

    return capableNeighbors >= 1;
}

bool SignalRoutingModule::topologyHealthyForUnicast(NodeNum destination) const
{
    if (!routingGraph || !nodeDB) {
        return false;
    }

    NodeNum myNode = nodeDB->getNodeNum();
    if (myNode == 0) {
        return false;
    }

    Route route = routingGraph->calculateRoute(destination, nowSeconds(), routePolicy());

    if (route.nextHop != 0) {
        LOG_INFO("[SR] Node %08x reachable via topology (nextHop=%08x, cost=%.2f)", destination, route.nextHop, route.getCost());
        return true;
    }

    NodeNum relay = routingGraph->getDownstreamRelay(destination);
    if (relay != 0) {
        Route relayRoute = routingGraph->calculateRoute(relay, nowSeconds(), routePolicy());
        if (relayRoute.nextHop != 0) {
            LOG_INFO("[SR] Node %08x via relay %08x (nextHop=%08x, cost=%.2f)", destination, relay, relayRoute.nextHop,
                     relayRoute.getCost());
            return true;
        }
    }

    return false;
}

void SignalRoutingModule::rememberRelayIdentity(NodeNum nodeId, uint8_t relayId)
{
    if (relayId == 0 || nodeId == 0 || isPlaceholderNode(nodeId) || (nodeId & 0xFF) != relayId) {
        return;
    }

    uint32_t nowMs = millis();

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
    if (!bucket)
        return;

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

bool SignalRoutingModule::rateLimitNodeInGraph(NodeNum nodeId) const
{
    if (!routingGraph || nodeId == 0 || isPlaceholderNode(nodeId)) {
        return false;
    }
    if (routingGraph->getEdgesFrom(nodeId) != nullptr) {
        return true;
    }
    if (nodeDB) {
        const NodeEdges *myEdges = routingGraph->getEdgesFrom(nodeDB->getNodeNum());
        if (myEdges) {
            for (uint8_t i = 0; i < myEdges->edgeCount; i++) {
                if (myEdges->edges[i].to == nodeId) {
                    return true;
                }
            }
        }
    }
    return false;
}

uint8_t SignalRoutingModule::rateLimitGraphHops(NodeNum nodeId) const
{
    if (!routingGraph || nodeId == 0 || isPlaceholderNode(nodeId)) {
        return 0;
    }
    if (nodeDB && nodeId == nodeDB->getNodeNum()) {
        return 0;
    }
    if (nodeDB) {
        const NodeEdges *myEdges = routingGraph->getEdgesFrom(nodeDB->getNodeNum());
        if (myEdges) {
            for (uint8_t i = 0; i < myEdges->edgeCount; i++) {
                if (myEdges->edges[i].to == nodeId) {
                    return 1;
                }
            }
        }
    }
    Route route = routingGraph->calculateRoute(nodeId, nowSeconds(), routePolicy());
    if (route.nextHop != 0 && route.hops > 0) {
        return route.hops;
    }
    if (rateLimitNodeInGraph(nodeId)) {
        return 255;
    }
    return 0;
}

NodeNum SignalRoutingModule::resolveRelayIdentity(uint8_t relayId, int16_t rxRssi, float rxSnr) const
{
    uint32_t nowMs = millis();
    NodeNum bestNode = 0;
    uint8_t nonDirectCount = 0;

    struct DirectCandidate {
        NodeNum nodeId;
        uint16_t edgeEtx;
    };
    DirectCandidate directCandidates[4];
    uint8_t directCount = 0;

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
                NodeNum n = bucket->entries[i].nodeId;
                int32_t edgeEtx = getDirectEdgeEtx(n);
                if (edgeEtx >= 0 && directCount < 4) {
                    directCandidates[directCount++] = {n, static_cast<uint16_t>(edgeEtx)};
                } else if (edgeEtx < 0) {
                    nonDirectCount++;
                    bestNode = n;
                }
            }
            break;
        }
    }

    if (myEdges) {
        for (uint8_t i = 0; i < myEdges->edgeCount; i++) {
            const Edge &edge = myEdges->edges[i];
            if ((edge.to & 0xFF) != relayId || isPlaceholderNode(edge.to) || edge.source != Edge::Source::Reported) {
                continue;
            }
            bool present = false;
            for (uint8_t j = 0; j < directCount; j++) {
                present |= directCandidates[j].nodeId == edge.to;
            }
            if (!present) {
                if (directCount == 4) {
                    return 0;
                }
                directCandidates[directCount++] = {edge.to, edge.etxFixed};
            }
        }
    }

    NodeNum bestDirectNode = 0;
    if (relayId == 0) {
        return 0;
    }
    if (directCount == 1) {
        bestDirectNode = directCandidates[0].nodeId;
    } else if (directCount > 1 && (rxRssi != 0 || rxSnr > 0.0f)) {
        int32_t normalizedRssi = rxRssi;
        if (!NeighborGraph::normalizeRxSignal(normalizedRssi, rxSnr)) {
            return 0;
        }
        float packetEtx = NeighborGraph::calculateETX(normalizedRssi, rxSnr, currentCostingSpreadingFactor());
        uint16_t packetEtxFixed = static_cast<uint16_t>(packetEtx * 100.0f);
        uint16_t bestDiff = UINT16_MAX;
        bool tied = false;
        for (uint8_t i = 0; i < directCount; i++) {
            uint16_t diff = (packetEtxFixed > directCandidates[i].edgeEtx) ? (packetEtxFixed - directCandidates[i].edgeEtx)
                                                                           : (directCandidates[i].edgeEtx - packetEtxFixed);
            if (diff < bestDiff) {
                bestDiff = diff;
                bestDirectNode = directCandidates[i].nodeId;
                tied = false;
            } else if (diff == bestDiff && directCandidates[i].nodeId != bestDirectNode) {
                tied = true;
            }
        }
        if (tied) {
            bestDirectNode = 0;
        }
    }

    NodeNum result = directCount > 0 ? bestDirectNode : (nonDirectCount == 1 ? bestNode : 0);

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

    // Guessing a colliding relay identity invents measured links and credits nonexistent coverage.
    NodeNum placeholderMatch = 0;
    if (routingGraph && nodeDB) {
        const NodeEdges *myEdges = routingGraph->getEdgesFrom(nodeDB->getNodeNum());
        if (myEdges) {
            NodeNum onlyMatch = 0;
            bool ambiguous = false;
            for (uint8_t i = 0; i < myEdges->edgeCount; i++) {
                NodeNum candidate = myEdges->edges[i].to;
                if ((candidate & 0xFF) != p->relay_node) {
                    continue;
                }
                if (isPlaceholderNode(candidate)) {
                    placeholderMatch = candidate;
                    continue;
                }
                if (onlyMatch != 0 && onlyMatch != candidate) {
                    ambiguous = true;
                    break;
                }
                onlyMatch = candidate;
            }
            if (!ambiguous && onlyMatch != 0) {
                const_cast<SignalRoutingModule *>(this)->rememberRelayIdentity(onlyMatch, p->relay_node);
                return onlyMatch;
            }
        }
    }

    if (placeholderMatch != 0) {
        return placeholderMatch;
    }

    // An unresolved relay byte must not falsely turn a forwarded frame into direct-originator evidence.
    return 0xFF000000 | p->relay_node;
}

bool SignalRoutingModule::hasDirectConnectivity(NodeNum nodeA, NodeNum nodeB)
{
    if (!routingGraph || !nodeDB) {
        return false;
    }

    const NodeEdges *edges = routingGraph->getEdgesFrom(nodeA);
    if (edges) {
        for (uint8_t i = 0; i < edges->edgeCount; i++) {
            if (edges->edges[i].to == nodeB) {
                return true;
            }
        }
    }

    return false;
}

bool SignalRoutingModule::hasVerifiedConnectivity(NodeNum transmitter, NodeNum receiver, bool *unknownOut)
{
    if (!routingGraph || !nodeDB) {
        if (unknownOut)
            *unknownOut = true;
        return false;
    }

    CapabilityStatus txStatus = getCapabilityStatus(transmitter);
    CapabilityStatus rxStatus = getCapabilityStatus(receiver);

    bool txIsStock =
        isPlaceholderNode(transmitter) || txStatus == CapabilityStatus::Legacy || txStatus == CapabilityStatus::Unknown;
    bool rxIsStock = isPlaceholderNode(receiver) || rxStatus == CapabilityStatus::Legacy || rxStatus == CapabilityStatus::Unknown;

    if (txIsStock && rxIsStock) {
        if (unknownOut)
            *unknownOut = true;
        return false;
    }

    bool foundEdge = false;

    if (!txIsStock) {
        if (hasDirectConnectivity(transmitter, receiver)) {
            foundEdge = true;
        }
    }

    if (!foundEdge && !rxIsStock) {
        if (hasDirectConnectivity(receiver, transmitter)) {
            foundEdge = true;
        }
    }

    if (foundEdge) {
        if (unknownOut)
            *unknownOut = false;
        return true;
    }

    if (txIsStock || rxIsStock) {
        if (unknownOut)
            *unknownOut = true;
    } else {
        if (unknownOut)
            *unknownOut = false;
    }

    return false;
}

#endif
