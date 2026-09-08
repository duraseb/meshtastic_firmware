#include "TestUtil.h"
#include "mesh/NodeDB.h"
#include "mesh/graph/NeighborGraph.h"
#include "mesh/SignalRoutingModule.h"
#include <cstring>
#include <unity.h>

#if !MESHTASTIC_EXCLUDE_SIGNALROUTING

namespace
{

class TestNodeDB : public NodeDB
{
};

TestNodeDB *testNodeDB = nullptr;

static void initGraphTestNodeDb(NodeNum localNode)
{
    if (!testNodeDB) {
        testNodeDB = new TestNodeDB();
        nodeDB = testNodeDB;
    }
    myNodeInfo.my_node_num = localNode;
}

static uint8_t buildPackedBuffer(uint8_t *buf, size_t bufSize, NodeNum nodeId, int8_t rssi, int8_t snr, bool srActive,
                                 bool hearsUs, uint8_t etxVariance)
{
    TEST_ASSERT_GREATER_OR_EQUAL(PACKED_NEIGHBOR_HEADER_SIZE + PACKED_NEIGHBOR_ENTRY_SIZE, bufSize);

    buf[0] = PACKED_NEIGHBOR_FORMAT_VERSION;
    buf[1] = PACKED_NEIGHBOR_ENTRY_SIZE;
    buf[2] = SIGNAL_ROUTING_VERSION;
    buf[3] = 7;
    buf[4] = PACKED_HEADER_FLAG_SR_ACTIVE;

    encodePackedNeighborEntry(&buf[PACKED_NEIGHBOR_HEADER_SIZE], nodeId, rssi, snr, srActive, hearsUs, etxVariance);
    return PACKED_NEIGHBOR_HEADER_SIZE + PACKED_NEIGHBOR_ENTRY_SIZE;
}

static void test_encode_decode_round_trip()
{
    uint8_t buf[32] = {};
    constexpr NodeNum nodeId = 0x0A0B0C0D;
    const int8_t rssi = -72;
    const int8_t snr = 9;
    const uint8_t etxVariance = 42;

    size_t packedLen = buildPackedBuffer(buf, sizeof(buf), nodeId, rssi, snr, true, true, etxVariance);

    PackedNeighborEntry out[1] = {};
    PackedHeader header = {};
    uint8_t count = decodePackedNeighbors(buf, packedLen, out, 1, &header);

    TEST_ASSERT_EQUAL_UINT8(1, count);
    TEST_ASSERT_EQUAL_UINT8(PACKED_NEIGHBOR_FORMAT_VERSION, header.formatVersion);
    TEST_ASSERT_EQUAL_UINT8(PACKED_NEIGHBOR_ENTRY_SIZE, header.entrySize);
    TEST_ASSERT_EQUAL_UINT8(SIGNAL_ROUTING_VERSION, header.routingVersion);
    TEST_ASSERT_EQUAL_UINT8(7, header.topologyVersion);
    TEST_ASSERT_TRUE(header.signalRoutingActive);
    TEST_ASSERT_EQUAL_UINT32(nodeId, out[0].nodeId);
    TEST_ASSERT_EQUAL_INT8(rssi, out[0].rssi);
    TEST_ASSERT_EQUAL_INT8(snr, out[0].snr);
    TEST_ASSERT_TRUE(out[0].signalRoutingActive);
    TEST_ASSERT_TRUE(out[0].hearsUs);
    TEST_ASSERT_EQUAL_UINT8(etxVariance, out[0].etxVariance);
}

static void test_packed_layout_offsets()
{
    uint8_t buf[32] = {};
    constexpr NodeNum nodeId = 0x78563412;
    const int8_t rssi = -95;
    const int8_t snr = 4;

    size_t packedLen = buildPackedBuffer(buf, sizeof(buf), nodeId, rssi, snr, false, true, 11);
    const uint8_t *entry = &buf[PACKED_NEIGHBOR_HEADER_SIZE];

    TEST_ASSERT_EQUAL_UINT8(0x12, entry[0]);
    TEST_ASSERT_EQUAL_UINT8(0x34, entry[1]);
    TEST_ASSERT_EQUAL_UINT8(0x56, entry[2]);
    TEST_ASSERT_EQUAL_UINT8(0x78, entry[3]);
    TEST_ASSERT_EQUAL_INT8(rssi, static_cast<int8_t>(entry[4]));
    TEST_ASSERT_EQUAL_INT8(snr, static_cast<int8_t>(entry[5]));
    TEST_ASSERT_EQUAL_UINT8(PACKED_NEIGHBOR_FLAG_HEARS_US, entry[6]);
    TEST_ASSERT_EQUAL_UINT8(11, entry[7]);
    TEST_ASSERT_EQUAL_UINT8(1, packedLen / PACKED_NEIGHBOR_ENTRY_SIZE);
}

static void test_merge_cost_from_decoded_signal()
{
    uint8_t buf[32] = {};
    const int8_t rssi = -80;
    const int8_t snr = 10;

    size_t packedLen = buildPackedBuffer(buf, sizeof(buf), 0x11111111, rssi, snr, true, false, 0);

    PackedNeighborEntry out[1] = {};
    uint8_t count = decodePackedNeighbors(buf, packedLen, out, 1);
    TEST_ASSERT_EQUAL_UINT8(1, count);

    float etx = NeighborGraph::calculateETX(out[0].rssi, out[0].snr);
    float expected = NeighborGraph::calculateETX(rssi, snr);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, expected, etx);
}

static void test_reject_v2_format_returns_zero()
{
    uint8_t buf[32] = {};
    buf[0] = 2; // v2 etx_fixed wire format — not supported on this branch
    buf[1] = PACKED_NEIGHBOR_ENTRY_SIZE;
    buf[2] = SIGNAL_ROUTING_VERSION;
    buf[3] = 1;
    buf[4] = 0;

    encodePackedNeighborEntry(&buf[PACKED_NEIGHBOR_HEADER_SIZE], 0x01020304, -80, 10, false, false, 0);

    PackedHeader header = {};
    PackedNeighborEntry out[1] = {};
    uint8_t count = decodePackedNeighbors(buf, PACKED_NEIGHBOR_HEADER_SIZE + PACKED_NEIGHBOR_ENTRY_SIZE, out, 1, &header);

    TEST_ASSERT_EQUAL_UINT8(0, count);
    TEST_ASSERT_EQUAL_UINT8(2, header.formatVersion);
}

