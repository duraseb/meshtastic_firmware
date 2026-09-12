#include "TestUtil.h"
#include "mesh/NodeDB.h"
#include "mesh/graph/NeighborGraph.h"
#include "mesh/SignalRoutingModule.h"
#include <cmath>
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

    float etx = NeighborGraph::calculateETX(out[0].rssi, out[0].snr, meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST);
    float expected = NeighborGraph::calculateETX(rssi, snr, meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST);
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

    // -80/10 and -70/12 both clear LONG_FAST's decode threshold by a wide margin and saturate to
    // the same delivery probability under the margin-dominant curve, so they no longer pin a
    // variance change (the point of this test) — -90/-15 sits well below the margin curve's
    // saturation point, so moving to -70/12 is a real change under the new curve too.
    int first = refreshReportedDirectNeighborObservation(&graph, signals, signalCount, NEIGHBOR_GRAPH_MAX_EDGES_PER_NODE,
                                                         localNode, gateway, -90, -15.0f, 1000);
    TEST_ASSERT_EQUAL_INT(EDGE_NEW, first);

    const DirectNeighborSignal *initial = lookupDirectNeighborSignal(signals, signalCount, gateway);
    TEST_ASSERT_NOT_NULL(initial);
    TEST_ASSERT_EQUAL_INT8(-90, initial->rssi);
    TEST_ASSERT_EQUAL_INT8(-15, initial->snr);

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

    float mirroredEtx = NeighborGraph::calculateETX(-50, 15.0f, meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST);
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

void test_a_publisher_we_stopped_hearing_is_nobodys_target()
{
    // Maintenance retracts our own link to a silent publisher, but a peer's published edge to it
    // outlives that by up to a broadcast interval. Crediting the peer with covering it hands it a
    // slot it will decline, having retracted the node under the same rule.
    constexpr NodeNum me = 0x0A0B0C0D;
    constexpr NodeNum near = 0x11111111;
    constexpr NodeNum gone = 0x22222222;
    constexpr uint32_t silence = 1200;
    initGraphTestNodeDb(me);

    NeighborGraph graph;
    graph.updateEdge(me, near, 1.0f, 1000, Edge::Source::Reported);
    graph.setEdgeHearsUs(me, near, true);
    // The peer published a healthy link to the node, and the node published once itself.
    graph.updateEdge(near, gone, 2.0f, 1000, Edge::Source::Mirrored);
    graph.updateEdge(gone, near, 2.0f, 1000, Edge::Source::Mirrored);

    NeighborGraph::CoveragePolicy policy;
    policy.me = me;
    policy.meRelays = true;
    policy.poorLinkEtx = 7.0f;
    policy.publishesTopology = [](void *, NodeNum) { return true; };
    policy.publisherSilenceSecs = silence;

    // Inside the horizon the peer covers it.
    policy.nowSecs = 1000 + silence;
    TEST_ASSERT_FALSE(graph.isSilentPublisher(gone, policy));
    TEST_ASSERT_TRUE(graph.admitsCoverage(near, gone, 7.0f, &policy));

    // Past it, no peer covers it.
    policy.nowSecs = 1001 + silence;
    TEST_ASSERT_TRUE(graph.isSilentPublisher(gone, policy));
    TEST_ASSERT_FALSE(graph.admitsCoverage(near, gone, 7.0f, &policy));

    // Hearing it again restores it; the node's own timestamp is the evidence.
    graph.updateNodeActivity(gone, policy.nowSecs);
    TEST_ASSERT_FALSE(graph.isSilentPublisher(gone, policy));
    TEST_ASSERT_TRUE(graph.admitsCoverage(near, gone, 7.0f, &policy));
}

