#pragma once
/**
 * NeighborGraph - Unified graph for signal-based routing
 *
 * Each node stores only:
 * 1. Direct neighbors (with full metrics) in neighbors[] slots
 * 2. Downstream routing table (which neighbor reaches which remote node)
 *
 * Memory: ~24 KB heap (targets 400+ mesh nodes on ESP32-C3)
 * Single class, no #ifdefs, works on all platforms including ESP32-C3.
 */

#include "NodeDB.h"
#include <cstdint>
#include <functional>
#include <limits>

// Fixed-size node set — replaces std::unordered_set<NodeNum> to avoid heap allocations.
// Capacity: enough for one node's edges + some margin. Keeps stack usage predictable.
static constexpr size_t NODE_SET_MAX = 48;

/// ETX costs are compared in buckets this wide (ETX × 100): half an ETX. Own links and peer-reported
/// links price a few hundredths apart, and exact comparison made colocated nodes disagree on order.
static constexpr uint16_t SR_COST_BUCKET_FIXED = 50;

struct NodeSet {
    NodeNum nodes[NODE_SET_MAX];
    uint16_t count = 0;

    void clear() { count = 0; }

    bool contains(NodeNum n) const {
        for (uint16_t i = 0; i < count; i++)
            if (nodes[i] == n) return true;
        return false;
    }

    bool insert(NodeNum n) {
        if (contains(n)) return false;
        if (count < NODE_SET_MAX) { nodes[count++] = n; return true; }
        return false;
    }

    void erase(NodeNum n) {
        for (uint16_t i = 0; i < count; i++) {
            if (nodes[i] == n) {
                nodes[i] = nodes[--count];
                return;
            }
        }
    }

    bool empty() const { return count == 0; }
    uint16_t size() const { return count; }
};

// Compile-time configuration
#ifndef NEIGHBOR_GRAPH_MAX_NEIGHBORS
#define NEIGHBOR_GRAPH_MAX_NEIGHBORS 32
#endif

#ifndef NEIGHBOR_GRAPH_MAX_EDGES_PER_NODE
// A city hub hears well over 24 nodes; with 24 slots it kept evicting weaker but real neighbours
// (FCM6 dropped Czar). Lists above MAX_SIGNAL_ROUTING_NEIGHBORS go out as several chunks.
// The graph is heap-allocated: 32x32 costs ~17 KB (24x24 was ~9.5 KB) on nodes that run with
// ~42-46 KB of free heap, so 40x40 (+16.5 KB) is not affordable here without a heap check.
#define NEIGHBOR_GRAPH_MAX_EDGES_PER_NODE 32
#endif

#ifndef NEIGHBOR_GRAPH_MAX_DOWNSTREAM
#define NEIGHBOR_GRAPH_MAX_DOWNSTREAM 1100
#endif

#ifndef NEIGHBOR_GRAPH_MAX_RELAY_STATES
#define NEIGHBOR_GRAPH_MAX_RELAY_STATES 32
#endif

#ifndef NEIGHBOR_GRAPH_MAX_CACHED_ROUTES
#define NEIGHBOR_GRAPH_MAX_CACHED_ROUTES 32
#endif

// Return values for updateEdge()
static constexpr int EDGE_NO_CHANGE = 0;
static constexpr int EDGE_NEW = 1;
static constexpr int EDGE_SIGNIFICANT_CHANGE = 2;

struct Edge {
    enum class Source : uint8_t { Mirrored = 0, Reported = 1 };

    NodeNum to;
    uint16_t etxFixed;   // ETX * 100 (fixed-point, range 1.00-655.35)
    uint32_t lastUpdate; // Full timestamp (seconds since boot)
    uint8_t etxVariance; // EWMA of |ETX change| × 20 (range 0.00–12.75 ETX units)
    Source source;
    bool hearsUs;        // True if this node proved it hears us: relayed our packets or listed us in its topology

    Edge() : to(0), etxFixed(100), lastUpdate(0), etxVariance(0), source(Source::Mirrored), hearsUs(false) {}