static void test_direct_signal_upsert_lookup_and_prune()
{
    DirectNeighborSignal table[NEIGHBOR_GRAPH_MAX_EDGES_PER_NODE] = {};
    uint8_t count = 0;

    upsertDirectNeighborSignal(table, count, NEIGHBOR_GRAPH_MAX_EDGES_PER_NODE, 0x11111111, -70, 8, 1000);
    upsertDirectNeighborSignal(table, count, NEIGHBOR_GRAPH_MAX_EDGES_PER_NODE, 0x22222222, -90, 2, 1000);

    const DirectNeighborSignal *first = lookupDirectNeighborSignal(table, count, 0x11111111);
    TEST_ASSERT_NOT_NULL(first);
    TEST_ASSERT_EQUAL_INT8(-70, first->rssi);
    TEST_ASSERT_EQUAL_INT8(8, first->snr);

    upsertDirectNeighborSignal(table, count, NEIGHBOR_GRAPH_MAX_EDGES_PER_NODE, 0x11111111, -65, 10, 1100);
    const DirectNeighborSignal *updated = lookupDirectNeighborSignal(table, count, 0x11111111);
    TEST_ASSERT_NOT_NULL(updated);
    TEST_ASSERT_EQUAL_INT8(-65, updated->rssi);
    TEST_ASSERT_EQUAL_INT8(10, updated->snr);
    TEST_ASSERT_EQUAL_UINT32(1100, updated->lastRx);

    pruneDirectNeighborSignals(table, count, 9000, 7200);
    TEST_ASSERT_EQUAL_UINT8(0, count);
}

static void test_empty_topology_reply_delay_range_and_determinism()
{
    constexpr NodeNum sender = 0x12345678;
    constexpr PacketId packetId = 0xABCDEF01;
    constexpr NodeNum ourNode = 0x87654321;

    uint32_t delay = computeEmptyTopologyReplyDelayMs(sender, packetId, ourNode);
    TEST_ASSERT_GREATER_OR_EQUAL(EMPTY_TOPOLOGY_REPLY_DELAY_BASE_MS, delay);
    TEST_ASSERT_LESS_OR_EQUAL(EMPTY_TOPOLOGY_REPLY_DELAY_BASE_MS + EMPTY_TOPOLOGY_REPLY_DELAY_JITTER_MS - 1, delay);
    TEST_ASSERT_EQUAL_UINT32(delay, computeEmptyTopologyReplyDelayMs(sender, packetId, ourNode));
}

static void test_refresh_reported_direct_neighbor_updates_cache_and_variance()
{
    constexpr NodeNum localNode = 0xAAAAAAAA;
    constexpr NodeNum gateway = 0xBBBBBBBB;
    initGraphTestNodeDb(localNode);

    NeighborGraph graph;
    DirectNeighborSignal signals[NEIGHBOR_GRAPH_MAX_EDGES_PER_NODE] = {};
    uint8_t signalCount = 0;

    int first = refreshReportedDirectNeighborObservation(&graph, signals, signalCount, NEIGHBOR_GRAPH_MAX_EDGES_PER_NODE,
                                                         localNode, gateway, -80, 10.0f, 1000);
    TEST_ASSERT_EQUAL_INT(EDGE_NEW, first);

    const DirectNeighborSignal *initial = lookupDirectNeighborSignal(signals, signalCount, gateway);
    TEST_ASSERT_NOT_NULL(initial);
    TEST_ASSERT_EQUAL_INT8(-80, initial->rssi);
    TEST_ASSERT_EQUAL_INT8(10, initial->snr);

    const NodeEdges *myEdges = graph.getEdgesFrom(localNode);
    TEST_ASSERT_NOT_NULL(myEdges);
    uint8_t varianceBefore = 0;
    for (uint8_t i = 0; i < myEdges->edgeCount; i++) {
        if (myEdges->edges[i].to == gateway) {
            varianceBefore = myEdges->edges[i].etxVariance;
            break;
        }
    }

    int second = refreshReportedDirectNeighborObservation(&graph, signals, signalCount, NEIGHBOR_GRAPH_MAX_EDGES_PER_NODE,
                                                          localNode, gateway, -70, 12.0f, 1001);
    TEST_ASSERT_NOT_EQUAL(EDGE_NO_CHANGE, second);

    const DirectNeighborSignal *updated = lookupDirectNeighborSignal(signals, signalCount, gateway);
    TEST_ASSERT_NOT_NULL(updated);
    TEST_ASSERT_EQUAL_INT8(-70, updated->rssi);
    TEST_ASSERT_EQUAL_INT8(12, updated->snr);

    for (uint8_t i = 0; i < myEdges->edgeCount; i++) {
        if (myEdges->edges[i].to == gateway) {
            TEST_ASSERT_GREATER_THAN(varianceBefore, myEdges->edges[i].etxVariance);
            break;
        }
    }
}

static void test_relay_refresh_skips_without_reported_edge()
{
    constexpr NodeNum localNode = 0xCCCCCCCC;
    constexpr NodeNum directNeighbor = 0xDDDDDDDD;
    constexpr NodeNum placeholderGateway = 0xFF0000AB;
    initGraphTestNodeDb(localNode);

    NeighborGraph graph;
    DirectNeighborSignal signals[NEIGHBOR_GRAPH_MAX_EDGES_PER_NODE] = {};
    uint8_t signalCount = 0;

    refreshReportedDirectNeighborObservation(&graph, signals, signalCount, NEIGHBOR_GRAPH_MAX_EDGES_PER_NODE, localNode,
                                             directNeighbor, -80, 10.0f, 1000);

    TEST_ASSERT_FALSE(hasReportedDirectEdgeTo(&graph, localNode, placeholderGateway));

    if (hasReportedDirectEdgeTo(&graph, localNode, placeholderGateway)) {
        refreshReportedDirectNeighborObservation(&graph, signals, signalCount, NEIGHBOR_GRAPH_MAX_EDGES_PER_NODE,
                                                 localNode, placeholderGateway, -70, 12.0f, 1001);
    }

    const DirectNeighborSignal *direct = lookupDirectNeighborSignal(signals, signalCount, directNeighbor);
    TEST_ASSERT_NOT_NULL(direct);
    TEST_ASSERT_EQUAL_INT8(-80, direct->rssi);
    TEST_ASSERT_EQUAL_INT8(10, direct->snr);
}

