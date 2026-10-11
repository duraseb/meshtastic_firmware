#pragma once
#include "ProtobufModule.h"
#include "concurrency/OSThread.h"
#include "mesh/generated/meshtastic/mesh.pb.h"

#include "MeshRadio.h"
#include "graph/HopHealth.h"
#include "graph/NeighborGraph.h"

#define SIGNAL_ROUTING_VERSION 3

static constexpr uint8_t PACKED_NEIGHBOR_FORMAT_VERSION = 1;
static constexpr uint8_t PACKED_NEIGHBOR_ENTRY_SIZE = 8;
static constexpr uint8_t PACKED_NEIGHBOR_HEADER_SIZE = 5;
static constexpr uint8_t PACKED_NEIGHBOR_FLAG_SR_ACTIVE = 0x01;
static constexpr uint8_t PACKED_NEIGHBOR_FLAG_HEARS_US = 0x02;
static constexpr uint8_t PACKED_HEADER_FLAG_SR_ACTIVE = 0x01;
static constexpr uint8_t PACKED_HEADER_FLAG_MORE_CHUNKS = 0x02;
static constexpr uint8_t PACKED_HEADER_FLAG_CONTINUATION = 0x04;

#define MAX_SIGNAL_ROUTING_NEIGHBORS 28

static inline void writePackedTopologyHeader(uint8_t *buf, uint8_t topologyVersion, bool signalRoutingActive,
                                             bool moreChunks = false, bool continuation = false, uint8_t chunkIndex = 0)
{
    buf[0] = PACKED_NEIGHBOR_FORMAT_VERSION;
    buf[1] = PACKED_NEIGHBOR_ENTRY_SIZE;
    buf[2] = SIGNAL_ROUTING_VERSION;
    buf[3] = topologyVersion;
    // Reserved v1 bits let new receivers detect lost chunks while old receivers retain compatibility.
    uint8_t flags = static_cast<uint8_t>(chunkIndex << 3);
    if (signalRoutingActive)
        flags |= PACKED_HEADER_FLAG_SR_ACTIVE;
    if (moreChunks)
        flags |= PACKED_HEADER_FLAG_MORE_CHUNKS;
    if (continuation)
        flags |= PACKED_HEADER_FLAG_CONTINUATION;
    buf[4] = flags;
}

inline bool srObserveModemPreset(bool &known, uint8_t &cached, uint8_t current)
{
    if (!known) {
        known = true;
        cached = current;
        return false;
    }
    if (cached == current) {
        return false;
    }
    cached = current;
    return true;
}

struct PackedNeighborEntry {
    NodeNum nodeId;
    int8_t rssi;
    int8_t snr;
    bool signalRoutingActive;
    bool hearsUs;
    uint8_t etxVariance;
};

struct PackedHeader {
    uint8_t formatVersion;
    uint8_t entrySize;
    uint8_t routingVersion;
    uint8_t topologyVersion;
    bool signalRoutingActive;
    bool moreChunks;
    uint8_t chunkIndex;
    bool continuation;

    bool isCompleteList() const { return !moreChunks && !continuation; }
};

static inline uint8_t decodePackedNeighbors(const uint8_t *data, size_t dataLen, PackedNeighborEntry *outEntries,
                                            uint8_t maxEntries, PackedHeader *header = nullptr)
{
    if (header) {
        *header = {};
    }
    if (!data || dataLen < PACKED_NEIGHBOR_HEADER_SIZE || data[0] != PACKED_NEIGHBOR_FORMAT_VERSION ||
        data[1] < PACKED_NEIGHBOR_ENTRY_SIZE) {
        return 0;
    }
    const size_t payloadLen = dataLen - PACKED_NEIGHBOR_HEADER_SIZE;
    const uint8_t entrySize = data[1];
    const size_t count = payloadLen / entrySize;
    if (payloadLen % entrySize != 0 || count > maxEntries || (count > 0 && !outEntries) ||
        ((data[4] & PACKED_HEADER_FLAG_CONTINUATION) == 0 && (data[4] >> 3) != 0)) {
        return 0;
    }
    if (header) {
        header->formatVersion = data[0];
        header->entrySize = entrySize;
        header->routingVersion = data[2];
        header->topologyVersion = data[3];
        header->signalRoutingActive = (data[4] & PACKED_HEADER_FLAG_SR_ACTIVE) != 0;
        header->moreChunks = (data[4] & PACKED_HEADER_FLAG_MORE_CHUNKS) != 0;
        header->continuation = (data[4] & PACKED_HEADER_FLAG_CONTINUATION) != 0;
        header->chunkIndex = data[4] >> 3;
    }
    const uint8_t entryCount = static_cast<uint8_t>(count);

    for (uint8_t i = 0; i < entryCount; i++) {
        const uint8_t *e = &data[PACKED_NEIGHBOR_HEADER_SIZE + i * entrySize];
        PackedNeighborEntry &out = outEntries[i];
        out.nodeId = (uint32_t)e[0] | ((uint32_t)e[1] << 8) | ((uint32_t)e[2] << 16) | ((uint32_t)e[3] << 24);
        out.rssi = static_cast<int8_t>(e[4]);
        out.snr = static_cast<int8_t>(e[5]);
        out.signalRoutingActive = (e[6] & PACKED_NEIGHBOR_FLAG_SR_ACTIVE) != 0;
        out.hearsUs = (e[6] & PACKED_NEIGHBOR_FLAG_HEARS_US) != 0;
        out.etxVariance = e[7];
    }
    return entryCount;
}

