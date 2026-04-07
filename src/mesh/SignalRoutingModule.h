#pragma once
#include "ProtobufModule.h"
#include "concurrency/OSThread.h"
#include "mesh/generated/meshtastic/mesh.pb.h"
#include "mesh/generated/meshtastic/telemetry.pb.h"

#include "graph/NeighborGraph.h"

// Routing protocol version for compatibility checking
#define SIGNAL_ROUTING_VERSION 2

// Maximum neighbors per SR broadcast packet (11 fit in 233 byte payload)
#define MAX_SIGNAL_ROUTING_NEIGHBORS 11

// Broadcast interval for signal routing info (6 minutes)
#define SIGNAL_ROUTING_BROADCAST_SECS 360

// Minimum inter-broadcast interval: dirty topology triggers an early broadcast only after this
// many seconds have elapsed since the last broadcast.
#define SIGNAL_ROUTING_DIRTY_BROADCAST_SECS 180

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
    void updateNeighborInfo(NodeNum nodeId, int32_t rssi, float snr, uint32_t lastRxTime, uint32_t variance = 0);
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
    void updateGraphWithNeighbor(NodeNum sender, const meshtastic_SignalNeighbor &neighbor);
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
    void collectNeighborsForBroadcast(meshtastic_SignalNeighbor *outNeighbors, uint8_t &outCount, uint8_t maxCount);
    void sendTopologyPacket(NodeNum dest, const meshtastic_SignalNeighbor *neighbors, uint8_t count, uint8_t topologyVersion = 0, uint32_t txAfterMs = 0);

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
    NodeNum resolveRelayIdentity(uint8_t relayId) const;
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

    bool hasAnyHearsUsNeighbor() const;

public:
    uint32_t pendingRelayDelayMs = 0; // Set by shouldRelayBroadcast, consumed by commitRelay

    void commitRelay(PacketId packetId, NodeNum originalHeardFrom, uint32_t txDelayMs = 0);
    bool isCommittedRelay(PacketId packetId) const;
    uint32_t getCommittedRelayDelay(PacketId packetId) const;
    void clearCommittedRelay(PacketId packetId);
    bool areAllNeighborsCovered(const meshtastic_MeshPacket *p);
    bool shouldZeroHopLimitForUnicastRelay(const meshtastic_MeshPacket *p);
    void maybeScheduleBroadcastRetransmit(const meshtastic_MeshPacket *p);
    void cancelBroadcastRetransmit(PacketId packetId);
};

extern SignalRoutingModule *signalRoutingModule;