static void test_mirrored_edge_update_does_not_upgrade_reported_edge()
{
    constexpr NodeNum localNode = 0xEEEEEEEE;
    constexpr NodeNum neighbor = 0x11112222;
    initGraphTestNodeDb(localNode);

    NeighborGraph graph;
    DirectNeighborSignal signals[NEIGHBOR_GRAPH_MAX_EDGES_PER_NODE] = {};
    uint8_t signalCount = 0;

    refreshReportedDirectNeighborObservation(&graph, signals, signalCount, NEIGHBOR_GRAPH_MAX_EDGES_PER_NODE, localNode,
                                             neighbor, -75, 8.0f, 2000);

    float mirroredEtx = NeighborGraph::calculateETX(-50, 15.0f);
    int mirroredChange = graph.updateEdge(localNode, neighbor, mirroredEtx, 2001, Edge::Source::Mirrored);
    TEST_ASSERT_EQUAL_INT(EDGE_NO_CHANGE, mirroredChange);

    const NodeEdges *myEdges = graph.getEdgesFrom(localNode);
    TEST_ASSERT_NOT_NULL(myEdges);
    for (uint8_t i = 0; i < myEdges->edgeCount; i++) {
        if (myEdges->edges[i].to == neighbor) {
            TEST_ASSERT_EQUAL(Edge::Source::Reported, myEdges->edges[i].source);
            break;
        }
    }
}

static void test_topology_listing_us_confirms_sender_hears_us()
{
    constexpr NodeNum localNode = 0x0A0B0C0D;
    constexpr NodeNum passiveSender = 0x22334455;
    constexpr NodeNum otherNode = 0x66778899;
    initGraphTestNodeDb(localNode);

    NeighborGraph graph;
    DirectNeighborSignal signals[NEIGHBOR_GRAPH_MAX_EDGES_PER_NODE] = {};
    uint8_t signalCount = 0;

    // No edge to the sender yet: nothing to confirm.
    TEST_ASSERT_FALSE(confirmTopologySenderHearsUs(&graph, localNode, passiveSender, localNode));

    refreshReportedDirectNeighborObservation(&graph, signals, signalCount, NEIGHBOR_GRAPH_MAX_EDGES_PER_NODE, localNode,
                                             passiveSender, -78, 9.0f, 3000);

    auto hearsUsOnEdgeToSender = [&]() {
        const NodeEdges *myEdges = graph.getEdgesFrom(localNode);
        TEST_ASSERT_NOT_NULL(myEdges);
        for (uint8_t i = 0; i < myEdges->edgeCount; i++) {
            if (myEdges->edges[i].to == passiveSender) {
                return myEdges->edges[i].hearsUs;
            }
        }
        TEST_FAIL_MESSAGE("edge to sender missing");
        return false;
    };

    TEST_ASSERT_FALSE(hearsUsOnEdgeToSender());

    // Sender listing some other node says nothing about us.
    TEST_ASSERT_FALSE(confirmTopologySenderHearsUs(&graph, localNode, passiveSender, otherNode));
    TEST_ASSERT_FALSE(hearsUsOnEdgeToSender());

    // Sender listing us proves it hears us: flag transitions once, then stays set.
    TEST_ASSERT_TRUE(confirmTopologySenderHearsUs(&graph, localNode, passiveSender, localNode));
    TEST_ASSERT_TRUE(hearsUsOnEdgeToSender());
    TEST_ASSERT_FALSE(confirmTopologySenderHearsUs(&graph, localNode, passiveSender, localNode));
    TEST_ASSERT_TRUE(hearsUsOnEdgeToSender());
}

static void test_topology_listing_peer_confirms_peer_hears_sender()
{
    constexpr NodeNum me = 0x0A0B0C0D;
    constexpr NodeNum peer = 0x11111111;
    constexpr NodeNum passive = 0x22222222;
    constexpr NodeNum stranger = 0x33333333;
    initGraphTestNodeDb(me);

    NeighborGraph graph;
    graph.updateEdge(me, peer, 1.0f, 1000, Edge::Source::Reported);
    graph.updateEdge(me, passive, 1.0f, 1000, Edge::Source::Reported);
    // The peer reported the passive node before the passive node listed it.
    graph.updateEdge(peer, passive, 1.2f, 1000, Edge::Source::Mirrored);

    auto peerEdgeHearsUs = [&]() {
        const NodeEdges *edges = graph.getEdgesFrom(peer);
        TEST_ASSERT_NOT_NULL(edges);
        for (uint8_t i = 0; i < edges->edgeCount; i++) {
            if (edges->edges[i].to == passive) {
                return edges->edges[i].hearsUs;
            }
        }
        TEST_FAIL_MESSAGE("peer edge to passive missing");
        return false;
    };
    TEST_ASSERT_FALSE(peerEdgeHearsUs());

    // The passive node lists the peer: the peer's edge to it is now known heard.
    TEST_ASSERT_TRUE(confirmTopologySenderHearsNeighbor(&graph, passive, peer));
    TEST_ASSERT_TRUE(peerEdgeHearsUs());
    TEST_ASSERT_FALSE(confirmTopologySenderHearsNeighbor(&graph, passive, peer));

    // A listed node without an edge to the sender gets nothing invented.
    TEST_ASSERT_FALSE(confirmTopologySenderHearsNeighbor(&graph, passive, stranger));
    TEST_ASSERT_NULL(graph.getEdgesFrom(stranger));
}

// An edge says who hears whom in one direction only; forwarding needs the other one. A
// topology-publishing destination that never confirmed the relay is unreachable through it.
static void test_route_never_uses_a_one_way_edge()
{
    constexpr NodeNum me = 0x0A0B0C0D;
    constexpr NodeNum relay = 0x11111111;
    constexpr NodeNum dest = 0x22222222;
    initGraphTestNodeDb(me);

    NeighborGraph graph;
    graph.updateEdge(me, relay, 1.0f, 1000, Edge::Source::Reported);
    graph.setEdgeHearsUs(me, relay, true);
    // The relay hears the destination; nothing says the destination hears the relay.
    graph.updateEdge(relay, dest, 4.0f, 1000, Edge::Source::Mirrored);

    NeighborGraph::RoutePolicy publishes;
    publishes.publishes = [](void *, NodeNum) { return true; };
    graph.clearCache();
    // No confirmed path: the inbound-gateway fallback still tries through the relay, penalised.
    Route fallback = graph.calculateRoute(dest, 1000, publishes);
    TEST_ASSERT_EQUAL_UINT32(relay, fallback.nextHop);
    TEST_ASSERT_FALSE(fallback.verified);
    TEST_ASSERT_EQUAL_UINT16(100 + 400 * UNVERIFIED_HOP_COST_FACTOR, fallback.costFixed);
    // A destination that publishes no topology cannot be ruled out.
    NodeNum destId = dest;
    NeighborGraph::RoutePolicy stockDest;
    stockDest.ctx = &destId;
    stockDest.publishes = [](void *c, NodeNum n) { return n != *static_cast<NodeNum *>(c); };
    graph.clearCache();
    TEST_ASSERT_EQUAL_UINT32(relay, graph.calculateRoute(dest, 1000, stockDest).nextHop);
    // The destination confirms it hears the relay: the route is verified again.
    graph.setEdgeHearsUs(relay, dest, true);
    graph.clearCache();
    Route verified = graph.calculateRoute(dest, 1000, publishes);
    TEST_ASSERT_EQUAL_UINT32(relay, verified.nextHop);
    TEST_ASSERT_TRUE(verified.verified);
    // Our own direct link is judged the same way.
    graph.updateEdge(me, dest, 1.0f, 1000, Edge::Source::Reported);
    graph.clearCache();
    TEST_ASSERT_EQUAL_UINT32(relay, graph.calculateRoute(dest, 1000, publishes).nextHop);
    graph.setEdgeHearsUs(me, dest, true);
    graph.clearCache();
    TEST_ASSERT_EQUAL_UINT32(dest, graph.calculateRoute(dest, 1000, publishes).nextHop);
}