void test_routing_through_us_confirms_the_sender_hears_us()
{
    // A peer that names us as its next hop learned that from our traffic, so it hears us. Same
    // evidence as it listing us, but available a broadcast interval sooner.
    constexpr NodeNum me = 0x0A0B0C0D;
    constexpr NodeNum peer = 0x11111111;
    constexpr NodeNum stranger = 0x22222222;
    initGraphTestNodeDb(me);

    NeighborGraph graph;
    graph.updateEdge(me, peer, 1.0f, 1000, Edge::Source::Reported);

    auto hearsUs = [&](NodeNum n) {
        const NodeEdges *mine = graph.getEdgesFrom(me);
        if (!mine) return false;
        for (uint8_t i = 0; i < mine->edgeCount; i++) {
            if (mine->edges[i].to == n) return mine->edges[i].hearsUs;
        }
        return false;
    };

    TEST_ASSERT_FALSE(hearsUs(peer));
    TEST_ASSERT_TRUE(confirmSenderHearsUs(&graph, me, peer));
    TEST_ASSERT_TRUE(hearsUs(peer));
    // Idempotent: only the first tells us anything new.
    TEST_ASSERT_FALSE(confirmSenderHearsUs(&graph, me, peer));
    // Nothing is invented for a node we have no edge to, nor for ourselves.
    TEST_ASSERT_FALSE(confirmSenderHearsUs(&graph, me, stranger));
    TEST_ASSERT_FALSE(confirmSenderHearsUs(&graph, me, me));
    // The list path still routes through the same definition.
    TEST_ASSERT_FALSE(confirmTopologySenderHearsUs(&graph, me, peer, stranger));
}

void test_a_guess_never_outranks_or_prices_a_measurement()
{
    // An edge minted because a relayed frame crossed the link says a path exists and nothing
    // about what it costs, so it must not overwrite a published measurement, must not evict one,
    // and must not price coverage or ownership.
    constexpr NodeNum me = 0x0A0B0C0D;
    constexpr NodeNum gw = 0x11111111;
    constexpr NodeNum far = 0x22222222;
    initGraphTestNodeDb(me);

    NeighborGraph graph;
    graph.updateEdge(me, gw, 1.0f, 1000, Edge::Source::Reported);
    // The gateway published a hopeless link to the far node.
    graph.updateEdge(gw, far, 15.87f, 1000, Edge::Source::Mirrored);

    NeighborGraph::CoveragePolicy policy;
    policy.me = me;
    policy.meRelays = true;
    policy.poorLinkEtx = 7.0f;

    TEST_ASSERT_FALSE(graph.covers(gw, far, 7.0f, &policy));

    // One relayed frame across that link must change nothing.
    graph.updateEdge(gw, far, 1.57f, 2000, Edge::Source::Inferred);
    const NodeEdges *gwEdges = graph.getEdgesFrom(gw);
    TEST_ASSERT_NOT_NULL(gwEdges);
    const Edge *kept = nullptr;
    for (uint8_t i = 0; i < gwEdges->edgeCount; i++) {
        if (gwEdges->edges[i].to == far) kept = &gwEdges->edges[i];
    }
    TEST_ASSERT_NOT_NULL(kept);
    TEST_ASSERT_EQUAL_UINT16(1587, kept->etxFixed);
    TEST_ASSERT_TRUE(Edge::Source::Mirrored == kept->source);
    TEST_ASSERT_FALSE(graph.covers(gw, far, 7.0f, &policy));

    // A link known only as a guess is nobody's coverage and nobody's to own.
    constexpr NodeNum guessed = 0x33333333;
    graph.updateEdge(gw, guessed, 1.57f, 2000, Edge::Source::Inferred);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, graph.hopCost(gw, guessed));
    TEST_ASSERT_FALSE(graph.covers(gw, guessed, 7.0f, &policy));
    TEST_ASSERT_EQUAL_UINT32(0, graph.coverageOwner(guessed, policy));

    // The sender's complete list is the whole truth about its own edges.
    NodeNum listed[] = {far};
    TEST_ASSERT_TRUE(graph.retainListedEdges(gw, listed, 1));
    TEST_ASSERT_FALSE(graph.retainListedEdges(gw, listed, 1));
}