static inline void encodePackedNeighborEntry(uint8_t *entry, NodeNum nodeId, int8_t rssi, int8_t snr, bool signalRoutingActive,
                                             bool hearsUs, uint8_t etxVariance)
{
    entry[0] = static_cast<uint8_t>((nodeId >> 0) & 0xFF);
    entry[1] = static_cast<uint8_t>((nodeId >> 8) & 0xFF);
    entry[2] = static_cast<uint8_t>((nodeId >> 16) & 0xFF);
    entry[3] = static_cast<uint8_t>((nodeId >> 24) & 0xFF);
    entry[4] = static_cast<uint8_t>(rssi);
    entry[5] = static_cast<uint8_t>(snr);
    uint8_t flags = 0;
    if (signalRoutingActive) {
        flags |= PACKED_NEIGHBOR_FLAG_SR_ACTIVE;
    }
    if (hearsUs) {
        flags |= PACKED_NEIGHBOR_FLAG_HEARS_US;
    }
    entry[6] = flags;
    entry[7] = etxVariance;
}

struct DirectNeighborSignal {
    NodeNum nodeId = 0;
    int8_t rssi = 0;
    int8_t snr = 0;
    uint32_t lastRx = 0;
};

static inline const DirectNeighborSignal *lookupDirectNeighborSignal(const DirectNeighborSignal *table, uint8_t count,
                                                                     NodeNum nodeId)
{
    for (uint8_t i = 0; i < count; i++) {
        if (table[i].nodeId == nodeId) {
            return &table[i];
        }
    }
    return nullptr;
}

static inline void upsertDirectNeighborSignal(DirectNeighborSignal *table, uint8_t &count, size_t maxEntries, NodeNum nodeId,
                                              int8_t rssi, int8_t snr, uint32_t nowSecs)
{
    for (uint8_t i = 0; i < count; i++) {
        if (table[i].nodeId == nodeId) {
            table[i].rssi = rssi;
            table[i].snr = snr;
            table[i].lastRx = nowSecs;
            return;
        }
    }

    if (count < maxEntries) {
        table[count].nodeId = nodeId;
        table[count].rssi = rssi;
        table[count].snr = snr;
        table[count].lastRx = nowSecs;
        count++;
        return;
    }

    uint8_t oldestIdx = 0;
    uint32_t oldestRx = table[0].lastRx;
    for (uint8_t i = 1; i < count; i++) {
        if (table[i].lastRx < oldestRx) {
            oldestRx = table[i].lastRx;
            oldestIdx = i;
        }
    }
    table[oldestIdx].nodeId = nodeId;
    table[oldestIdx].rssi = rssi;
    table[oldestIdx].snr = snr;
    table[oldestIdx].lastRx = nowSecs;
}

static inline void removeDirectNeighborSignal(DirectNeighborSignal *table, uint8_t &count, NodeNum nodeId)
{
    for (uint8_t i = 0; i < count;) {
        if (table[i].nodeId == nodeId) {
            if (i < count - 1) {
                table[i] = table[count - 1];
            }
            count--;
        } else {
            i++;
        }
    }
}

static inline void pruneDirectNeighborSignals(DirectNeighborSignal *table, uint8_t &count, uint32_t nowSecs, uint32_t ttlSecs)
{
    for (uint8_t i = 0; i < count;) {
        if ((nowSecs - table[i].lastRx) > ttlSecs) {
            if (i < count - 1) {
                table[i] = table[count - 1];
            }
            count--;
        } else {
            i++;
        }
    }
}

static constexpr uint32_t EMPTY_TOPOLOGY_REPLY_DELAY_BASE_MS = 5000;
static constexpr uint32_t EMPTY_TOPOLOGY_REPLY_DELAY_JITTER_MS = 11001;

static inline uint32_t computeEmptyTopologyReplyDelayMs(NodeNum senderNodeId, PacketId packetId, NodeNum ourNodeId)
{
    return EMPTY_TOPOLOGY_REPLY_DELAY_BASE_MS + ((senderNodeId ^ packetId ^ ourNodeId) % EMPTY_TOPOLOGY_REPLY_DELAY_JITTER_MS);
}

static inline bool srTopologyVersionInWindow(uint8_t received, uint8_t last)
{
    return static_cast<uint8_t>(received - last) < 0x80;
}