// Costs are what the receiver of each hop measured: the relay hears us at ETX 3 and the
// destination hears the relay at ETX 2, however good the relay's signal looks to us.
static void test_route_cost_is_measured_at_the_receiver()
{
    constexpr NodeNum me = 0x0A0B0C0D;
    constexpr NodeNum relay = 0x11111111;
    constexpr NodeNum dest = 0x22222222;
    initGraphTestNodeDb(me);

    NeighborGraph graph;
    graph.updateEdge(me, relay, 1.0f, 1000, Edge::Source::Reported);
    graph.updateEdge(relay, me, 3.0f, 1000, Edge::Source::Mirrored);
    graph.updateEdge(relay, dest, 1.0f, 1000, Edge::Source::Mirrored);
    graph.updateEdge(dest, relay, 2.0f, 1000, Edge::Source::Mirrored);

    NeighborGraph::RoutePolicy publishes;
    publishes.publishes = [](void *, NodeNum) { return true; };
    Route route = graph.calculateRoute(dest, 1000, publishes);
    TEST_ASSERT_EQUAL_UINT32(relay, route.nextHop);
    TEST_ASSERT_EQUAL_UINT16(500, route.costFixed);
    TEST_ASSERT_EQUAL_UINT8(2, route.hops);
}

// A confirmed path wins whenever one exists, however long; without one the node that hears the
// far side carries the frame out, and passive nodes never do.
static void test_inbound_gateway_is_the_fallback_only_without_a_confirmed_path()
{
    constexpr NodeNum me = 0x0A0B0C0D;
    constexpr NodeNum gateway = 0x11111111;
    constexpr NodeNum passive = 0x22222222;
    constexpr NodeNum hub = 0x33333333;
    constexpr NodeNum far = 0x44444444;
    initGraphTestNodeDb(me);

    NeighborGraph graph;
    graph.updateEdge(me, gateway, 1.0f, 1000, Edge::Source::Reported);
    graph.setEdgeHearsUs(me, gateway, true);
    graph.updateEdge(me, passive, 1.0f, 1000, Edge::Source::Reported);
    graph.setEdgeHearsUs(me, passive, true);
    // Both hear the hub; the hub confirms neither.
    graph.updateEdge(gateway, hub, 2.0f, 1000, Edge::Source::Mirrored);
    graph.updateEdge(passive, hub, 1.0f, 1000, Edge::Source::Mirrored);

    NodeNum passiveId = passive;
    NeighborGraph::RoutePolicy policy;
    policy.ctx = &passiveId;
    policy.routable = [](void *c, NodeNum n) { return n != *static_cast<NodeNum *>(c); };
    policy.publishes = [](void *, NodeNum) { return true; };
    graph.clearCache();
    Route route = graph.calculateRoute(hub, 1000, policy);
    TEST_ASSERT_EQUAL_UINT32(gateway, route.nextHop);
    TEST_ASSERT_FALSE(route.verified);
    TEST_ASSERT_EQUAL_UINT16(100 + 200 * UNVERIFIED_HOP_COST_FACTOR, route.costFixed);
    TEST_ASSERT_EQUAL_UINT8(2, route.hops);

    // A confirmed path three hops long beats the two-hop unconfirmed one.
    graph.updateEdge(gateway, far, 3.0f, 1000, Edge::Source::Mirrored);
    graph.setEdgeHearsUs(gateway, far, true);
    graph.updateEdge(hub, far, 3.0f, 1000, Edge::Source::Mirrored);
    graph.clearCache();
    route = graph.calculateRoute(hub, 1000, policy);
    TEST_ASSERT_TRUE(route.verified);
    TEST_ASSERT_EQUAL_UINT32(gateway, route.nextHop);
    TEST_ASSERT_EQUAL_UINT8(3, route.hops);
    TEST_ASSERT_EQUAL_UINT16(100 + 300 + 300, route.costFixed);
}