    float getEtx() const { return etxFixed / 100.0f; }
    void setEtx(float etx) { etxFixed = static_cast<uint16_t>(etx * 100.0f); }
    float getEtxVariance() const { return etxVariance / 20.0f; }
    void updateEtxVariance(float absChange)
    {
        // EWMA: alpha=0.25 for smooth tracking
        float cur = etxVariance / 20.0f;
        float updated = 0.75f * cur + 0.25f * absChange;
        uint16_t scaled = static_cast<uint16_t>(updated * 20.0f + 0.5f);
        etxVariance = (scaled > 255) ? 255 : static_cast<uint8_t>(scaled);
    }
};

struct NodeEdges {
    NodeNum nodeId;
    Edge edges[NEIGHBOR_GRAPH_MAX_EDGES_PER_NODE];
    uint8_t edgeCount;
    uint32_t lastFullUpdate; // Full timestamp for aging

    NodeEdges() : nodeId(0), edgeCount(0), lastFullUpdate(0) {}
};

// Cost factor of an unconfirmed hop in the fallback route search: the receiver never listed the
// sender, so the link is marginal or one-way; a confirmed path of up to this many times the raw
// cost is preferred.
static constexpr uint16_t UNVERIFIED_HOP_COST_FACTOR = 4;

// Bucket width (fixed-point ETX) for comparing links when picking a coverage owner: two nodes price
// the same link a few hundredths apart, so only a real difference may change ownership.
static constexpr uint16_t SR_OWNER_COST_BUCKET = 50;

struct Route {
    NodeNum destination;
    NodeNum nextHop;
    uint16_t costFixed; // Cost * 100 (fixed-point)
    uint32_t timestamp;
    uint8_t hops; // Path length, 1 for a direct neighbour; 0 from the downstream table or none
    // Every hop confirmed by its receiver. False for the inbound-gateway fallback (a hop into a
    // topology-publishing node that never confirmed the sender, at UNVERIFIED_HOP_COST_FACTOR times
    // its cost) and for downstream-table routes.
    bool verified;

    Route() : destination(0), nextHop(0), costFixed(0), timestamp(0), hops(0), verified(true) {}

    float getCost() const { return costFixed / 100.0f; }
};

struct RelayCandidate {
    NodeNum nodeId;
    uint8_t coverageCount; // unique coverage (not already covered)
    uint16_t avgCostFixed;
    uint8_t tier;
    uint8_t totalCoverage; // coverage before pre-coverage was subtracted (diagnostics)

    RelayCandidate() : nodeId(0), coverageCount(0), avgCostFixed(0), tier(0), totalCoverage(0) {}
    RelayCandidate(NodeNum node, uint8_t coverage, uint16_t cost, uint8_t t, uint8_t total = 0)
        : nodeId(node), coverageCount(coverage), avgCostFixed(cost), tier(t), totalCoverage(total) {}

    float getAvgCost() const { return avgCostFixed / 100.0f; }

    bool operator<(const RelayCandidate &other) const
    {
        if (tier != other.tier)
            return tier < other.tier;
        if (coverageCount != other.coverageCount)
            return coverageCount > other.coverageCount;
        return avgCostFixed < other.avgCostFixed;
    }
};

struct RelayState {
    NodeNum nodeId;
    uint32_t packetId;
    uint16_t timestampLo; // Lower 16 bits

    RelayState() : nodeId(0), packetId(0), timestampLo(0) {}
};

struct DownstreamEntry {
    NodeNum destination; // Remote node
    NodeNum relay;       // Which direct neighbor reaches it
    uint16_t costFixed;  // Cumulative ETX * 100
    uint32_t lastUpdate; // For aging

    DownstreamEntry() : destination(0), relay(0), costFixed(0), lastUpdate(0) {}
};

class NeighborGraph {
  public:
    static uint32_t getContentionWindowMs();


    NeighborGraph();

    // --- Core methods ---

    int updateEdge(NodeNum from, NodeNum to, float etx, uint32_t timestamp,
                   Edge::Source source = Edge::Source::Mirrored, bool updateTimestamp = true);

    const NodeEdges *getEdgesFrom(NodeNum node) const;

    void updateNodeActivity(NodeNum nodeId, uint32_t timestamp);

    void ageEdges(uint32_t currentTimeSecs, uint32_t ttlSecs);

    // Returns the count of nodes that have a Reported edge pointing to our node
    // (i.e., direct neighbors we have directly received a signal from).
    uint8_t countDirectNeighbors() const;