void test_a_silent_publisher_loses_our_direct_link()
{
    // A publisher promises a list every broadcast interval. Two missed intervals and our own
    // direct claim goes, so it stops drawing a relay out of us; the node stays reachable through
    // a peer that still hears it.
    constexpr NodeNum me = 0x0A0B0C0D;
    constexpr NodeNum pub = 0x11111111;
    constexpr NodeNum gw = 0x22222222;
    constexpr NodeNum stock = 0x33333333;
    constexpr uint32_t silence = 1200;
    initGraphTestNodeDb(me);

    NeighborGraph graph;
    graph.updateEdge(me, pub, 1.0f, 1000, Edge::Source::Reported);
    graph.updateEdge(pub, me, 1.0f, 1000, Edge::Source::Reported);
    graph.updateEdge(me, gw, 1.0f, 1000, Edge::Source::Reported);
    graph.updateEdge(gw, pub, 1.5f, 1000, Edge::Source::Mirrored);
    graph.updateEdge(me, stock, 1.0f, 1000, Edge::Source::Reported);

    auto hasEdge = [&](NodeNum from, NodeNum to) {
        const NodeEdges *edges = graph.getEdgesFrom(from);
        if (!edges) return false;
        for (uint8_t i = 0; i < edges->edgeCount; i++) {
            if (edges->edges[i].to == to) return true;
        }
        return false;
    };

    static NodeNum publisher = pub;
    NeighborGraph::CoveragePolicy policy;
    policy.me = me;
    policy.meRelays = true;
    policy.poorLinkEtx = 7.0f;
    policy.publishesTopology = [](void *, NodeNum n) { return n == publisher; };

    TEST_ASSERT_EQUAL_UINT8(0, graph.pruneSilentPublishers(me, 1000 + silence, silence, &policy));
    TEST_ASSERT_TRUE(hasEdge(me, pub));

    TEST_ASSERT_EQUAL_UINT8(1, graph.pruneSilentPublishers(me, 1001 + silence, silence, &policy));
    TEST_ASSERT_FALSE(hasEdge(me, pub));
    TEST_ASSERT_FALSE(hasEdge(pub, me));
    // The stock neighbour promises no cadence, so its silence proves nothing: it keeps the TTL.
    TEST_ASSERT_TRUE(hasEdge(me, stock));
    // The node itself and the peer's report of it survive.
    TEST_ASSERT_NOT_NULL(graph.getEdgesFrom(pub));
    TEST_ASSERT_TRUE(hasEdge(gw, pub));
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

/// An expired rung keeps the separation the ladder gave it, rather than redrawing at random.
///
/// Two nodes on adjacent rungs of the same ladder differ by a half-airtime; two on the same rung
/// differ only by their jitter. Both differences have to survive the expiry, because the failure
/// mode is not lateness — it is two nodes keying up at the same instant, where neither cancels the
/// other. Field 2026-09-08: two of the three unicast double relays completed 5 ms and 6 ms apart
/// with neither node logging the other's copy.
void test_expired_relay_keeps_its_ladder_separation()
{
    const uint32_t origin = 250;
    const uint32_t slot = 10;
    const uint32_t half = 50;

    // Adjacent rungs stay a half-airtime apart, and stay in order.
    uint32_t rung0 = srExpiredRelayReanchorMs(origin, origin, slot);
    uint32_t rung1 = srExpiredRelayReanchorMs(origin + half, origin, slot);
    uint32_t rung2 = srExpiredRelayReanchorMs(origin + 2 * half, origin, slot);
    TEST_ASSERT_EQUAL_UINT32(half, rung2 - rung1);
    // Rung 0 with no jitter at all is the one case the floor bites: it is lifted from zero to one
    // slot, so its gap to rung 1 is a half-airtime less that slot. Still ordered, still separated.
    TEST_ASSERT_EQUAL_UINT32(half - slot, rung1 - rung0);
    TEST_ASSERT_TRUE(rung0 < rung1 && rung1 < rung2);

    // Two nodes on the same rung keep their jitter apart.
    uint32_t jitterLow = srExpiredRelayReanchorMs(origin + 12, origin, slot);
    uint32_t jitterHigh = srExpiredRelayReanchorMs(origin + 24, origin, slot);
    TEST_ASSERT_EQUAL_UINT32(12, jitterHigh - jitterLow);

    // An expired rung 0 waits one slot rather than keying up instantly.
    TEST_ASSERT_EQUAL_UINT32(slot, rung0);
    TEST_ASSERT_EQUAL_UINT32(slot, srExpiredRelayReanchorMs(0, origin, slot));
    // A rung below the origin cannot go negative.
    TEST_ASSERT_EQUAL_UINT32(slot, srExpiredRelayReanchorMs(origin - 100, origin, slot));
    // No slot time known: still never zero.
    TEST_ASSERT_EQUAL_UINT32(1, srExpiredRelayReanchorMs(origin, origin, 0));
}

/// Unicast slot waits are floors on the same instant, so they compose by max. Adding the
/// destination's ACK wait on top of stock's contention floor delayed every relay on a confirmed
/// link by a whole contention window and bought nothing; folding the rung spacing into the max
/// instead of onto it drops two adjacent rungs onto one millisecond.
void test_unicast_slot_waits_are_floors_not_addends()
{
    const uint32_t floorMs = 160;   // 2 * CWmax * slotTime at the fleet preset
    const uint32_t ackWaitMs = 718; // turnaround + 2 * contention + reply airtime
    const uint32_t leaderWait = 494;
    const uint32_t half = 150;

    // Slot 0, nothing expected of the destination: stock's contention floor exactly.
    TEST_ASSERT_EQUAL_UINT32(floorMs, srUnicastSlotDelayMs(0, 0, floorMs, leaderWait, half));

    // Slot 0 with the destination expected to answer: the longer floor alone, never the sum.
    uint32_t earliest = ackWaitMs > floorMs ? ackWaitMs : floorMs;
    TEST_ASSERT_EQUAL_UINT32(ackWaitMs, srUnicastSlotDelayMs(0, 0, earliest, leaderWait, half));
    TEST_ASSERT_TRUE(srUnicastSlotDelayMs(0, 0, earliest, leaderWait, half) < floorMs + ackWaitMs);

    // Later rungs clear the leader first, and keep their spacing whichever floor dominates.
    uint32_t r1 = srUnicastSlotDelayMs(0, 1, floorMs, leaderWait, half);
    uint32_t r2 = srUnicastSlotDelayMs(0, 2, floorMs, leaderWait, half);
    TEST_ASSERT_EQUAL_UINT32(leaderWait, r1);
    TEST_ASSERT_EQUAL_UINT32(half, r2 - r1);
    uint32_t a1 = srUnicastSlotDelayMs(0, 1, earliest, leaderWait, half);
    uint32_t a2 = srUnicastSlotDelayMs(0, 2, earliest, leaderWait, half);
    TEST_ASSERT_EQUAL_UINT32(ackWaitMs, a1);
    TEST_ASSERT_EQUAL_UINT32(half, a2 - a1);

    // A designated next hop owns slot 0; ranked candidates queue behind its reservation and
    // still space by a half-airtime.
    const uint32_t reserved = 900;
    TEST_ASSERT_EQUAL_UINT32(reserved, srUnicastSlotDelayMs(reserved, 0, earliest, leaderWait, half));
    TEST_ASSERT_EQUAL_UINT32(reserved + half, srUnicastSlotDelayMs(reserved, 1, earliest, leaderWait, half));

    // No candidate ever keys up at zero: that was the state the contention floor exists to stop.
    TEST_ASSERT_TRUE(srUnicastSlotDelayMs(0, 0, floorMs, leaderWait, half) > 0);
    TEST_ASSERT_TRUE(srUnicastSlotDelayMs(0, 3, floorMs, leaderWait, half) > 0);
}

// The ranking inversion the recalibration exists to fix: at SHORT_SLOW (SF8, threshold -10 dB), a
// link heard at -104 dBm/-12 dB (margin -2, below threshold) must price above the ETX 7 coverage
// ceiling, while a link heard at -106 dBm/-5 dB (margin +5, above threshold) must clear it — even
// though the first has 2 dB more RSSI. Both figures are from a field capture on this fleet.
// (!046b553a and !ee594922's views of !94d4a83a).
void test_below_threshold_link_prices_above_the_coverage_ceiling_at_short_slow()
{
    float etx = NeighborGraph::calculateETX(-104, -12.0f, meshtastic_Config_LoRaConfig_ModemPreset_SHORT_SLOW);
    TEST_ASSERT_TRUE(etx > 7.0f);
}

void test_above_threshold_link_clears_the_coverage_ceiling_at_short_slow()
{
    float etx = NeighborGraph::calculateETX(-106, -5.0f, meshtastic_Config_LoRaConfig_ModemPreset_SHORT_SLOW);
    TEST_ASSERT_TRUE(etx <= 7.0f);
}

// The same pair at LONG_SLOW (SF12, threshold -20 dB), where both clear the threshold, are both
// usable — the recalibration should admit wrongly-excluded links, not just flip one comparison.
void test_both_links_are_usable_at_long_slow()
{
    float below = NeighborGraph::calculateETX(-104, -12.0f, meshtastic_Config_LoRaConfig_ModemPreset_LONG_SLOW);
    float above = NeighborGraph::calculateETX(-106, -5.0f, meshtastic_Config_LoRaConfig_ModemPreset_LONG_SLOW);
    TEST_ASSERT_TRUE(below <= 7.0f);
    TEST_ASSERT_TRUE(above <= 7.0f);
}

// +8 dB of margin or more is the saturation point; going further must not lower the ETX any more,
// at a fast preset (SF7, threshold -7.5) or a slow one (SF12, threshold -20).
void test_margin_saturates_at_the_top_regardless_of_preset()
{
    float atCap = NeighborGraph::calculateETX(-60, -7.5f + 8.0f, meshtastic_Config_LoRaConfig_ModemPreset_SHORT_FAST);
    float pastCap = NeighborGraph::calculateETX(-60, 20.0f, meshtastic_Config_LoRaConfig_ModemPreset_SHORT_FAST);
    TEST_ASSERT_EQUAL_FLOAT(atCap, pastCap);

    float atCapSlow = NeighborGraph::calculateETX(-60, -20.0f + 8.0f, meshtastic_Config_LoRaConfig_ModemPreset_LONG_SLOW);
    float pastCapSlow = NeighborGraph::calculateETX(-60, 20.0f, meshtastic_Config_LoRaConfig_ModemPreset_LONG_SLOW);
    TEST_ASSERT_EQUAL_FLOAT(atCapSlow, pastCapSlow);
}

// Deepest negative margin at the strongest RSSI reproduces the old curve's sentinel value; at the
// weakest RSSI it is a little higher, because RSSI is now a mild factor, not a veto.
void test_deep_below_threshold_hits_the_curves_floor()
{
    float strongRssi = NeighborGraph::calculateETX(-60, -100.0f, meshtastic_Config_LoRaConfig_ModemPreset_SHORT_SLOW);
    float weakRssi = NeighborGraph::calculateETX(-130, -100.0f, meshtastic_Config_LoRaConfig_ModemPreset_SHORT_SLOW);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 40.0f, strongRssi);
    TEST_ASSERT_TRUE(weakRssi > strongRssi);
    TEST_ASSERT_TRUE(weakRssi < 45.0f);
}