static void test_self_coverage_counts_only_reported_edges()
{
    constexpr NodeNum me = 0x0A0B0C0D;
    constexpr NodeNum peer = 0x11111111;
    constexpr NodeNum a = 0x22222222;
    constexpr NodeNum b = 0x33333333;
    constexpr NodeNum c = 0x44444444;
    initGraphTestNodeDb(me);

    NeighborGraph graph;
    // Our reported neighbours: the peer and a. b is only mirrored (it relayed us once).
    graph.updateEdge(me, peer, 1.0f, 1000, Edge::Source::Reported);
    graph.updateEdge(me, a, 1.5f, 1000, Edge::Source::Reported);
    graph.updateEdge(me, b, 1.5f, 1000, Edge::Source::Mirrored);
    // The peer's topology as we mirrored it: a, b and c.
    graph.updateEdge(peer, a, 1.5f, 1000, Edge::Source::Reported);
    graph.updateEdge(peer, b, 1.5f, 1000, Edge::Source::Mirrored);
    graph.updateEdge(peer, c, 1.5f, 1000, Edge::Source::Mirrored);
    // Coverage needs the delivery direction: each listed node confirmed hearing the lister.
    for (NodeNum n : {peer, a, b}) {
        graph.setEdgeHearsUs(me, n, true);
    }
    for (NodeNum n : {a, b, c}) {
        graph.setEdgeHearsUs(peer, n, true);
    }

    NodeNum out[NODE_SET_MAX];
    // Ranking ourselves: only what we report (and peers can see) counts.
    size_t n = graph.getCoverageIfRelays(me, out, NODE_SET_MAX, nullptr, 0, me);
    TEST_ASSERT_EQUAL_UINT32(2, n);
    for (size_t i = 0; i < n; i++) {
        TEST_ASSERT_NOT_EQUAL(b, out[i]);
    }
    // Ranking a peer from our mirrored view: every edge we hold for it counts.
    TEST_ASSERT_EQUAL_UINT32(3, graph.getCoverageIfRelays(peer, out, NODE_SET_MAX, nullptr, 0, me));
    // Without a self node the legacy behaviour is unchanged.
    TEST_ASSERT_EQUAL_UINT32(3, graph.getCoverageIfRelays(me, out, NODE_SET_MAX, nullptr, 0));

    // Through the ranking: the peer covers three nodes, we report two. The legacy count gave us a
    // phantom third (b) and let our cheaper edges win slot 0; the peer sees it the other way round.
    NodeSet candidates;
    candidates.insert(me);
    candidates.insert(peer);
    NodeSet covered;
    RelayCandidate best = graph.findBestRelayCandidate(candidates, covered, 1, 0x10, false, 0, me);
    TEST_ASSERT_EQUAL_UINT32(peer, best.nodeId);
    TEST_ASSERT_EQUAL_UINT32(3, best.coverageCount);
    RelayCandidate legacy = graph.findBestRelayCandidate(candidates, covered, 1, 0x10, false, 0);
    TEST_ASSERT_EQUAL_UINT32(me, legacy.nodeId);
}

// Coverage is evidenced delivery over a link that is not hopeless, priced at the receiver.
static void test_covers_requires_evidence_and_a_sound_link()
{
    constexpr NodeNum me = 0x0A0B0C0D;
    constexpr NodeNum peer = 0x11111111;
    constexpr NodeNum u = 0x22222222;
    initGraphTestNodeDb(me);

    NeighborGraph graph;
    graph.updateEdge(me, peer, 1.0f, 1000, Edge::Source::Reported);
    // The peer lists u: that only says the peer hears u.
    graph.updateEdge(peer, u, 1.5f, 1000, Edge::Source::Mirrored);
    // u publishes topology, so its silence about the peer counts against coverage.
    NeighborGraph::CoveragePolicy reports;
    reports.publishesTopology = [](void *, NodeNum) { return true; };
    TEST_ASSERT_FALSE(graph.knownToHear(peer, u));
    TEST_ASSERT_FALSE(graph.covers(peer, u, 7.0f, &reports));
    // A node that publishes nothing can never confirm anything, so the peer's own edge is all the
    // evidence there will ever be and it counts.
    TEST_ASSERT_TRUE(graph.covers(peer, u, 7.0f));

    // u confirmed hearing the peer.
    graph.setEdgeHearsUs(peer, u, true);
    TEST_ASSERT_TRUE(graph.knownToHear(peer, u));
    TEST_ASSERT_TRUE(graph.covers(peer, u, 7.0f, &reports));

    // Confirmed once, hopeless now: hearsUs is sticky, coverage is not.
    graph.updateEdge(peer, u, 40.0f, 1000, Edge::Source::Mirrored);
    TEST_ASSERT_FALSE(graph.covers(peer, u, 7.0f, &reports));
    TEST_ASSERT_TRUE(graph.covers(peer, u, 0.0f, &reports)); // no ceiling: evidence alone

    // u's own measurement of the peer is the delivery-direction cost and wins the pricing.
    graph.updateEdge(u, peer, 1.2f, 1000, Edge::Source::Mirrored);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 1.2f, graph.hopCost(peer, u));
    TEST_ASSERT_TRUE(graph.covers(peer, u, 7.0f, &reports));
}

// Delivery is optimistic only for nodes that publish no topology; coverage never is.
static void test_can_deliver_is_optimistic_only_for_silent_nodes()
{
    constexpr NodeNum me = 0x0A0B0C0D;
    constexpr NodeNum peer = 0x11111111;
    constexpr NodeNum u = 0x22222222;
    initGraphTestNodeDb(me);

    NeighborGraph graph;
    graph.updateEdge(me, peer, 1.0f, 1000, Edge::Source::Reported);
    graph.updateEdge(peer, u, 1.5f, 1000, Edge::Source::Mirrored);

    NeighborGraph::RoutePolicy publishes;
    publishes.publishes = [](void *, NodeNum) { return true; };
    NeighborGraph::RoutePolicy silent; // no predicate: nothing publishes
    TEST_ASSERT_FALSE(graph.canDeliver(peer, u, publishes));
    TEST_ASSERT_TRUE(graph.canDeliver(peer, u, silent));
    NeighborGraph::CoveragePolicy reports;
    reports.publishesTopology = [](void *, NodeNum) { return true; };
    TEST_ASSERT_FALSE(graph.covers(peer, u, 7.0f, &reports));

    graph.setEdgeHearsUs(peer, u, true);
    TEST_ASSERT_TRUE(graph.canDeliver(peer, u, publishes));
}

// A neighbour nobody can be shown to reach belongs to exactly one relayer: the one hearing it
// best, in buckets, with stock relay routers given way first and the node id as the tie-break.
static void test_a_publisher_has_no_owner()
{
    // A neighbour that publishes topology and omits a candidate has reported that the candidate
    // cannot reach it. That silence is evidence, so nobody owns it.
    constexpr NodeNum me = 0x0A0B0C0D;
    constexpr NodeNum target = 0x22222222;
    initGraphTestNodeDb(me);
    NeighborGraph graph;
    graph.updateEdge(me, target, 2.0f, 1000, Edge::Source::Reported);
    static bool targetReports = false;
    NeighborGraph::CoveragePolicy policy;
    policy.me = me;
    policy.meRelays = true;
    policy.poorLinkEtx = 7.0f;
    policy.publishesTopology = [](void *, NodeNum) { return targetReports; };
    TEST_ASSERT_EQUAL_UINT32(me, graph.coverageOwner(target, policy));
    targetReports = true;
    TEST_ASSERT_EQUAL_UINT32(0, graph.coverageOwner(target, policy));
}