static constexpr uint8_t SR_TOPOLOGY_DELAYED_BEHIND_MAX = 7;

static inline uint8_t srTopologyVersionBehind(uint8_t received, uint8_t last)
{
    return static_cast<uint8_t>(last - received);
}

static inline bool srTopologyVersionLargeBehind(uint8_t received, uint8_t last)
{
    uint8_t behind = srTopologyVersionBehind(received, last);
    return received != last && behind > SR_TOPOLOGY_DELAYED_BEHIND_MAX && behind < 0x80;
}

static inline bool srTopologyVersionClimbing(uint8_t received, uint8_t staleVersion)
{
    uint8_t step = static_cast<uint8_t>(received - staleVersion);
    return step >= 1 && step <= SR_TOPOLOGY_DELAYED_BEHIND_MAX;
}

enum class SrTopologyVerdict : uint8_t {
    Stale,
    Accept,
    FirstContact,
    BootReset,
    SilenceResync,
    RestartClimb,
    DirectResync,
    CounterReset,
};

// Stock reservations model an unknown hardware-seeded draw; only actual SR transmissions justify airtime spacing.
struct SrPositionAllocator {
    uint32_t windowWidthMs = 0;
    uint32_t slotTimeMs = 0;
    uint32_t halfAirtimeMs = 0;
    uint32_t originMs = 0;
    uint32_t cursorMs = 0;
    uint32_t lastPlacedMs = 0;
    uint32_t ladderStartMs = 0;
    uint8_t placed = 0;
    uint8_t nextIdx = 0;
    bool anyPlaced = false;
    bool ladderFrozen = false;

    SrPositionAllocator(uint32_t slotTime, uint32_t halfAirtime, uint32_t origin, uint8_t positions)
        : windowWidthMs(slotTime * positions), slotTimeMs(slotTime), halfAirtimeMs(halfAirtime), originMs(origin),
          maxPositions(positions)
    {
    }

    // An empty router window must not add a phantom half-airtime at slow presets.
    uint32_t firstRungMs() const
    {
        if (!anyPlaced) {
            return originMs;
        }
        uint32_t cleared = lastPlacedMs + halfAirtimeMs;
        return cleared > originMs ? cleared : originMs;
    }

    uint32_t takeReserved() { return place(slotTimeMs); }

    uint32_t takeRung() { return place(halfAirtimeMs); }

    uint32_t takeLateRung()
    {
        uint32_t at = spill();
        nextIdx++;
        return at;
    }

    uint32_t firstFreeMs() { return anyPlaced ? spill() : originMs; }

  private:
    uint8_t maxPositions = 0;

    uint32_t spill()
    {
        if (!ladderFrozen) {
            ladderStartMs = firstRungMs();
            ladderFrozen = true;
        }
        uint8_t rung = nextIdx > placed ? static_cast<uint8_t>(nextIdx - placed) : 0;
        return ladderStartMs + rung * halfAirtimeMs;
    }

    uint32_t place(uint32_t widthMs)
    {
        if (placed >= maxPositions || cursorMs >= windowWidthMs) {
            uint32_t at = spill();
            nextIdx++;
            return at;
        }
        uint32_t at = cursorMs;
        cursorMs += widthMs;
        lastPlacedMs = at;
        anyPlaced = true;
        placed++;
        nextIdx++;
        return at;
    }
};

// A fresh random draw would synchronize peers whose scheduled rungs expired together.
static inline uint32_t srExpiredRelayReanchorMs(uint32_t rungDelayMs, uint32_t originMs, uint32_t slotTimeMs)
{
    uint32_t offset = rungDelayMs > originMs ? rungDelayMs - originMs : 0;
    uint32_t floorMs = slotTimeMs > 0 ? slotTimeMs : 1;
    return offset > floorMs ? offset : floorMs;
}

// Shared-origin timing floors compose by max; rung separation must survive the winning floor.
static inline uint32_t srUnicastSlotDelayMs(uint32_t reservedBaseMs, uint8_t slotIndex, uint32_t earliestMs,
                                            uint32_t leaderWaitMs, uint32_t halfAirtimeMs)
{
    if (reservedBaseMs > 0) {
        return reservedBaseMs + (uint32_t)slotIndex * halfAirtimeMs;
    }
    if (slotIndex == 0) {
        return earliestMs;
    }
    uint32_t base = earliestMs > leaderWaitMs ? earliestMs : leaderWaitMs;
    return base + ((uint32_t)slotIndex - 1) * halfAirtimeMs;
}