    // Route to `destination`: a Dijkstra search run backwards from the destination over "who hears
    // whom". A settled node N is reached by the nodes that can deliver to it: the nodes N lists (N
    // hears them, priced at the cost N measured on their signal), the nodes whose edge to N carries
    // hearsUs (N confirmed it hears them), and, when N publishes no topology (publishesTopology(N)
    // false), anyone who hears N. An edge is never used against its direction and every hop is
    // priced at its receiver. nodeFilter gates intermediate hops only. When no confirmed path
    // exists (nor a downstream-table one), the search runs again allowing unconfirmed hops at
    // UNVERIFIED_HOP_COST_FACTOR times their cost, so the node that hears the far side still
    // carries the frame out; that route is marked unverified.
    // Predicates the route search asks the caller: may `node` relay (intermediate hops only), and
    // does `node` publish topology. Plain function pointers with a context, not std::function: the
    // image sits at the BLE OTA size limit and every std::function instantiation costs flash.
    struct RoutePolicy {
        void *ctx;
        bool (*routable)(void *ctx, NodeNum node);
        bool (*publishes)(void *ctx, NodeNum node);
        RoutePolicy() : ctx(nullptr), routable(nullptr), publishes(nullptr) {}
    };
    Route calculateRoute(NodeNum destination, uint32_t currentTime, const RoutePolicy &policy = RoutePolicy());

    Route getCachedRoute(NodeNum destination, uint32_t currentTime);

    void clearCache();

    static float calculateETX(int32_t rssi, float snr);

    static void etxToSignal(float etx, int32_t &rssi, int32_t &snr);

    // --- Downstream methods (new) ---

    void updateDownstream(NodeNum destination, NodeNum relay, float totalCost, uint32_t timestamp);

    // Like updateDownstream, but ensures each destination has exactly one relay entry.
    // If the destination already exists with a different relay, the relay is replaced.
    void updateDownstreamExclusive(NodeNum destination, NodeNum relay, float totalCost, uint32_t timestamp);

    NodeNum getDownstreamRelay(NodeNum destination) const;

    bool isDownstream(NodeNum destination) const;

    size_t getDownstreamCountForRelay(NodeNum relay) const;

    size_t getDownstreamNodesForRelay(NodeNum relay, NodeNum *outArray, uint16_t *outCosts, size_t maxCount,
                                      size_t skipCount = 0) const;

    bool isRelayFor(NodeNum myNode, NodeNum destination) const;

    void clearDownstreamForRelay(NodeNum relay);

    // Transfer all downstream entries from oldRelay to newRelay, then clear oldRelay
    size_t transferDownstream(NodeNum oldRelay, NodeNum newRelay);

    void clearDownstreamForDestination(NodeNum destination);

    // --- Relay decisions ---

    bool shouldRelayEnhanced(NodeNum myNode, NodeNum sourceNode, NodeNum heardFrom, uint32_t currentTime, uint32_t packetId,
                             uint32_t packetRxTime = 0, const NodeNum *coListeners = nullptr,
                             uint8_t coListenerCount = 0) const;

    bool shouldRelayEnhancedConservative(NodeNum myNode, NodeNum sourceNode, NodeNum heardFrom, uint32_t currentTime,
                                         uint32_t packetId, uint32_t packetRxTime = 0,
                                         const NodeNum *coListeners = nullptr, uint8_t coListenerCount = 0) const;

    bool shouldRelaySimple(NodeNum myNode, NodeNum sourceNode, NodeNum heardFrom, uint32_t currentTime) const;

    bool shouldRelaySimpleConservative(NodeNum myNode, NodeNum sourceNode, NodeNum heardFrom, uint32_t currentTime) const;

    // What the module knows about roles: which nodes publish topology, which are stock relay
    // routers, which are SR-active, and whether we relay at all. Plain function pointers with a
    // context, not std::function: the image sits at the BLE OTA size limit and every std::function
    // instantiation costs flash (the same reason RoutePolicy is shaped this way).
    struct CoveragePolicy {
        void *ctx;
        bool (*publishesTopology)(void *ctx, NodeNum node);
        bool (*isStockRelayRouter)(void *ctx, NodeNum node);
        bool (*isSrActive)(void *ctx, NodeNum node);
        NodeNum me;
        bool meRelays;
        float poorLinkEtx;
        CoveragePolicy()
            : ctx(nullptr), publishesTopology(nullptr), isStockRelayRouter(nullptr), isSrActive(nullptr), me(0),
              meRelays(false), poorLinkEtx(0.0f)
        {
        }
        bool reports(NodeNum node) const { return publishesTopology && publishesTopology(ctx, node); }
    };