void test_admits_coverage_credits_an_owned_neighbour()
{
    // Admission and absorb must credit the same set: a relay that owns a silent neighbour
    // carries it, even though covers() alone would refuse.
    constexpr NodeNum me = 0x0A0B0C0D;
    constexpr NodeNum peer = 0xEE0000EE;
    constexpr NodeNum silent = 0x22222222;
    initGraphTestNodeDb(me);
    NeighborGraph graph;
    graph.updateEdge(me, peer, 1.0f, 1000, Edge::Source::Reported);
    graph.setEdgeHearsUs(me, peer, true);
    graph.updateEdge(peer, silent, 1.0f, 1000, Edge::Source::Mirrored);
    static NodeNum peerId = peer;
    NeighborGraph::CoveragePolicy policy;
    policy.me = me;
    policy.meRelays = true;
    policy.poorLinkEtx = 7.0f;
    policy.isSrActive = [](void *, NodeNum n) { return n == peerId; };
    TEST_ASSERT_EQUAL_UINT32(peer, graph.coverageOwner(silent, policy));
    TEST_ASSERT_TRUE(graph.admitsCoverage(peer, silent, 7.0f, &policy));
}

void test_a_sticky_confirmation_behind_a_decayed_link_is_not_ours()
{
    // hearsUs is sticky: whose neighbour it is does not make it reachable.
    constexpr NodeNum me = 0x0A0B0C0D;
    constexpr NodeNum src = 0x33333333;
    constexpr NodeNum edge = 0xEE0000EE;
    initGraphTestNodeDb(me);
    NeighborGraph graph;
    graph.updateEdge(me, edge, 1.0f, 1000, Edge::Source::Reported);
    graph.setEdgeHearsUs(me, edge, true);
    NeighborGraph::CoveragePolicy policy;
    policy.me = me;
    policy.meRelays = true;
    policy.poorLinkEtx = 7.0f;
    NodeNum coveredBy[1] = {src};
    TEST_ASSERT_EQUAL_UINT32(edge, graph.uniqueCoverageNeighbor(me, coveredBy, 1, 7.0f, &policy));
    // The link decays to the heard-once sentinel; the confirmation stays.
    graph.updateEdge(me, edge, 40.0f, 2000, Edge::Source::Reported);
    graph.updateEdge(edge, me, 40.0f, 2000, Edge::Source::Reported);
    TEST_ASSERT_EQUAL_UINT32(0, graph.uniqueCoverageNeighbor(me, coveredBy, 1, 7.0f, &policy));
}

void test_the_coverage_ceiling_is_inclusive()
{
    // Exactly at the ceiling a link still counts, in coverage and in ownership alike.
    constexpr NodeNum me = 0x0A0B0C0D;
    constexpr NodeNum target = 0x22222222;
    initGraphTestNodeDb(me);
    NeighborGraph graph;
    graph.updateEdge(me, target, 7.0f, 1000, Edge::Source::Reported);
    NeighborGraph::CoveragePolicy policy;
    policy.me = me;
    policy.meRelays = true;
    policy.poorLinkEtx = 7.0f;
    TEST_ASSERT_TRUE(graph.covers(me, target, 7.0f, &policy));
    TEST_ASSERT_EQUAL_UINT32(me, graph.coverageOwner(target, policy));
    graph.updateEdge(me, target, 7.01f, 2000, Edge::Source::Reported);
    TEST_ASSERT_FALSE(graph.covers(me, target, 7.0f, &policy));
    TEST_ASSERT_EQUAL_UINT32(0, graph.coverageOwner(target, policy));
}

void test_ownership_stops_at_the_coverage_ceiling()
{
    // Ownership picks who carries a neighbour nobody can be shown to reach; it must not make an
    // unreachable neighbour look reachable. Over a link past the ceiling nobody owns it, or the
    // ranking credits unique coverage to a node that cannot deliver and hands it the first slot.
    constexpr NodeNum me = 0x0A0B0C0D;
    constexpr NodeNum nearPeer = 0xEE0000EE;
    constexpr NodeNum silent = 0x22222222;
    initGraphTestNodeDb(me);

    NeighborGraph graph;
    graph.updateEdge(me, nearPeer, 1.0f, 1000, Edge::Source::Reported);
    graph.setEdgeHearsUs(me, nearPeer, true);
    graph.updateEdge(me, silent, 40.0f, 1000, Edge::Source::Reported);
    graph.updateEdge(nearPeer, silent, 2.0f, 1000, Edge::Source::Mirrored);

    static NodeNum peerId = nearPeer;
    NeighborGraph::CoveragePolicy policy;
    policy.me = me;
    policy.meRelays = true;
    policy.poorLinkEtx = 7.0f;
    policy.isSrActive = [](void *, NodeNum n) { return n == peerId; };
    // The sound link owns it; our own heard-once sentinel does not qualify.
    TEST_ASSERT_EQUAL_UINT32(nearPeer, graph.coverageOwner(silent, policy));
    // Its link decays to the sentinel too: now nobody owns it.
    graph.updateEdge(nearPeer, silent, 40.0f, 1000, Edge::Source::Mirrored);
    TEST_ASSERT_EQUAL_UINT32(0, graph.coverageOwner(silent, policy));
}

void test_coverage_owner_is_the_best_link_then_the_lowest_id()
{
    constexpr NodeNum me = 0x0A0B0C0D;
    constexpr NodeNum nearPeer = 0xEE0000EE;
    constexpr NodeNum silent = 0x22222222;
    initGraphTestNodeDb(me);

    NeighborGraph graph;
    graph.updateEdge(me, nearPeer, 1.0f, 1000, Edge::Source::Reported);
    graph.setEdgeHearsUs(me, nearPeer, true);
    graph.updateEdge(me, silent, 3.0f, 1000, Edge::Source::Reported);
    graph.updateEdge(nearPeer, silent, 1.0f, 1000, Edge::Source::Mirrored);

    static NodeNum peerId = nearPeer;
    NeighborGraph::CoveragePolicy policy;
    policy.me = me;
    policy.meRelays = true;
    policy.poorLinkEtx = 7.0f; // the shipped ceiling, so this pins production behaviour
    policy.isSrActive = [](void *, NodeNum n) { return n == peerId; };
    // A bucket better wins, id notwithstanding.
    TEST_ASSERT_EQUAL_UINT32(nearPeer, graph.coverageOwner(silent, policy));
    // Same bucket: the lowest id decides.
    graph.updateEdge(nearPeer, silent, 3.0f, 1000, Edge::Source::Mirrored);
    TEST_ASSERT_EQUAL_UINT32(me, graph.coverageOwner(silent, policy));
    // A stock relay router is given way even with a worse link.
    graph.updateEdge(nearPeer, silent, 6.0f, 1000, Edge::Source::Mirrored);
    NeighborGraph::CoveragePolicy stockFirst = policy;
    stockFirst.isSrActive = nullptr;
    stockFirst.isStockRelayRouter = [](void *, NodeNum n) { return n == peerId; };
    TEST_ASSERT_EQUAL_UINT32(nearPeer, graph.coverageOwner(silent, stockFirst));
    // Our role does not relay: we cannot own it.
    NeighborGraph::CoveragePolicy passive = policy;
    passive.meRelays = false;
    passive.isSrActive = nullptr;
    TEST_ASSERT_EQUAL_UINT32(0, graph.coverageOwner(silent, passive));
}