static inline SrTopologyVerdict srTopologyVersionVerdict(uint8_t received, uint8_t last, uint32_t lastAcceptMs, uint32_t nowMs,
                                                         uint32_t resyncMs, bool bootBroadcast, bool staleValid = false,
                                                         uint8_t staleVersion = 0, bool directCompleteList = false)
{
    if (lastAcceptMs == 0)
        return SrTopologyVerdict::FirstContact;
    if (bootBroadcast)
        return SrTopologyVerdict::BootReset;
    if (srTopologyVersionInWindow(received, last))
        return SrTopologyVerdict::Accept;

    if (srTopologyVersionLargeBehind(received, last))
        return SrTopologyVerdict::CounterReset;
    uint32_t silent = nowMs - lastAcceptMs;
    if (silent >= resyncMs && silent < 0x80000000u)
        return SrTopologyVerdict::SilenceResync;

    if (staleValid && srTopologyVersionClimbing(received, staleVersion))
        return SrTopologyVerdict::RestartClimb;

    if (directCompleteList)
        return SrTopologyVerdict::DirectResync;
    return SrTopologyVerdict::Stale;
}

static inline bool hasReportedDirectEdgeTo(const NeighborGraph *graph, NodeNum myNode, NodeNum neighborId)
{
    if (!graph) {
        return false;
    }
    const NodeEdges *nodeEdges = graph->getEdgesFrom(myNode);
    if (!nodeEdges) {
        return false;
    }
    for (uint8_t i = 0; i < nodeEdges->edgeCount; i++) {
        if (nodeEdges->edges[i].to == neighborId && nodeEdges->edges[i].source == Edge::Source::Reported) {
            return true;
        }
    }
    return false;
}

// Passive publishers never relay our packets, so their lists provide the reachability evidence.
static inline bool confirmSenderHearsUs(NeighborGraph *graph, NodeNum myNode, NodeNum sender)
{
    if (!graph || sender == 0 || sender == myNode) {
        return false;
    }
    const NodeEdges *myEdges = graph->getEdgesFrom(myNode);
    if (!myEdges) {
        return false;
    }
    for (uint8_t i = 0; i < myEdges->edgeCount; i++) {
        if (myEdges->edges[i].to != sender) {
            continue;
        }
        if (myEdges->edges[i].hearsUs) {
            return false;
        }
        graph->setEdgeHearsUs(myNode, sender, true);
        return true;
    }
    return false;
}

static inline bool confirmTopologySenderHearsUs(NeighborGraph *graph, NodeNum myNode, NodeNum sender, NodeNum listedNeighbor)
{
    if (listedNeighbor != myNode) {
        return false;
    }
    return confirmSenderHearsUs(graph, myNode, sender);
}

// Peers must agree on a passive neighbor's coverage or both choose the first relay slot.
static inline bool confirmTopologySenderHearsNeighbor(NeighborGraph *graph, NodeNum sender, NodeNum listedNeighbor)
{
    if (!graph || listedNeighbor == sender || listedNeighbor == 0) {
        return false;
    }
    const NodeEdges *edges = graph->getEdgesFrom(listedNeighbor);
    if (!edges) {
        return false;
    }
    for (uint8_t i = 0; i < edges->edgeCount; i++) {
        if (edges->edges[i].to != sender) {
            continue;
        }
        if (edges->edges[i].hearsUs) {
            return false;
        }
        graph->setEdgeHearsUs(listedNeighbor, sender, true);
        return true;
    }
    return false;
}

// A custom modem must be priced against its actual demodulator threshold, not its preset label.
static inline uint8_t currentCostingSpreadingFactor()
{
    if (config.lora.use_preset) {
        float bwKHz = 0.0f;
        uint8_t sf = 0;
        uint8_t cr = 0;
        modemPresetToParams(config.lora.modem_preset, false, bwKHz, sf, cr);
        return sf;
    }
    return static_cast<uint8_t>(config.lora.spread_factor);
}

static inline int refreshReportedDirectNeighborObservation(NeighborGraph *graph, DirectNeighborSignal *signals,
                                                           uint8_t &signalCount, size_t maxSignals, NodeNum myNode,
                                                           NodeNum nodeId, int32_t rssi, float snr, uint32_t nowSecs)
{
    if (!graph) {
        return EDGE_NO_CHANGE;
    }

    float etx = NeighborGraph::calculateETX(rssi, snr, currentCostingSpreadingFactor());
    graph->foldSilenceBeforeReportedHear(myNode, nodeId, nowSecs);
    // The measured direction must exist first or graph admission can reject its inferred reverse.
    int changeType = graph->updateEdge(myNode, nodeId, etx, nowSecs, Edge::Source::Reported);
    graph->stampReportedLastHeard(myNode, nodeId, nowSecs);
    // Symmetry is an assumption; ranking its reverse as measured would block stronger peer evidence.
    (void)graph->updateEdge(nodeId, myNode, etx, nowSecs, Edge::Source::Inferred);

    int8_t clampedSnr = static_cast<int8_t>(std::max(-128.f, std::min(127.f, snr)));
    upsertDirectNeighborSignal(signals, signalCount, maxSignals, nodeId, static_cast<int8_t>(rssi), clampedSnr, nowSecs);
    return changeType;
}