void test_non_finite_snr_prices_as_hopeless_not_perfect()
{
    float etxNan = NeighborGraph::calculateETX(-60, NAN, meshtastic_Config_LoRaConfig_ModemPreset_SHORT_SLOW);
    TEST_ASSERT_TRUE(etxNan > 7.0f);
    float etxInf = NeighborGraph::calculateETX(-60, INFINITY, meshtastic_Config_LoRaConfig_ModemPreset_SHORT_SLOW);
    TEST_ASSERT_TRUE(etxInf > 7.0f);
}

// A two-point sample (weak vs. strong) cannot see a kink between the endpoints — sweep the whole
// domain in both dimensions instead.
void test_curve_is_monotonic_in_rssi_and_snr()
{
    const meshtastic_Config_LoRaConfig_ModemPreset presets[] = {
        meshtastic_Config_LoRaConfig_ModemPreset_SHORT_FAST,
        meshtastic_Config_LoRaConfig_ModemPreset_SHORT_SLOW,
        meshtastic_Config_LoRaConfig_ModemPreset_LONG_SLOW,
    };
    for (auto preset : presets) {
        float previous = INFINITY;
        for (int32_t rssi = -140; rssi <= -20; rssi++) {
            float etx = NeighborGraph::calculateETX(rssi, 5.0f, preset);
            TEST_ASSERT_TRUE(etx <= previous + 1e-4f);
            previous = etx;
        }

        previous = INFINITY;
        for (int tenthsOfDb = -300; tenthsOfDb <= 300; tenthsOfDb += 5) { // -30.0..+30.0 dB, 0.5 dB steps
            float snr = static_cast<float>(tenthsOfDb) / 10.0f;
            float etx = NeighborGraph::calculateETX(-90, snr, preset);
            TEST_ASSERT_TRUE(etx <= previous + 1e-4f);
            previous = etx;
        }
    }
}