static void test_unique_coverage_ignores_poor_links_and_peer_owned_stock_nodes()
{
    constexpr NodeNum me = 0x0A0B0C0D;
    constexpr NodeNum peer = 0x11111111;
    constexpr NodeNum u = 0x22222222;
    constexpr NodeNum mute = 0x33333333;
    initGraphTestNodeDb(me);

    NeighborGraph graph;
    graph.updateEdge(me, peer, 1.0f, 1000, Edge::Source::Reported);
    graph.updateEdge(me, u, 1.0f, 1000, Edge::Source::Reported);
    graph.updateEdge(me, mute, 1.0f, 1000, Edge::Source::Reported);
    // The peer reaches u only at ETX 40 (heard once, barely) and the mute node well. Both
    // confirmed hearing the peer, so only the cost separates them.
    graph.updateEdge(peer, u, 40.0f, 1000, Edge::Source::Mirrored);
    graph.updateEdge(peer, mute, 1.5f, 1000, Edge::Source::Mirrored);
    graph.setEdgeHearsUs(peer, u, true);
    graph.setEdgeHearsUs(peer, mute, true);
    for (NodeNum n : {peer, u, mute}) {
        graph.setEdgeHearsUs(me, n, true);
    }
    const NodeNum coveredBy[] = {peer};
    NeighborGraph::CoveragePolicy policy;
    policy.me = me;
    policy.meRelays = true;
    policy.poorLinkEtx = 7.0f;

    // The peer's ETX-40 edge does not cover u, so u is still ours.
    TEST_ASSERT_TRUE(graph.hasUniqueCoverage(me, coveredBy, 1, 7.0f, &policy));
    // Fix the peer's link to u: covered again.
    graph.updateEdge(peer, u, 1.5f, 1000, Edge::Source::Mirrored);
    TEST_ASSERT_FALSE(graph.hasUniqueCoverage(me, coveredBy, 1, 7.0f, &policy));
    // A neighbour nobody heard covering it is ours to cover.
    const NodeNum nobody[] = {u};
    TEST_ASSERT_TRUE(graph.hasUniqueCoverage(me, nobody, 1, 7.0f, &policy));
    // Unless our own link to it cannot deliver: whose it is does not make it reachable.
    graph.updateEdge(me, mute, 40.0f, 2000, Edge::Source::Reported);
    graph.updateEdge(mute, me, 40.0f, 2000, Edge::Source::Reported);
    const NodeNum allButMute[] = {u, peer};
    TEST_ASSERT_EQUAL_UINT32(0, graph.uniqueCoverageNeighbor(me, allButMute, 2, 7.0f, &policy));
}

static void test_ranking_costs_within_a_bucket_tie_on_node_id()
{
    constexpr NodeNum me = 0x0A0B0C0D; // lower id
    constexpr NodeNum peer = 0x11111111;
    constexpr NodeNum a = 0x22222222;
    initGraphTestNodeDb(me);

    NeighborGraph graph;
    graph.updateEdge(me, peer, 1.0f, 1000, Edge::Source::Reported);
    graph.updateEdge(me, a, 1.31f, 1000, Edge::Source::Reported);
    graph.updateEdge(peer, a, 1.18f, 1000, Edge::Source::Mirrored);
    graph.setEdgeHearsUs(me, peer, true);
    graph.setEdgeHearsUs(me, a, true);
    graph.setEdgeHearsUs(peer, a, true);

    NodeSet candidates;
    candidates.insert(me);
    candidates.insert(peer);
    NodeSet covered;
    covered.insert(peer); // the peer heard the packet; only `a` is left to cover
    // Equal unique coverage {a}; costs 1.31 vs 1.18 sit in the same half-ETX bucket, so the
    // packet-id parity decides: even id -> lower node id (me), odd id -> higher (peer).
    RelayCandidate even = graph.findBestRelayCandidate(candidates, covered, 1, 0x10, false, 0, me);
    TEST_ASSERT_EQUAL_UINT32(me, even.nodeId);
    RelayCandidate odd = graph.findBestRelayCandidate(candidates, covered, 1, 0x11, true, 0, me);
    TEST_ASSERT_EQUAL_UINT32(peer, odd.nodeId);
    // A full bucket apart (ETX cannot go below 1.0, so make ours worse), the cheaper link wins
    // regardless of parity.
    graph.updateEdge(me, a, 1.6f, 1000, Edge::Source::Reported);
    RelayCandidate cheaper = graph.findBestRelayCandidate(candidates, covered, 1, 0x10, false, 0, me);
    TEST_ASSERT_EQUAL_UINT32(peer, cheaper.nodeId);
}

} // namespace

void setUp(void) {}

void tearDown(void) {}

static void test_topology_version_window_is_forward_only_and_wraps()
{
    TEST_ASSERT_TRUE(srTopologyVersionInWindow(1, 0));     // first report from an unknown peer
    TEST_ASSERT_TRUE(srTopologyVersionInWindow(6, 5));     // next version
    TEST_ASSERT_TRUE(srTopologyVersionInWindow(5, 5));     // repeat / continuation chunk
    TEST_ASSERT_TRUE(srTopologyVersionInWindow(2, 250));   // wraparound
    TEST_ASSERT_TRUE(srTopologyVersionInWindow(132, 5));   // +127 still forward
    TEST_ASSERT_FALSE(srTopologyVersionInWindow(4, 5));    // backwards
    TEST_ASSERT_FALSE(srTopologyVersionInWindow(133, 5));  // +128 reads as backwards
    TEST_ASSERT_FALSE(srTopologyVersionInWindow(1, 98));   // rebooted peer: needs boot reset or silence rule
}