    /// `selfNode` is the node running the ranking: its own coverage counts only Reported edges (what it
    /// broadcasts in its topology), so peers ranking it from their mirrored view reach the same order.
    // poorLinkEtx: coverage ceiling handed to `covers` for both the coverage sets and the cost.
    RelayCandidate findBestRelayCandidate(const NodeSet &candidates, const NodeSet &alreadyCovered,
                                          uint32_t currentTime, uint32_t packetId,
                                          bool preferHighNodeId = false, NodeNum sourceNode = 0,
                                          NodeNum selfNode = 0, const CoveragePolicy *policy = nullptr) const;

    // Is `to` known to hear `from`? Edges are one-directional evidence: `from` listing `to` only
    // says `from` hears `to`. The reverse needs hearsUs on that edge (`to` confirmed it) or `to`
    // listing `from`.
    bool knownToHear(NodeNum from, NodeNum to) const;

    // Cost of the hop from → to, priced at the receiver when it published a measurement of the
    // sender, else at the sender's own. 0 when neither has an edge.
    float hopCost(NodeNum from, NodeNum to) const;

    // Does a transmission by `from` reach `to` well enough to relieve a bystander of relaying?
    //
    // What counts as evidence depends on whether the receiver ever reports. `publishesTopology(to)`
    // true: it is held to its own lists and must be known to hear the sender (knownToHear), since
    // its silence about the sender is itself information. False (stock, mute, unclassified): it can
    // never confirm anything, so the sender's own edge to it is all the evidence there will ever be
    // — demanding more made every neighbour of one silent node relay for it on every frame. Either
    // way the delivery-direction link must not be hopeless (hearsUs is sticky, so a peer that heard
    // the sender once keeps the flag while its link decays). `poorLinkEtx` 0 drops the ceiling.
    bool covers(NodeNum from, NodeNum to, float poorLinkEtx, const CoveragePolicy *policy = nullptr) const;

    // Who relays for `target` when no transmitter can be shown to reach it? Exactly one node, or
    // the whole branch relays the same frame for the same unconfirmed neighbour. The target never
    // reports, so its receive path is all anyone can measure: the node hearing it best is the
    // likeliest to be heard by it. Stock ROUTER/REPEATER/ROUTER_CLIENT first (they rebroadcast
    // regardless of SR and already hold the earliest slots), then the best link in
    // SR_OWNER_COST_BUCKET buckets, then the lowest node id. `meRelays` is our own eligibility:
    // mute and passive roles never own, because they do not relay.
    NodeNum coverageOwner(NodeNum target, const CoveragePolicy &policy) const;

    /// Does `relay` carry `target` if it transmits: it can be shown to deliver, or `target` is a
    /// neighbour nobody can be shown to reach and `relay` is the one node that owns it. The
    /// ranking and the absorb step must credit the same set, or an owned neighbour is left
    /// uncovered after its owner takes a slot and a later phase relays for it again.
    bool admitsCoverage(NodeNum relay, NodeNum target, float poorLinkEtx,
                        const CoveragePolicy *policy) const;

    /// Whose copy an originator will actually hear: among the nodes it can be shown to hear
    /// (its own published list, or us watching it carry their frame), the cheapest in the
    /// delivery direction, stock rebroadcasters given way, node id as the tie-break.
    /// Deliberately not coverageOwner(): that ranks a candidate's own edge *to* the target,
    /// which is the only evidence for a neighbour nobody can be shown to reach but the wrong
    /// direction for a witness.
    NodeNum witnessOwner(NodeNum source, const CoveragePolicy &policy) const;

    // May a frame from `from` be delivered to `to`? Evidenced delivery (knownToHear), or `to`
    // publishes no topology and its silence is no proof it cannot hear. The optimistic half is
    // what keeps stock destinations reachable; coverage decisions use the strict `covers` instead.
    bool canDeliver(NodeNum from, NodeNum to, const RoutePolicy &policy) const;