// etxToSignal has no production caller; this pins that it round-trips through
// calculateETX to within 10% and that it reports the fixed representative RSSI.
// This particular sample round-trips to within 2.5% — a sanity check that the inverse is wired up
// at all, not a claim about the function's worst case. See
// test_round_trip_worst_case_is_bounded below for the real bound.
void test_etx_to_signal_round_trips_within_ten_percent()
{
    int32_t rssi = 0;
    int32_t snr = 0;
    float etx = NeighborGraph::calculateETX(-75, 8.0f, meshtastic_Config_LoRaConfig_ModemPreset_SHORT_SLOW);
    NeighborGraph::etxToSignal(etx, meshtastic_Config_LoRaConfig_ModemPreset_SHORT_SLOW, rssi, snr);
    float again = NeighborGraph::calculateETX(rssi, static_cast<float>(snr), meshtastic_Config_LoRaConfig_ModemPreset_SHORT_SLOW);
    float relDiff = fabsf(again - etx) / etx;
    TEST_ASSERT_TRUE(relDiff < 0.10f);
    TEST_ASSERT_EQUAL_INT32(-60, rssi);
}

// etxToSignal reports SNR as an integer (the wire format's own type), so recovering it from a
// continuous margin loses up to 1 dB to truncation. Near the curve's steep transition that dB can
// swing the recomputed ETX far from the original — the sample above (2.5% drift) is not
// representative. Swept over ETX 1.0-50.0 in 0.01 steps at every preset whose threshold falls on a
// whole number of dB (SF8/10/12: SHORT_SLOW, MEDIUM_SLOW, LONG_SLOW), the true worst case is
// ~43.7% at ETX ~6.666. Pinned here, with headroom, so the 10% figure above is never mistaken for
// a universal bound.
void test_round_trip_worst_case_is_bounded()
{
    const meshtastic_Config_LoRaConfig_ModemPreset presets[] = {
        meshtastic_Config_LoRaConfig_ModemPreset_SHORT_SLOW,
        meshtastic_Config_LoRaConfig_ModemPreset_MEDIUM_SLOW,
        meshtastic_Config_LoRaConfig_ModemPreset_LONG_SLOW,
    };
    for (auto preset : presets) {
        float worst = 0.0f;
        for (int32_t hundredthsOfEtx = 100; hundredthsOfEtx <= 5000; hundredthsOfEtx++) {
            float etx = static_cast<float>(hundredthsOfEtx) / 100.0f;
            int32_t rssi = 0;
            int32_t snr = 0;
            NeighborGraph::etxToSignal(etx, preset, rssi, snr);
            float recomputed = NeighborGraph::calculateETX(rssi, static_cast<float>(snr), preset);
            float relDiff = fabsf(recomputed - etx) / etx;
            if (relDiff > worst) {
                worst = relDiff;
            }
        }
        TEST_ASSERT_TRUE(worst < 0.45f);
    }
}