#define SIGNAL_ROUTING_BROADCAST_SECS 900

#define SIGNAL_ROUTING_DIRTY_BROADCAST_SECS 600

#define SR_BROADCAST_MAX_HOPS 4

static constexpr uint8_t SR_LAST_HOP_BUDGET = 1;

class SignalRoutingModule : public ProtobufModule<meshtastic_SignalRoutingInfo>, private concurrency::OSThread
{
  public:
    SignalRoutingModule();
    ~SignalRoutingModule() override;

    SignalRoutingModule(const SignalRoutingModule &) = delete;
    SignalRoutingModule &operator=(const SignalRoutingModule &) = delete;

#if defined(UNIT_TEST)
    uint32_t debugNextRunMs() const { return _cached_next_run; }
#endif

    bool shouldUseSignalBasedRouting(const meshtastic_MeshPacket *p);

    bool isHandOffDuplicate(const meshtastic_MeshPacket *p);

    bool isOriginatorRetryReplan(const meshtastic_MeshPacket *p);

    void observeRelayedPacket(const meshtastic_MeshPacket &mp);

    NodeNum alternateNextHop(NodeNum dest, const NodeNum *excluded, uint8_t n, uint8_t nominatedByte);

    NodeNum nominatedHopFor(NodeNum flagged, uint8_t nextHopByte) const;

    void recordHopMiss(NodeNum destination, NodeNum nextHop);
    void recordHopSuccess(NodeNum destination, NodeNum nextHop);

    NodeNum takePendingUnicastNextHop()
    {
        NodeNum n = pendingUnicastNextHop;
        pendingUnicastNextHop = 0;
        return n;
    }
    UnicastSlotFlags takePendingUnicastFlags()
    {
        UnicastSlotFlags f = pendingUnicastFlags;
        pendingUnicastFlags = {};
        return f;
    }
    UnicastSlotFlags unicastCommitFlags(PacketId packetId) const;
    bool hasPricedDeliveryHop(NodeNum dest) const;

    bool hasStrongHopTo(NodeNum peer) const;

    bool hasStrongDeliveryHop(NodeNum dest) const;
    bool isSignalRoutingNode(NodeNum nodeId) const { return publishesTopology(nodeId); }
    uint32_t nextHopCarryWaitMs(NodeNum nextHop, uint32_t airtimeMs, float rxSnr) const;

    uint32_t namedForwardFollowupDelayMs(NodeNum nextHop, uint32_t airtimeMs, float rxSnr) const;
    void updateNodeActivityForPacketAndRelay(const meshtastic_MeshPacket *p);
    bool shouldRelay(const meshtastic_MeshPacket *p);

    bool shouldRelayBroadcast(const meshtastic_MeshPacket *p, bool unicastFloodCoverage = false);

    NodeNum getNextHop(NodeNum destination, NodeNum sourceNode = 0, NodeNum heardFrom = 0, bool allowOpportunistic = true,
                       bool *verified = nullptr);
    NodeNum findBetterPositionedNeighbor(NodeNum destination, NodeNum sourceNode, NodeNum heardFrom, float ourRouteCost,
                                         uint32_t currentTime);
    bool shouldRelayUnicastForCoordination(const meshtastic_MeshPacket *p);
    bool hasDirectConnectivity(NodeNum nodeA, NodeNum nodeB);
    bool hasVerifiedConnectivity(NodeNum transmitter, NodeNum receiver, bool *unknownOut = nullptr);
    void updateNeighborInfo(NodeNum nodeId, int32_t rssi, float snr);
    void sendSignalRoutingInfo(NodeNum dest = NODENUM_BROADCAST);

    static bool isDirectPacket(const meshtastic_MeshPacket &mp);
    void preProcessSignalRoutingPacket(const meshtastic_MeshPacket *p);

  protected:
    virtual bool handleReceivedProtobuf(const meshtastic_MeshPacket &mp, meshtastic_SignalRoutingInfo *p) override;
    void updateGraphWithNeighbor(NodeNum sender, NodeNum neighborId, int8_t rssi, int8_t snr, bool hearsUs);
    uint8_t packNeighborsForBroadcast(uint8_t *outBuf, size_t bufSize);

    void noteTopologySenderHearsUs(NodeNum sender, NodeNum listedNeighbor);
    virtual ProcessMessage handleReceived(const meshtastic_MeshPacket &mp) override;
    virtual bool wantPacket(const meshtastic_MeshPacket *p) override { return true; }
    virtual meshtastic_MeshPacket *allocReply() override;
    virtual int32_t runOnce() override;

    NeighborGraph *routingGraph = nullptr;