static void test_topology_header_chunk_flags_round_trip()
{
    uint8_t buf[PACKED_NEIGHBOR_HEADER_SIZE];
    PackedHeader h = {};
    writePackedTopologyHeader(buf, 9, true);
    TEST_ASSERT_EQUAL_UINT8(0, decodePackedNeighbors(buf, sizeof(buf), nullptr, 0, &h));
    TEST_ASSERT_EQUAL_UINT8(9, h.topologyVersion);
    TEST_ASSERT_TRUE(h.signalRoutingActive);
    TEST_ASSERT_TRUE(h.isCompleteList());

    writePackedTopologyHeader(buf, 9, false, true, false); // first of several chunks
    decodePackedNeighbors(buf, sizeof(buf), nullptr, 0, &h);
    TEST_ASSERT_FALSE(h.signalRoutingActive);
    TEST_ASSERT_TRUE(h.moreChunks);
    TEST_ASSERT_FALSE(h.continuation);
    TEST_ASSERT_FALSE(h.isCompleteList());

    writePackedTopologyHeader(buf, 9, true, false, true); // last chunk
    decodePackedNeighbors(buf, sizeof(buf), nullptr, 0, &h);
    TEST_ASSERT_FALSE(h.moreChunks);
    TEST_ASSERT_TRUE(h.continuation);
    TEST_ASSERT_FALSE(h.isCompleteList());
}

static void test_topology_version_verdict_rules()
{
    const uint32_t resync = 1200000; // 2 x 600 s
    // First contact accepts any version, including one past the 128 window (FCM6 was at 148).
    TEST_ASSERT_EQUAL(SrTopologyVerdict::FirstContact, srTopologyVersionVerdict(148, 0, 0, 5000, resync, false));
    TEST_ASSERT_EQUAL(SrTopologyVerdict::FirstContact, srTopologyVersionVerdict(0, 0, 0, 5000, resync, true));
    // Normal forward moves and repeats.
    TEST_ASSERT_EQUAL(SrTopologyVerdict::Accept, srTopologyVersionVerdict(149, 148, 5000, 6000, resync, false));
    TEST_ASSERT_EQUAL(SrTopologyVerdict::Accept, srTopologyVersionVerdict(148, 148, 5000, 6000, resync, false));
    TEST_ASSERT_EQUAL(SrTopologyVerdict::Accept, srTopologyVersionVerdict(3, 250, 5000, 6000, resync, false));
    // Backwards is stale while the peer keeps talking...
    TEST_ASSERT_EQUAL(SrTopologyVerdict::Stale, srTopologyVersionVerdict(1, 26, 5000, 6000, resync, false));
    TEST_ASSERT_EQUAL(SrTopologyVerdict::Stale, srTopologyVersionVerdict(1, 26, 5000, 5000 + resync - 1, resync, false));
    // ...until two silent intervals, or its version-0 boot broadcast.
    TEST_ASSERT_EQUAL(SrTopologyVerdict::SilenceResync, srTopologyVersionVerdict(2, 26, 5000, 5000 + resync, resync, false));
    TEST_ASSERT_EQUAL(SrTopologyVerdict::BootReset, srTopologyVersionVerdict(0, 26, 5000, 6000, resync, true));
    // A header-only version-0 report without the boot flag semantics is just backwards.
    TEST_ASSERT_EQUAL(SrTopologyVerdict::Stale, srTopologyVersionVerdict(0, 26, 5000, 6000, resync, false));
    // millis() wrap: an accept just before the wrap is still recent after it.
    TEST_ASSERT_EQUAL(SrTopologyVerdict::Stale, srTopologyVersionVerdict(1, 26, 0xFFFFF000u, 1000, resync, false));
    // A lost boot broadcast: the second of two rejected versions climbing by one re-bases; a repeat
    // or a jump does not.
    TEST_ASSERT_EQUAL(SrTopologyVerdict::RestartClimb, srTopologyVersionVerdict(2, 13, 5000, 6000, resync, false, true, 1));
    TEST_ASSERT_EQUAL(SrTopologyVerdict::Stale, srTopologyVersionVerdict(1, 13, 5000, 6000, resync, false, true, 1));
    TEST_ASSERT_EQUAL(SrTopologyVerdict::Stale, srTopologyVersionVerdict(5, 13, 5000, 6000, resync, false, true, 1));
    TEST_ASSERT_EQUAL(SrTopologyVerdict::RestartClimb, srTopologyVersionVerdict(0, 13, 5000, 6000, resync, false, true, 255));
}

void setup()
{
    initializeTestEnvironment();

    UNITY_BEGIN();

    RUN_TEST(test_encode_decode_round_trip);
    RUN_TEST(test_packed_layout_offsets);
    RUN_TEST(test_merge_cost_from_decoded_signal);
    RUN_TEST(test_reject_v2_format_returns_zero);
    RUN_TEST(test_direct_signal_upsert_lookup_and_prune);
    RUN_TEST(test_empty_topology_reply_delay_range_and_determinism);
    RUN_TEST(test_refresh_reported_direct_neighbor_updates_cache_and_variance);
    RUN_TEST(test_relay_refresh_skips_without_reported_edge);
    RUN_TEST(test_mirrored_edge_update_does_not_upgrade_reported_edge);
    RUN_TEST(test_topology_listing_us_confirms_sender_hears_us);
    RUN_TEST(test_self_coverage_counts_only_reported_edges);
    RUN_TEST(test_route_never_uses_a_one_way_edge);
    RUN_TEST(test_route_cost_is_measured_at_the_receiver);
    RUN_TEST(test_inbound_gateway_is_the_fallback_only_without_a_confirmed_path);
    RUN_TEST(test_topology_listing_peer_confirms_peer_hears_sender);
    RUN_TEST(test_covers_requires_evidence_and_a_sound_link);
    RUN_TEST(test_can_deliver_is_optimistic_only_for_silent_nodes);
    RUN_TEST(test_coverage_owner_is_the_best_link_then_the_lowest_id);
    RUN_TEST(test_ownership_stops_at_the_coverage_ceiling);
    RUN_TEST(test_a_publisher_has_no_owner);
    RUN_TEST(test_admits_coverage_credits_an_owned_neighbour);
    RUN_TEST(test_a_sticky_confirmation_behind_a_decayed_link_is_not_ours);
    RUN_TEST(test_the_coverage_ceiling_is_inclusive);
    RUN_TEST(test_unique_coverage_ignores_poor_links_and_peer_owned_stock_nodes);
    RUN_TEST(test_ranking_costs_within_a_bucket_tie_on_node_id);
    RUN_TEST(test_topology_version_window_is_forward_only_and_wraps);
    RUN_TEST(test_topology_header_chunk_flags_round_trip);
    RUN_TEST(test_topology_version_verdict_rules);

    UNITY_END();
}

void loop() {}

#else

void setup() {}

void loop() {}

#endif // !MESHTASTIC_EXCLUDE_SIGNALROUTING