// Mutation-tested pins. A reviewer mutated four single constants in this curve and found the
// existing suite (42/42) let every one through undetected. Each test below is designed, and was
// verified by hand, to fail under one specific mutation: apply it, run the suite, confirm the
// failure, then revert. Expected values are computed independently in dB/probability arithmetic
// (see the comment on each), not by re-deriving them from this module's own interpolation code —
// restating the implementation would not catch a wrong constant.

// SHORT_SLOW's threshold is -10 dB; SNR == -10 dB is exactly zero margin — the single most
// consequential point on the curve, because it is exactly where ETX crosses the 7.0 coverage
// ceiling. RSSI is pinned at the RSSI factor's saturating end (-60, >= the breakpoint) so the RSSI
// term is exactly 1.0 and cannot mask a change in the margin term. Expected:
// 1 / (marginProb[3] * 1.0) = 1 / 0.15 = 6.6667.
void test_margin_at_threshold_pins_the_zero_margin_probability()
{
    float etx = NeighborGraph::calculateETX(-60, -10.0f, meshtastic_Config_LoRaConfig_ModemPreset_SHORT_SLOW);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 6.6667f, etx);
}

// 1 dB below SHORT_SLOW's threshold (SNR -11.0, margin -1.0) interpolates between
// marginBreakDb[2] = -2 (prob 0.10) and marginBreakDb[3] = 0 (prob 0.15): prob =
// 0.10 + 0.5*(0.15-0.10) = 0.125, ETX = 8.0 exactly. Moving marginBreakDb[3] to -1.0 puts this same
// margin exactly on the (moved) breakpoint, collapsing the result to marginProb[3] = 0.15 and ETX
// 6.6667 instead — a large, easily-detected swing that the zero-margin test above cannot
// distinguish from a marginProb[3] mutation on its own.
void test_margin_one_db_below_threshold_pins_the_zero_margin_breakpoint()
{
    float etx = NeighborGraph::calculateETX(-60, -11.0f, meshtastic_Config_LoRaConfig_ModemPreset_SHORT_SLOW);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 8.0f, etx);
}