  private:
    uint32_t nowSeconds() const;
    void wakeWithin(uint32_t delayMs);
    void maintainGraph(uint32_t currentTime);
    void processTopology(const meshtastic_MeshPacket &mp, const meshtastic_SignalRoutingInfo &info);
    HopHealth hopHealth;
    uint32_t lastGraphUpdate = 0;
    static constexpr uint32_t GRAPH_MAINTENANCE_INTERVAL_SECS = 60;
    static constexpr uint32_t NODE_TTL_SECS = 10800;
    static constexpr uint32_t CAPABILITY_TTL_SECS = SIGNAL_ROUTING_BROADCAST_SECS * 3 + 20;

    static constexpr uint32_t PUBLISHER_SILENCE_SECS = SIGNAL_ROUTING_BROADCAST_SECS * 2 + 20;
    static constexpr uint32_t RELAY_ID_CACHE_TTL_MS = 600 * 1000;

    bool signalBasedRoutingEnabled = true;

    bool loggedOwnRole = false;
    bool operatingPresetKnown = false;
    uint8_t operatingPreset = 0;
    bool needsBootBroadcast = false;
    bool topologyDirty = false;
    bool topologyBroadcastActive = false;
    uint32_t lastBroadcast = 0;

    int32_t sinceLastBroadcast(uint32_t nowMs) const { return (int32_t)(nowMs - lastBroadcast); }

    struct PendingTopologyReply {
        bool active = false;
        uint32_t fireAfterMs = 0;

        uint32_t requestedMs = 0;
    };
    PendingTopologyReply pendingTopologyReply;
    // A reboot burst must not make every peer broadcast its list for every request.
    static constexpr uint32_t BOOTSTRAP_REPLY_MIN_INTERVAL_MS = 120000;
    // Receivers need turnaround time to hear the next relay after reading the previous frame.
    // Colocated radios need this measured separation to hear and cancel each other.
    static constexpr uint32_t SR_MIN_RUNG_SPACING_MS = 50;

    // Tie-break jitter must stay below rung spacing or adjacent candidates can reverse order.
    static constexpr uint32_t SR_MIN_TIE_BREAK_RANGE_MS = 20;

    static constexpr uint32_t SR_FALLBACK_HALF_AIRTIME_MS = 150;

    static constexpr uint32_t SR_PEER_TURNAROUND_MS = 250;

    static constexpr uint32_t SR_SLOT_ORIGIN_MS = SR_PEER_TURNAROUND_MS;
    uint32_t lastBootstrapReplyMs = 0;

    uint32_t lastTopologyListMs = 0;
    uint8_t currentTopologyVersion = 0;

    static constexpr size_t MAX_TOPOLOGY_VERSION_ENTRIES = NEIGHBOR_GRAPH_MAX_NEIGHBORS;
    struct TopologyVersionEntry {
        NodeNum nodeId = 0;
        uint8_t version = 0;
        PacketId packetId = 0;
        uint32_t processedChunks = 0;
        uint32_t lastAcceptMs = 0;

        uint8_t staleVersion = 0;
        bool staleValid = false;
    };
    TopologyVersionEntry lastTopologyVersion[MAX_TOPOLOGY_VERSION_ENTRIES];
    uint8_t lastTopologyVersionCount = 0;
    TopologyVersionEntry lastPreProcessedVersion[MAX_TOPOLOGY_VERSION_ENTRIES];
    uint8_t lastPreProcessedVersionCount = 0;

    static constexpr uint8_t PENDING_LISTED_SLOTS = 4;
    struct PendingListed {
        NodeNum ids[NEIGHBOR_GRAPH_MAX_EDGES_PER_NODE];
        uint8_t count = 0;
        NodeNum sender = 0;
        uint8_t version = 0;
        uint8_t nextChunk = 1;
        bool valid = false;
    };
    PendingListed pendingListed[PENDING_LISTED_SLOTS];

    uint8_t pendingListedSlot(NodeNum sender) const
    {
        for (uint8_t i = 0; i < PENDING_LISTED_SLOTS; i++)
            if (pendingListed[i].valid && pendingListed[i].sender == sender)
                return i;
        for (uint8_t i = 0; i < PENDING_LISTED_SLOTS; i++)
            if (!pendingListed[i].valid)
                return i;
        return 0;
    }

    static TopologyVersionEntry *findTopologyEntry(TopologyVersionEntry *table, uint8_t count, NodeNum nodeId);

    bool getTopologyStale(const TopologyVersionEntry *table, uint8_t count, NodeNum nodeId, uint8_t *staleVersion) const;
    void noteTopologyStale(TopologyVersionEntry *table, uint8_t count, NodeNum nodeId, uint8_t received);
    uint8_t getTopologyVersion(const TopologyVersionEntry *table, uint8_t count, NodeNum nodeId,
                               uint32_t *lastAcceptMs = nullptr) const;
    void setTopologyVersion(TopologyVersionEntry *table, uint8_t &count, NodeNum nodeId, uint8_t version, uint32_t nowMs = 0,
                            PacketId packetId = 0, uint32_t chunkBit = 0);