    size_t getCoverageIfRelays(NodeNum relay, NodeNum *coveredNodes, size_t maxNodes, const NodeNum *alreadyCovered,
                               size_t alreadyCoveredCount, NodeNum selfNode = 0,
                               const CoveragePolicy *policy = nullptr) const;

    /// Do we still reach a direct neighbour none of `coveredBy` reaches? A coverer counts only when
    /// it `covers` the neighbour, the same rule pre-coverage applies at ranking time.
    bool hasUniqueCoverage(NodeNum myNode, const NodeNum *coveredBy, size_t coveredByCount, float poorLinkEtx = 0.0f,
                           const CoveragePolicy *policy = nullptr) const
    {
        return uniqueCoverageNeighbor(myNode, coveredBy, coveredByCount, poorLinkEtx, policy) != 0;
    }

    /// The neighbour that makes our relay worth its airtime: ours to cover and reached by none of
    /// `coveredBy`. Returned rather than reduced to a bool so the decision can be logged — a relay
    /// nobody needs and a relay that saves a node look identical in a field log otherwise.
    NodeNum uniqueCoverageNeighbor(NodeNum myNode, const NodeNum *coveredBy, size_t coveredByCount,
                                   float poorLinkEtx = 0.0f, const CoveragePolicy *policy = nullptr) const;

    bool isGatewayNode(NodeNum nodeId, NodeNum sourceNode) const;

    bool shouldRelayWithContention(NodeNum myNode, NodeNum sourceNode, NodeNum heardFrom, uint32_t packetId,
                                   uint32_t currentTime) const;

    void recordNodeTransmission(NodeNum nodeId, uint32_t packetId, uint32_t currentTime);

    bool hasNodeTransmitted(NodeNum nodeId, uint32_t packetId, uint32_t currentTime) const;

    // --- Node management ---

    void setEdgeHearsUs(NodeNum from, NodeNum to, bool hearsUs);

    void removeNode(NodeNum nodeId);

    void clearEdgesForNode(NodeNum nodeId);

    void removeEdgesTo(NodeNum nodeId);

    void clearInferredEdgesToNode(NodeNum nodeId);

    uint8_t getNeighborCount(NodeNum node) const;

    size_t getNodeCount() const { return neighborCount; }

    size_t getAllNodeIds(NodeNum *outArray, size_t maxCount) const;

    static constexpr size_t getMemoryUsage() { return sizeof(NeighborGraph); }

    void setEtxChangeThreshold(float v) { etxChangeThreshold = v; }
    float getEtxChangeThreshold() const { return etxChangeThreshold; }

  private:
    float etxChangeThreshold = 1.2f; // Minimum ETX delta to register an edge change as significant
    NodeEdges neighbors[NEIGHBOR_GRAPH_MAX_NEIGHBORS];
    uint8_t neighborCount;

    DownstreamEntry downstream[NEIGHBOR_GRAPH_MAX_DOWNSTREAM];
    uint16_t downstreamCount;

    RelayState relayStates[NEIGHBOR_GRAPH_MAX_RELAY_STATES];
    uint8_t relayStateCount;

    Route routeCache[NEIGHBOR_GRAPH_MAX_CACHED_ROUTES];
    uint8_t routeCacheCount;
    static constexpr uint32_t ROUTE_CACHE_TIMEOUT_SECS = 300;

    uint32_t nodeTtlSecs = 5400; // Set by ageEdges(), used for downstream freshness checks

    // Find or create neighbor slot (returns nullptr if full)
    NodeEdges *findOrCreateNeighbor(NodeNum nodeId);

    // Find neighbor slot (returns nullptr if not found)
    NodeEdges *findNeighbor(NodeNum nodeId);
    const NodeEdges *findNeighbor(NodeNum nodeId) const;

    // Find edge in node (returns nullptr if not found)
    Edge *findEdge(NodeEdges *node, NodeNum to);
    const Edge *findEdge(const NodeEdges *node, NodeNum to) const;

    // Check if a node is our direct neighbor (has a slot)
    bool isOurDirectNeighbor(NodeNum nodeId) const;
};