// SNR -15.0 at SHORT_SLOW is margin -5.0, exactly marginBreakDb[1]: prob = marginProb[1] = 0.05,
// ETX = 20.0 exactly. Isolated from the other three mutations above (touches only index 1).
void test_margin_five_db_below_threshold_pins_the_low_breakpoint_probability()
{
    float etx = NeighborGraph::calculateETX(-60, -15.0f, meshtastic_Config_LoRaConfig_ModemPreset_SHORT_SLOW);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 20.0f, etx);
}

// Margin pinned at the saturation point (SNR -2.0 at SHORT_SLOW is margin +8.0, so delivery
// probability is flat at marginProb[5] = 0.95) isolates the RSSI term. At RSSI -110 dBm, strictly
// between rssiFactorBreakDbm's -120 and -60: t = (-110 - (-120)) / 60 = 1/6,
// rssiFactor = 0.90 + (1/6)*0.10 = 0.91667, prob = 0.95 * 0.91667 = 0.87083,
// ETX = 1/0.87083 = 1.14833. Moving rssiFactorBreakDbm[0] from -120 to -100 puts -110
// at-or-below the new breakpoint, so the RSSI factor collapses to the flat 0.90 and ETX becomes
// 1.16959 instead.
void test_weak_rssi_at_saturated_margin_pins_the_rssi_floor_breakpoint()
{
    float etx = NeighborGraph::calculateETX(-110, -2.0f, meshtastic_Config_LoRaConfig_ModemPreset_SHORT_SLOW);
    TEST_ASSERT_FLOAT_WITHIN(0.0005f, 1.14833f, etx);
}

static void test_purge_for_preset_change_drops_neighbours_and_derived_state()
{
    constexpr NodeNum me = 0xAAAAAAAA;
    constexpr NodeNum peer = 0xBBBBBBBB;
    constexpr NodeNum far = 0xCCCCCCCC;
    initGraphTestNodeDb(me);

    NeighborGraph graph;
    graph.updateEdge(me, peer, 1.0f, 1000, Edge::Source::Reported);
    graph.updateDownstream(far, peer, 2.0f, 1000);
    graph.recordNodeTransmission(peer, 42, 1000);
    Route routed = graph.calculateRoute(peer, 1000);
    TEST_ASSERT_NOT_EQUAL(0, routed.nextHop);
    TEST_ASSERT_GREATER_THAN(0, graph.countDirectNeighbors());
    TEST_ASSERT_TRUE(graph.isDownstream(far));
    TEST_ASSERT_TRUE(graph.hasNodeTransmitted(peer, 42, 1000));
    TEST_ASSERT_NOT_EQUAL(0, graph.getCachedRoute(peer, 1000).nextHop);

    graph.purgeForPresetChange();

    TEST_ASSERT_EQUAL(0, graph.countDirectNeighbors());
    TEST_ASSERT_EQUAL_UINT32(0, graph.getNodeCount());
    TEST_ASSERT_FALSE(graph.isDownstream(far));
    TEST_ASSERT_FALSE(graph.hasNodeTransmitted(peer, 42, 2000));
    TEST_ASSERT_EQUAL_UINT32(0, graph.calculateRoute(peer, 2000).nextHop);
    TEST_ASSERT_EQUAL_UINT32(0, graph.getCachedRoute(peer, 2000).nextHop);
}