    SrTopologyVerdict gateTopologyVersion(NodeNum from, uint8_t receivedVersion, bool bootBroadcast, bool directCompleteList,
                                          uint32_t nowMs);

    uint32_t topologyResyncMs() const { return 2 * cfgBroadcastSecs * 1000 + 20 * 1000; }

    bool topologySenderHorizonOk(NodeNum sender, bool heardDirectFromSender, const PackedNeighborEntry *neighbors,
                                 uint8_t neighborCount) const;
    void sendTopologyPacket(NodeNum dest, const uint8_t *packedData, size_t packedLen, uint32_t txAfterMs = 0);

    DirectNeighborSignal directSignals[NEIGHBOR_GRAPH_MAX_EDGES_PER_NODE];
    uint8_t directSignalCount = 0;

    const DirectNeighborSignal *lookupDirectSignal(NodeNum nodeId) const;
    void removeDirectSignal(NodeNum nodeId);
    void pruneDirectSignals(uint32_t nowSecs);
    void syncDirectSignalsWithGraph();

    enum class CapabilityStatus : uint8_t { Unknown = 0, SRactive, Passive, Legacy };

    struct CapabilityRecord {
        CapabilityStatus status = CapabilityStatus::Unknown;
        uint32_t lastUpdated = 0;
    };

    struct RelayIdentityEntry {
        NodeNum nodeId = 0;
        uint32_t lastHeardMs = 0;
    };

    static constexpr size_t MAX_CAPABILITY_RECORDS = 64;
    static constexpr size_t MAX_RELAY_IDENTITY_ENTRIES = 16;

    struct CapabilityRecordEntry {
        NodeNum nodeId = 0;
        CapabilityRecord record;
    };
    CapabilityRecordEntry capabilityRecords[MAX_CAPABILITY_RECORDS];
    uint8_t capabilityRecordCount = 0;

    struct RelayIdentityCacheEntry {
        uint8_t relayId = 0;
        RelayIdentityEntry entries[4];
        uint8_t entryCount = 0;
    };
    RelayIdentityCacheEntry relayIdentityCache[MAX_RELAY_IDENTITY_ENTRIES];
    uint8_t relayIdentityCacheCount = 0;

    void trackNodeCapability(NodeNum nodeId, CapabilityStatus status);
    void pruneCapabilityCache(uint32_t nowSecs);
    CapabilityStatus getCapabilityStatus(NodeNum nodeId) const;

    bool publishesTopology(NodeNum nodeId) const
    {
        CapabilityStatus s = getCapabilityStatus(nodeId);
        return s == CapabilityStatus::SRactive || s == CapabilityStatus::Passive;
    }

    NeighborGraph::CoveragePolicy coveragePolicy() const;

    NeighborGraph::RoutePolicy routePolicy() const;

    bool topologyHealthyForBroadcast() const;
    bool topologyHealthyForUnicast(NodeNum destination) const;
    bool isImmediateRelayRouter(NodeNum nodeId) const;
    bool isLegacyRouter(NodeNum nodeId) const;
    void pruneRelayIdentityCache(uint32_t nowMs);

  public:
    NodeNum resolveHeardFrom(const meshtastic_MeshPacket *p, NodeNum sourceNode) const;
    void rememberRelayIdentity(NodeNum nodeId, uint8_t relayId);
    NodeNum resolveRelayIdentity(uint8_t relayId, int16_t rxRssi = 0, float rxSnr = 0) const;

    static bool isPlaceholderNodeId(NodeNum nodeId) { return (nodeId & 0xFF000000u) == 0xFF000000u; }
    bool isPlaceholderNode(NodeNum nodeId) const;

    // Dropped traffic must still feed direct-link evidence or the limiter can starve its own relay resolver.
    void observeForGraph(const meshtastic_MeshPacket &mp);

    bool hasEstablishedGraph() const;

    bool rateLimitNodeInGraph(NodeNum nodeId) const;

    uint8_t rateLimitGraphHops(NodeNum nodeId) const;

    void purgeGraphForPresetChange();

    void radioReconfigured();
    uint8_t publishedTopologyVersion() const { return currentTopologyVersion; }
    bool bootBroadcastPending() const { return needsBootBroadcast; }
    uint8_t relayIdentityCacheSize() const { return relayIdentityCacheCount; }

  private:
    bool isActiveRoutingRole() const;
    bool canSendTopology() const;
    void handleNodeInfoPacket(const meshtastic_MeshPacket &mp);
    CapabilityStatus capabilityFromRole(meshtastic_Config_DeviceConfig_Role role) const;
    void handleSniffedPayload(const meshtastic_MeshPacket &mp);
    void handleRoutingControlPacket(const meshtastic_MeshPacket &mp);

