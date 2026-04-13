#pragma once
#include "ProtobufModule.h"
#include "concurrency/OSThread.h"
#include "mesh/generated/meshtastic/mesh.pb.h"
#include "mesh/generated/meshtastic/telemetry.pb.h"

#include "graph/NeighborGraph.h"

// Routing protocol version for compatibility checking
#define SIGNAL_ROUTING_VERSION 3

// Packed neighbor format constants
static constexpr uint8_t PACKED_NEIGHBOR_FORMAT_VERSION = 1;
static constexpr uint8_t PACKED_NEIGHBOR_ENTRY_SIZE = 8; // bytes per entry: 4 node_id + 1 rssi + 1 snr + 1 flags + 1 etx_variance
static constexpr uint8_t PACKED_NEIGHBOR_HEADER_SIZE = 5; // format_version + entry_size + routing_version + topology_version + header_flags
static constexpr uint8_t PACKED_NEIGHBOR_FLAG_SR_ACTIVE = 0x01;
static constexpr uint8_t PACKED_NEIGHBOR_FLAG_HEARS_US = 0x02;
static constexpr uint8_t PACKED_HEADER_FLAG_SR_ACTIVE = 0x01;

// Maximum neighbors per SR broadcast packet (28 fit in 233 byte payload with packed encoding)
#define MAX_SIGNAL_ROUTING_NEIGHBORS 28

// Decoded packed neighbor entry for iteration
struct PackedNeighborEntry {
    NodeNum nodeId;
    int8_t rssi;
    int8_t snr;
    bool signalRoutingActive;
    bool hearsUs;
    uint8_t etxVariance;
};

// Decoded packed header metadata
struct PackedHeader {
    uint8_t formatVersion;
    uint8_t entrySize;
    uint8_t routingVersion;
    uint8_t topologyVersion;
    bool signalRoutingActive;
};