static void test_modem_preset_observer_ignores_first_sighting_and_repeats()
{
    bool known = false;
    uint8_t cached = 0;
    TEST_ASSERT_FALSE(srObserveModemPreset(known, cached, meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST));
    TEST_ASSERT_FALSE(srObserveModemPreset(known, cached, meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST));
    TEST_ASSERT_TRUE(srObserveModemPreset(known, cached, meshtastic_Config_LoRaConfig_ModemPreset_SHORT_SLOW));
    TEST_ASSERT_FALSE(srObserveModemPreset(known, cached, meshtastic_Config_LoRaConfig_ModemPreset_SHORT_SLOW));
    TEST_ASSERT_TRUE(srObserveModemPreset(known, cached, meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST));
}

static void test_preset_change_resets_topology_version_and_relay_identity()
{
    initGraphTestNodeDb(0xAAAAAAAA);
    SignalRoutingModule module;
    module.rememberRelayIdentity(0xBBBB00BB, 0xBB);
    TEST_ASSERT_GREATER_THAN(0, module.relayIdentityCacheSize());
    module.purgeGraphForPresetChange();
    TEST_ASSERT_EQUAL_UINT8(0, module.publishedTopologyVersion());
    TEST_ASSERT_TRUE(module.bootBroadcastPending());
    TEST_ASSERT_EQUAL_UINT8(0, module.relayIdentityCacheSize());
    TEST_ASSERT_EQUAL_UINT32(0, module.resolveRelayIdentity(0xBB));
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
    RUN_TEST(test_a_publisher_we_stopped_hearing_is_nobodys_target);
    RUN_TEST(test_routing_through_us_confirms_the_sender_hears_us);
    RUN_TEST(test_a_guess_never_outranks_or_prices_a_measurement);
    RUN_TEST(test_a_silent_publisher_loses_our_direct_link);
    RUN_TEST(test_admits_coverage_credits_an_owned_neighbour);
    RUN_TEST(test_a_sticky_confirmation_behind_a_decayed_link_is_not_ours);
    RUN_TEST(test_the_coverage_ceiling_is_inclusive);
    RUN_TEST(test_unique_coverage_ignores_poor_links_and_peer_owned_stock_nodes);
    RUN_TEST(test_ranking_costs_within_a_bucket_tie_on_node_id);
    RUN_TEST(test_topology_version_window_is_forward_only_and_wraps);
    RUN_TEST(test_topology_header_chunk_flags_round_trip);
    RUN_TEST(test_topology_version_verdict_rules);
    RUN_TEST(test_expired_relay_keeps_its_ladder_separation);
    RUN_TEST(test_unicast_slot_waits_are_floors_not_addends);
    RUN_TEST(test_below_threshold_link_prices_above_the_coverage_ceiling_at_short_slow);
    RUN_TEST(test_above_threshold_link_clears_the_coverage_ceiling_at_short_slow);
    RUN_TEST(test_both_links_are_usable_at_long_slow);
    RUN_TEST(test_margin_saturates_at_the_top_regardless_of_preset);
    RUN_TEST(test_deep_below_threshold_hits_the_curves_floor);
    RUN_TEST(test_non_finite_snr_prices_as_hopeless_not_perfect);
    RUN_TEST(test_curve_is_monotonic_in_rssi_and_snr);
    RUN_TEST(test_etx_to_signal_round_trips_within_ten_percent);
    RUN_TEST(test_round_trip_worst_case_is_bounded);
    RUN_TEST(test_margin_at_threshold_pins_the_zero_margin_probability);
    RUN_TEST(test_margin_one_db_below_threshold_pins_the_zero_margin_breakpoint);
    RUN_TEST(test_margin_five_db_below_threshold_pins_the_low_breakpoint_probability);
    RUN_TEST(test_weak_rssi_at_saturated_margin_pins_the_rssi_floor_breakpoint);
    RUN_TEST(test_purge_for_preset_change_drops_neighbours_and_derived_state);
    RUN_TEST(test_modem_preset_observer_ignores_first_sighting_and_repeats);
    RUN_TEST(test_preset_change_resets_topology_version_and_relay_identity);

    UNITY_END();
}

void loop() {}

#else

void setup() {}

void loop() {}

#endif // !MESHTASTIC_EXCLUDE_SIGNALROUTING