    NodeNum createPlaceholderNode(uint8_t relayId);
    bool resolvePlaceholder(NodeNum placeholderId, NodeNum realNodeId);
    NodeNum getPlaceholderForRelay(uint8_t relayId) const;
    void replaceGatewayNode(NodeNum oldNode, NodeNum newNode);
    bool isPlaceholderConnectedToUs(NodeNum placeholderId) const;
    bool hasBetterPositionedSRNeighbor(NodeNum myNode, NodeNum heardFrom, NodeNum destination, uint32_t packetId);
    bool unicastCanFinish(NodeNum node, NodeNum destination) const;
    uint16_t unicastCandidateCost(NodeNum node, NodeNum destination, NodeNum myNextHop) const;
    bool unicastDupeCancels(const meshtastic_MeshPacket *p, NodeNum dupeRelayer);
    bool isNodeRoutable(NodeNum nodeId) const;
    void logNetworkTopology();

    void markTopologyDirty();
    void commitTopologyTxTimestamp();
    void scheduleEmptyTopologyReply(NodeNum senderNodeId, PacketId packetId);
    int refreshReportedDirectNeighbor(NodeNum nodeId, int32_t rssi, float snr, uint32_t nowSecs);
    bool hasReportedDirectEdge(NodeNum neighborId) const;
    void markStockNodeRelayedOurPacket(NodeNum stockNode);

    static constexpr size_t MAX_COMMITTED_RELAYS = 8;
    static constexpr size_t MAX_HEARD_TRANSMITTERS = 6;
    struct CommittedRelay {
        PacketId packetId = 0;
        NodeNum originalHeardFrom = 0;
        uint32_t txDelayMs = 0;
        NodeNum heardTransmitters[MAX_HEARD_TRANSMITTERS];
        uint8_t heardTransmitterCount = 0;
        UnicastSlotFlags unicastFlags;
        CommittedRelay() : packetId(0), originalHeardFrom(0), txDelayMs(0), heardTransmitterCount(0), unicastFlags()
        {
            memset(heardTransmitters, 0, sizeof(heardTransmitters));
        }
    };
    CommittedRelay committedRelays[MAX_COMMITTED_RELAYS];
    uint8_t committedRelayCount = 0;

    static constexpr size_t MAX_PENDING_RETRANSMITS = 4;
    struct PendingRetransmit {
        PacketId packetId = 0;
        meshtastic_MeshPacket *packet = nullptr;
        uint32_t fireAfterMs = 0;
        bool canceled = false;
        PendingRetransmit() : packetId(0), packet(nullptr), fireAfterMs(0), canceled(false) {}
    };
    PendingRetransmit pendingRetransmits[MAX_PENDING_RETRANSMITS];
    bool isRetransmitting = false;

    uint32_t cfgBroadcastSecs = SIGNAL_ROUTING_BROADCAST_SECS;
    uint32_t cfgDirtyBroadcastSecs = SIGNAL_ROUTING_DIRTY_BROADCAST_SECS;
    uint32_t cfgNodeTtlSecs = NODE_TTL_SECS;
    uint32_t cfgBroadcastMaxHops = SR_BROADCAST_MAX_HOPS;
    float cfgPoorLinkEtxThreshold = 7.0f;
    bool t1RetransmitEnabled = true;

    bool hasAnyHearsUsNeighbor() const;

    // CLIENT_BASE cancellation depends on local favorites that peers cannot know.
    bool willNotCancelForUs(NodeNum node) const;

    bool planAcknowledgement(const meshtastic_MeshPacket *p, NodeNum sourceNode, NodeNum heardFrom, uint32_t halfAirtime,
                             uint32_t *delayMsOut, uint8_t *slotsAheadOut);

  public:
    uint32_t pendingRelayDelayMs = 0;

    NodeNum pendingUnicastNextHop = 0;
    UnicastSlotFlags pendingUnicastFlags;

    void commitRelay(PacketId packetId, NodeNum originalHeardFrom, uint32_t txDelayMs = 0);
    bool isCommittedRelay(PacketId packetId) const;
    uint32_t getCommittedRelayDelay(PacketId packetId) const;

    uint32_t ladderTransitionMs() const;
    uint32_t expiredRelayReanchorMs(PacketId packetId, uint32_t slotTimeMs) const;
    void clearCommittedRelay(PacketId packetId);

    bool areAllNeighborsCovered(const meshtastic_MeshPacket *p, NodeNum *uniqueNeighbor = nullptr);

    bool capsLastHop(const meshtastic_MeshPacket *p);

    void maybeScheduleBroadcastRetransmit(const meshtastic_MeshPacket *p);
    // Multiple deferring insurers need distinct delays when the ranked relay goes missing.
    void armDeferredBroadcastRetransmit(const meshtastic_MeshPacket *p, uint32_t staggerMs, uint8_t ladderRungs);
    void scheduleT1Broadcast(const meshtastic_MeshPacket *p, uint32_t staggerMs, bool deferred, uint8_t ladderRungs);
    void cancelBroadcastRetransmit(PacketId packetId);
};

extern SignalRoutingModule *signalRoutingModule;