// Decode packed_neighbors bytes. Returns number of entries decoded into outEntries.
// outEntries must have room for at least maxEntries elements.
// header is filled with the packet-level metadata from the 5-byte header.
static inline uint8_t decodePackedNeighbors(const uint8_t *data, size_t dataLen,
                                            PackedNeighborEntry *outEntries, uint8_t maxEntries,
                                            PackedHeader *header = nullptr)
{
    if (!data || dataLen < PACKED_NEIGHBOR_HEADER_SIZE) {
        return 0;
    }
    uint8_t entrySize = data[1];
    if (entrySize < PACKED_NEIGHBOR_ENTRY_SIZE || entrySize == 0) {
        return 0; // unknown or too-small entry format
    }

    if (header) {
        header->formatVersion = data[0];
        header->entrySize = entrySize;
        header->routingVersion = data[2];
        header->topologyVersion = data[3];
        header->signalRoutingActive = (data[4] & PACKED_HEADER_FLAG_SR_ACTIVE) != 0;
    }

    size_t payloadLen = dataLen - PACKED_NEIGHBOR_HEADER_SIZE;
    uint8_t entryCount = payloadLen / entrySize;
    if (entryCount > maxEntries) {
        entryCount = maxEntries;
    }

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


// Broadcast interval for signal routing info (6 minutes)
#define SIGNAL_ROUTING_BROADCAST_SECS 360

// Minimum inter-broadcast interval: dirty topology triggers an early broadcast only after this
// many seconds have elapsed since the last broadcast.
#define SIGNAL_ROUTING_DIRTY_BROADCAST_SECS 300

// Maximum hops for SR topology broadcasts; capped at min(user config, this value)
#define SR_BROADCAST_MAX_HOPS 5

class SignalRoutingModule : public ProtobufModule<meshtastic_SignalRoutingInfo>, private concurrency::OSThread
{
public:
    SignalRoutingModule();

    // Delete copy constructor and assignment operator since this class manages dynamic memory and threading
    SignalRoutingModule(const SignalRoutingModule&) = delete;
    SignalRoutingModule& operator=(const SignalRoutingModule&) = delete;

    bool shouldUseSignalBasedRouting(const meshtastic_MeshPacket *p);
    void updateNodeActivityForPacket(NodeNum nodeId);
    void updateNodeActivityForPacketAndRelay(const meshtastic_MeshPacket *p);
    bool shouldRelay(const meshtastic_MeshPacket *p);
    bool shouldRelayBroadcast(const meshtastic_MeshPacket *p);
    NodeNum getNextHop(NodeNum destination, NodeNum sourceNode = 0, NodeNum heardFrom = 0, bool allowOpportunistic = true);
    NodeNum findBetterPositionedNeighbor(NodeNum destination, NodeNum sourceNode, NodeNum heardFrom,
                                       float ourRouteCost, uint32_t currentTime);
    bool shouldRelayUnicastForCoordination(const meshtastic_MeshPacket *p);
    bool hasDirectConnectivity(NodeNum nodeA, NodeNum nodeB);
    bool hasVerifiedConnectivity(NodeNum transmitter, NodeNum receiver, bool* unknownOut = nullptr);
    void updateNeighborInfo(NodeNum nodeId, int32_t rssi, float snr, uint32_t lastRxTime);
    void sendSignalRoutingInfo(NodeNum dest = NODENUM_BROADCAST);
    // Call when this node originates and sends any packet (not a relay).
    // Resets the topology-broadcast keepalive timer so we don't send redundant broadcasts
    // while we are already visible to neighbors due to recent transmissions.
    void notifyOriginatedPacketSent();

    // Returns true if the packet was received directly from the sender (not relayed).
    // Checks both hop_start==hop_limit and relay_node matching the sender's last byte.
    static bool isDirectPacket(const meshtastic_MeshPacket &mp);
    void preProcessSignalRoutingPacket(const meshtastic_MeshPacket *p, uint32_t packetReceivedTimestamp = 0);

protected:
    virtual bool handleReceivedProtobuf(const meshtastic_MeshPacket &mp, meshtastic_SignalRoutingInfo *p) override;
    void updateGraphWithNeighbor(NodeNum sender, NodeNum neighborId, int8_t rssi, int8_t snr, bool hearsUs);
    virtual ProcessMessage handleReceived(const meshtastic_MeshPacket &mp) override;
    virtual bool wantPacket(const meshtastic_MeshPacket *p) override { return true; }
    virtual meshtastic_MeshPacket *allocReply() override;
    virtual int32_t runOnce() override;

private:
    NeighborGraph *routingGraph = nullptr;
    uint32_t lastGraphUpdate = 0;
    static constexpr uint32_t GRAPH_MAINTENANCE_INTERVAL_SECS = 60;
    static constexpr uint32_t NODE_TTL_SECS = 5400;    // 90 min for all nodes in the graph
    static constexpr uint32_t CAPABILITY_TTL_SECS = SIGNAL_ROUTING_BROADCAST_SECS * 3 + 10;  // detect when SR node stops being SR
    static constexpr uint32_t RELAY_ID_CACHE_TTL_MS = 600 * 1000;  // 10 min

    bool signalBasedRoutingEnabled = true;
    bool needsBootBroadcast = false;
    bool topologyDirty = false; // Set when topology changes; triggers early broadcast via runOnce
    uint32_t lastBroadcast = 0;
    uint8_t currentTopologyVersion = 0;

    static constexpr size_t MAX_TOPOLOGY_VERSION_ENTRIES = 24;
    struct TopologyVersionEntry {
        NodeNum nodeId = 0;
        uint8_t version = 0;
    };
    TopologyVersionEntry lastTopologyVersion[MAX_TOPOLOGY_VERSION_ENTRIES];
    uint8_t lastTopologyVersionCount = 0;
    TopologyVersionEntry lastPreProcessedVersion[MAX_TOPOLOGY_VERSION_ENTRIES];
    uint8_t lastPreProcessedVersionCount = 0;

    uint8_t getTopologyVersion(const TopologyVersionEntry *table, uint8_t count, NodeNum nodeId) const;
    void setTopologyVersion(TopologyVersionEntry *table, uint8_t &count, NodeNum nodeId, uint8_t version);

    bool isSignalBasedCapable(NodeNum nodeId) const;
    uint8_t packNeighborsForBroadcast(uint8_t *outBuf, size_t bufSize);
    void sendTopologyPacket(NodeNum dest, const uint8_t *packedData, size_t packedLen, uint8_t topologyVersion = 0, uint32_t txAfterMs = 0);

    enum class CapabilityStatus : uint8_t {
        Unknown = 0,
        SRactive,
        Passive,
        Legacy
    };

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
    bool topologyHealthyForBroadcast() const;
    bool topologyHealthyForUnicast(NodeNum destination) const;
    bool isImmediateRelayRouter(NodeNum nodeId) const;
    bool isLegacyRouter(NodeNum nodeId) const;
    void rememberRelayIdentity(NodeNum nodeId, uint8_t relayId);
    void pruneRelayIdentityCache(uint32_t nowMs);
    NodeNum resolveRelayIdentity(uint8_t relayId, int16_t rxRssi = 0, float rxSnr = 0) const;
public:
    NodeNum resolveHeardFrom(const meshtastic_MeshPacket *p, NodeNum sourceNode) const;
private:
    bool isActiveRoutingRole() const;
    bool canSendTopology() const;
    void handleNodeInfoPacket(const meshtastic_MeshPacket &mp);
    CapabilityStatus capabilityFromRole(meshtastic_Config_DeviceConfig_Role role) const;
    void handleSniffedPayload(const meshtastic_MeshPacket &mp, bool isDirectNeighbor);
    void handlePositionPacket(const meshtastic_MeshPacket &mp, bool isDirectNeighbor);
    void handleTelemetryPacket(const meshtastic_MeshPacket &mp);
    void handleRoutingControlPacket(const meshtastic_MeshPacket &mp);

    bool isPlaceholderNode(NodeNum nodeId) const;
    NodeNum createPlaceholderNode(uint8_t relayId);
    bool resolvePlaceholder(NodeNum placeholderId, NodeNum realNodeId);
    NodeNum getPlaceholderForRelay(uint8_t relayId) const;
    void replaceGatewayNode(NodeNum oldNode, NodeNum newNode);
    bool isPlaceholderConnectedToUs(NodeNum placeholderId) const;
    bool shouldRelayForStockNeighbors(NodeNum myNode, NodeNum sourceNode, NodeNum heardFrom, uint32_t currentTime);
    bool hasBetterPositionedSRNeighbor(NodeNum myNode, NodeNum heardFrom, NodeNum destination = 0);
    bool isNodeRoutable(NodeNum nodeId) const;
    void logNetworkTopology();

    void markTopologyDirty();
    bool isNonRelayingLegacyRole(NodeNum nodeId) const;
    void markStockNodeRelayedOurPacket(NodeNum stockNode);

    // Committed relay tracking — prevents dupe cancellation of SR relay decisions
    static constexpr size_t MAX_COMMITTED_RELAYS = 8;
    static constexpr size_t MAX_HEARD_TRANSMITTERS = 6;
    struct CommittedRelay {
        PacketId packetId = 0;
        NodeNum originalHeardFrom = 0;
        uint32_t txDelayMs = 0;
        NodeNum heardTransmitters[MAX_HEARD_TRANSMITTERS];
        uint8_t heardTransmitterCount = 0;
        CommittedRelay() : packetId(0), originalHeardFrom(0), txDelayMs(0), heardTransmitterCount(0) {
            memset(heardTransmitters, 0, sizeof(heardTransmitters));
        }
    };
    CommittedRelay committedRelays[MAX_COMMITTED_RELAYS];
    uint8_t committedRelayCount = 0;

    // Broadcast retransmit insurance: one-shot T1 resend after ROUTER_LATE window if no relay heard
    static constexpr size_t MAX_PENDING_RETRANSMITS = 4;
    struct PendingRetransmit {
        PacketId packetId = 0;
        meshtastic_MeshPacket *packet = nullptr;
        uint32_t fireAfterMs = 0;
        bool canceled = false;
        PendingRetransmit() : packetId(0), packet(nullptr), fireAfterMs(0), canceled(false) {}
    };
    PendingRetransmit pendingRetransmits[MAX_PENDING_RETRANSMITS];
    bool isRetransmitting = false; // Guard: prevents T2 scheduling when T1 is being fired

    // Runtime-configurable values loaded from moduleConfig.signal_routing at construction.
    // All default to the compile-time constants; overridden when has_signal_routing is set.
    uint32_t cfgBroadcastSecs = SIGNAL_ROUTING_BROADCAST_SECS;
    uint32_t cfgDirtyBroadcastSecs = SIGNAL_ROUTING_DIRTY_BROADCAST_SECS;
    uint32_t cfgNodeTtlSecs = NODE_TTL_SECS;
    uint32_t cfgBroadcastMaxHops = SR_BROADCAST_MAX_HOPS;
    float cfgPoorLinkEtxThreshold = 7.0f;
    bool t1RetransmitEnabled = true;

    bool hasAnyHearsUsNeighbor() const;
    bool allHearsUsNeighborsHeardPacket(PacketId packetId) const;

public:
    uint32_t pendingRelayDelayMs = 0; // Set by shouldRelayBroadcast, consumed by commitRelay

    void commitRelay(PacketId packetId, NodeNum originalHeardFrom, uint32_t txDelayMs = 0);
    bool isCommittedRelay(PacketId packetId) const;
    uint32_t getCommittedRelayDelay(PacketId packetId) const;
    void clearCommittedRelay(PacketId packetId);
    bool areAllNeighborsCovered(const meshtastic_MeshPacket *p);
    // Returns the hop_limit to set for a unicast to a direct hearsUs neighbor when stock
    // neighbors are present, or -1 if no limiting should be applied.
    // Good links (ETX < 3.0): 0 hops (direct delivery, no further relay)
    // Marginal links: 1 hop (allow one retry relay if our TX is lost)
    int8_t getUnicastHopLimitForDirectNeighbor(const meshtastic_MeshPacket *p);
    void maybeScheduleBroadcastRetransmit(const meshtastic_MeshPacket *p);
    void cancelBroadcastRetransmit(PacketId packetId);
};

extern SignalRoutingModule *signalRoutingModule;
