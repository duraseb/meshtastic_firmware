#include "TestUtil.h"
#include "mesh/NodeDB.h"
#include "mesh/graph/HopHealth.h"
#include "mesh/graph/NeighborGraph.h"
#include "mesh/SignalRoutingModule.h"
#include <cmath>
#include <cstring>
#include <pb_encode.h>
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

static const Edge *edgeBetween(const NeighborGraph &graph, NodeNum from, NodeNum to)
{
    const NodeEdges *n = graph.getEdgesFrom(from);
    if (!n) {
        return nullptr;
    }
    for (uint8_t i = 0; i < n->edgeCount; i++) {
        if (n->edges[i].to == to) {
            return &n->edges[i];
        }
    }
    return nullptr;
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

    // The return value is the verdict on the direction we publish (us -> neighbour) and nothing
    // else. The dirty threshold is an absolute ETX delta, so an improvement past the bar is
    // significant the same way a degradation is. The cache and the variance below still move,
    // which is the rest of this function's job.
    int second = refreshReportedDirectNeighborObservation(&graph, signals, signalCount, NEIGHBOR_GRAPH_MAX_EDGES_PER_NODE,
                                                          localNode, gateway, -70, 12.0f, 1001);
    TEST_ASSERT_EQUAL_INT(EDGE_SIGNIFICANT_CHANGE, second);

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

    // A degradation does clear the threshold, and that is the signal the caller acts on. It comes
    // from the measured direction: the reverse edge is only inferred and never decides this.
    int third = refreshReportedDirectNeighborObservation(&graph, signals, signalCount, NEIGHBOR_GRAPH_MAX_EDGES_PER_NODE,
                                                         localNode, gateway, -120, -20.0f, 1002);
    TEST_ASSERT_EQUAL_INT(EDGE_SIGNIFICANT_CHANGE, third);

    // The direction we did not measure is held as an assumption of symmetry, so it must not carry
    // the class that outranks the neighbour's own published measurement of us.
    const NodeEdges *theirEdges = graph.getEdgesFrom(gateway);
    TEST_ASSERT_NOT_NULL(theirEdges);
    bool sawReverse = false;
    for (uint8_t i = 0; i < theirEdges->edgeCount; i++) {
        if (theirEdges->edges[i].to == localNode) {
            TEST_ASSERT_EQUAL_INT(static_cast<int>(Edge::Source::Inferred), static_cast<int>(theirEdges->edges[i].source));
            sawReverse = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(sawReverse);
}

static void test_a_single_rf_observation_survives_aging()
{
    constexpr NodeNum me = 0x0A0A0A0A;
    constexpr NodeNum peer = 0x0B0B0B0B;
    initGraphTestNodeDb(me);

    NeighborGraph graph;
    DirectNeighborSignal signals[NEIGHBOR_GRAPH_MAX_EDGES_PER_NODE] = {};
    uint8_t signalCount = 0;

    TEST_ASSERT_EQUAL_INT(EDGE_NEW, refreshReportedDirectNeighborObservation(
                                        &graph, signals, signalCount, NEIGHBOR_GRAPH_MAX_EDGES_PER_NODE, me, peer, -22, 12.0f,
                                        100));
    TEST_ASSERT_TRUE(hasReportedDirectEdgeTo(&graph, me, peer));

    const NodeEdges *theirs = graph.getEdgesFrom(peer);
    TEST_ASSERT_NOT_NULL(theirs);
    TEST_ASSERT_GREATER_THAN(0, theirs->edgeCount);

    graph.ageEdges(160, 5400);
    TEST_ASSERT_TRUE(hasReportedDirectEdgeTo(&graph, me, peer));
    TEST_ASSERT_NOT_NULL(lookupDirectNeighborSignal(signals, signalCount, peer));
}

static void test_age_edges_keeps_a_heard_neighbour_with_no_published_list()
{
    constexpr NodeNum me = 0x11111111;
    constexpr NodeNum peer = 0x22222222;
    initGraphTestNodeDb(me);
    NeighborGraph graph;

    TEST_ASSERT_EQUAL_INT(EDGE_NEW, graph.updateEdge(me, peer, 1.0f, 100, Edge::Source::Reported));
    TEST_ASSERT_TRUE(hasReportedDirectEdgeTo(&graph, me, peer));
    graph.ageEdges(160, 5400);
    TEST_ASSERT_TRUE(hasReportedDirectEdgeTo(&graph, me, peer));
}

static void test_age_edges_keeps_empty_l3_publisher_until_admission_ttl()
{
    // Parent→L3 measurements live on the parent; the L3 slot itself is often edgeless.
    // Field (angl 2026-10-07): purging those slots each tick collapsed the ball between hub dumps.
    constexpr NodeNum me = 0xAA0000AA;
    constexpr NodeNum l1 = 0xBB0000BB;
    constexpr NodeNum l2 = 0xCC0000CC;
    constexpr NodeNum l3 = 0xDD0000DD;
    initGraphTestNodeDb(me);
    NeighborGraph graph;
    const uint32_t t0 = 100;
    TEST_ASSERT_EQUAL_INT(EDGE_NEW, graph.updateEdge(me, l1, 1.0f, t0, Edge::Source::Reported));
    graph.setL1(l1);
    TEST_ASSERT_EQUAL_INT(EDGE_NEW, graph.updateEdge(l1, l2, 2.0f, t0, Edge::Source::Mirrored));
    graph.setEdgeHearsUs(l1, l2, true);
    TEST_ASSERT_TRUE(graph.ensurePublisher(l2, NodeClass::L2, l1, t0));
    TEST_ASSERT_EQUAL_INT(EDGE_NEW, graph.updateEdge(l2, l3, 2.0f, t0, Edge::Source::Mirrored));
    graph.setEdgeHearsUs(l2, l3, true);
    TEST_ASSERT_TRUE(graph.ensurePublisher(l3, NodeClass::L3, l2, t0));
    const NodeEdges *l3Node = graph.getEdgesFrom(l3);
    TEST_ASSERT_NOT_NULL(l3Node);
    TEST_ASSERT_EQUAL_UINT8(0, l3Node->edgeCount);

    graph.ageEdges(t0 + 60, 5400);
    TEST_ASSERT_NOT_NULL(graph.getEdgesFrom(l3));

    graph.ageEdges(t0 + 5401, 5400);
    TEST_ASSERT_NULL(graph.getEdgesFrom(l3));
}

static void test_an_improvement_larger_than_the_bar_is_significant()
{
    constexpr NodeNum me = 0xAAAAAAAA;
    constexpr NodeNum peer = 0xBBBBBBBB;
    initGraphTestNodeDb(me);
    NeighborGraph graph;

    TEST_ASSERT_EQUAL_INT(EDGE_NEW, graph.updateEdge(me, peer, 3.0f, 1000, Edge::Source::Reported));
    // Absolute drop of 1.0 ETX clears the 0.5 floor on a quiet edge.
    TEST_ASSERT_EQUAL_INT(EDGE_SIGNIFICANT_CHANGE,
                          graph.updateEdge(me, peer, 2.0f, 1001, Edge::Source::Reported));
}

static void test_a_degradation_larger_than_the_bar_is_significant()
{
    constexpr NodeNum me = 0xAAAAAAAA;
    constexpr NodeNum peer = 0xBBBBBBBB;
    initGraphTestNodeDb(me);
    NeighborGraph graph;

    TEST_ASSERT_EQUAL_INT(EDGE_NEW, graph.updateEdge(me, peer, 2.0f, 1000, Edge::Source::Reported));
    TEST_ASSERT_EQUAL_INT(EDGE_SIGNIFICANT_CHANGE,
                          graph.updateEdge(me, peer, 3.0f, 1001, Edge::Source::Reported));
}

static void test_a_change_just_above_half_is_significant_on_a_quiet_edge()
{
    // Pins that significance consults the variance already on the edge, not the value after
    // folding this observation in: with post-update EWMA a 0.55 jump would fail (0.5+0.1375).
    constexpr NodeNum me = 0xAAAAAAAA;
    constexpr NodeNum peer = 0xBBBBBBBB;
    initGraphTestNodeDb(me);
    NeighborGraph graph;

    graph.updateEdge(me, peer, 2.0f, 1000, Edge::Source::Reported);
    TEST_ASSERT_EQUAL_INT(EDGE_SIGNIFICANT_CHANGE,
                          graph.updateEdge(me, peer, 2.55f, 1001, Edge::Source::Reported));
}

static void test_a_change_smaller_than_the_bar_is_not_significant_either_way()
{
    constexpr NodeNum me = 0xAAAAAAAA;
    constexpr NodeNum peer = 0xBBBBBBBB;
    initGraphTestNodeDb(me);
    NeighborGraph graph;

    graph.updateEdge(me, peer, 2.0f, 1000, Edge::Source::Reported);
    TEST_ASSERT_EQUAL_INT(EDGE_NO_CHANGE, graph.updateEdge(me, peer, 2.3f, 1001, Edge::Source::Reported));
    TEST_ASSERT_EQUAL_INT(EDGE_NO_CHANGE, graph.updateEdge(me, peer, 2.0f, 1002, Edge::Source::Reported));
}

static void test_variance_raises_the_bar_so_a_noisy_edge_stops_reporting_small_jumps()
{
    constexpr NodeNum me = 0xAAAAAAAA;
    constexpr NodeNum quiet = 0xBBBBBBBB;
    constexpr NodeNum noisy = 0xCCCCCCCC;
    initGraphTestNodeDb(me);
    NeighborGraph graph;

    graph.updateEdge(me, quiet, 2.0f, 1000, Edge::Source::Reported);
    graph.updateEdge(me, noisy, 2.0f, 1000, Edge::Source::Reported);

    float etx = 2.0f;
    for (uint32_t i = 0; i < 8; i++) {
        etx = (i % 2 == 0) ? 7.0f : 2.0f;
        graph.updateEdge(me, noisy, etx, 2000 + i, Edge::Source::Reported);
    }
    if (etx != 2.0f) {
        graph.updateEdge(me, noisy, 2.0f, 3000, Edge::Source::Reported);
    }

    TEST_ASSERT_EQUAL_INT(EDGE_SIGNIFICANT_CHANGE,
                          graph.updateEdge(me, quiet, 3.0f, 4000, Edge::Source::Reported));
    TEST_ASSERT_EQUAL_INT(EDGE_NO_CHANGE,
                          graph.updateEdge(me, noisy, 3.0f, 4000, Edge::Source::Reported));
}

static void test_a_brand_new_edge_is_significant_unconditionally()
{
    constexpr NodeNum me = 0xAAAAAAAA;
    initGraphTestNodeDb(me);
    NeighborGraph graph;

    TEST_ASSERT_EQUAL_INT(EDGE_NEW, graph.updateEdge(me, 0xBBBBBBBB, 1.0f, 1000, Edge::Source::Reported));
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
    // Both hops are the sender's measurement of a publisher.
    TEST_ASSERT_EQUAL_UINT16((100 + 400) * UNVERIFIED_HOP_COST_FACTOR, fallback.costFixed);
    // A destination that publishes no topology cannot be ruled out.
    NodeNum destId = dest;
    NeighborGraph::RoutePolicy stockDest;
    stockDest.ctx = &destId;
    stockDest.publishes = [](void *c, NodeNum n) { return n != *static_cast<NodeNum *>(c); };
    graph.clearCache();
    TEST_ASSERT_EQUAL_UINT32(relay, graph.calculateRoute(dest, 1000, stockDest).nextHop);
    // hearsUs is the relay's claim, priced at the relay's SNR of the destination.
    graph.setEdgeHearsUs(relay, dest, true);
    graph.clearCache();
    Route claimed = graph.calculateRoute(dest, 1000, publishes);
    TEST_ASSERT_EQUAL_UINT32(relay, claimed.nextHop);
    TEST_ASSERT_FALSE(claimed.verified);
    TEST_ASSERT_EQUAL_UINT16((100 + 400) * UNVERIFIED_HOP_COST_FACTOR, claimed.costFixed);
    // The destination's own measurement, and the relay's measurement of us, verify the path.
    graph.updateEdge(relay, me, 1.0f, 1000, Edge::Source::Mirrored);
    graph.updateEdge(dest, relay, 2.5f, 1000, Edge::Source::Mirrored);
    graph.clearCache();
    Route verified = graph.calculateRoute(dest, 1000, publishes);
    TEST_ASSERT_EQUAL_UINT32(relay, verified.nextHop);
    TEST_ASSERT_TRUE(verified.verified);
    TEST_ASSERT_EQUAL_UINT16(100 + 250, verified.costFixed);
    // Our own edge to the destination, even with hearsUs, does not beat that verified relay
    // until the destination publishes a measurement of us.
    graph.updateEdge(me, dest, 1.0f, 1000, Edge::Source::Reported);
    graph.clearCache();
    TEST_ASSERT_EQUAL_UINT32(relay, graph.calculateRoute(dest, 1000, publishes).nextHop);
    graph.setEdgeHearsUs(me, dest, true);
    graph.clearCache();
    TEST_ASSERT_EQUAL_UINT32(relay, graph.calculateRoute(dest, 1000, publishes).nextHop);
    graph.updateEdge(dest, me, 1.0f, 1000, Edge::Source::Mirrored);
    graph.clearCache();
    TEST_ASSERT_EQUAL_UINT32(dest, graph.calculateRoute(dest, 1000, publishes).nextHop);
}

// Asymmetric L1: we hear A but A does not hear us. A hears B (L2); we reach B through C (L1).
// The ball stores that picture; the route to A is via C→B→A at receiver prices — not our one-way RX of A.
static void test_asymmetric_l1_routes_via_l2_hearer_not_our_rx()
{
    constexpr NodeNum me = 0xAA0000AA;
    constexpr NodeNum a = 0xA10000A1;
    constexpr NodeNum b = 0xB20000B2;
    constexpr NodeNum c = 0xC30000C3;
    initGraphTestNodeDb(me);

    NeighborGraph graph;
    const uint32_t t0 = 1000;
    // We hear A (L1); A never confirms us (no setEdgeHearsUs(me, a)).
    graph.updateEdge(me, a, 1.5f, t0, Edge::Source::Reported);
    // Path to B via C: we hear C; C lists B with hearsUs → B is L2.
    graph.updateEdge(me, c, 1.0f, t0, Edge::Source::Reported);
    graph.setEdgeHearsUs(me, c, true);
    graph.updateEdge(c, me, 1.0f, t0, Edge::Source::Mirrored);
    graph.updateEdge(c, b, 1.5f, t0, Edge::Source::Mirrored);
    graph.setEdgeHearsUs(c, b, true);
    TEST_ASSERT_TRUE(graph.ensurePublisher(b, NodeClass::L2, c, t0));
    // B's measurement of C prices C→B; A's measurement of B prices B→A.
    graph.updateEdge(b, c, 1.2f, t0, Edge::Source::Mirrored);
    graph.updateEdge(a, b, 2.0f, t0, Edge::Source::Mirrored);

    NeighborGraph::RoutePolicy publishes;
    publishes.publishes = [](void *, NodeNum) { return true; };

    TEST_ASSERT_NOT_NULL(graph.getEdgesFrom(me));
    TEST_ASSERT_NOT_NULL(graph.getEdgesFrom(a));
    TEST_ASSERT_TRUE(graph.getNodeClass(b) == NodeClass::L2);
    TEST_ASSERT_FALSE_MESSAGE(graph.canDeliver(me, a, publishes),
                              "one-way RX is not a delivery hop into a publishing A");
    TEST_ASSERT_TRUE(graph.canDeliver(b, a, publishes));
    TEST_ASSERT_TRUE(graph.canDeliver(c, b, publishes));

    graph.clearCache();
    Route route = graph.calculateRoute(a, t0, publishes);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(c, route.nextHop, "leave toward C, not A");
    TEST_ASSERT_TRUE_MESSAGE(route.verified, "receiver-priced path C→B→A is verified");
    TEST_ASSERT_EQUAL_UINT8(3, route.hops);
    // ME→C at C's 1.0 of ME + C→B at B's 1.2 of C + B→A at A's 2.0 of B.
    TEST_ASSERT_EQUAL_UINT16(100 + 120 + 200, route.costFixed);
    TEST_ASSERT_TRUE(route.costFixed != static_cast<uint16_t>(150 * UNVERIFIED_HOP_COST_FACTOR));
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

// Variance is EWMA(|ΔETX|)×20 and one unit adds 5 to the ETX×100 route cost. The noisy hop has
// the better mean, so it wins while both edges are quiet; once it swings, the stable hop is the
// one a stamped next hop should name.
static void test_variance_outranks_a_slightly_better_mean()
{
    constexpr NodeNum me = 0x0A0B0C0D;
    constexpr NodeNum stable = 0x11111111;
    constexpr NodeNum noisy = 0x33333333;
    constexpr NodeNum dest = 0x22222222;
    initGraphTestNodeDb(me);

    NeighborGraph graph;
    // Our own links first: a remote list is stored only once that node is already reachable.
    graph.updateEdge(me, noisy, 1.0f, 1000, Edge::Source::Reported);
    graph.updateEdge(me, stable, 1.0f, 1000, Edge::Source::Reported);
    graph.updateEdge(noisy, dest, 1.0f, 1000, Edge::Source::Mirrored);
    graph.updateEdge(stable, dest, 1.0f, 1000, Edge::Source::Mirrored);
    graph.updateEdge(dest, noisy, 1.0f, 1000, Edge::Source::Mirrored);
    graph.updateEdge(dest, stable, 1.5f, 1000, Edge::Source::Mirrored);
    graph.updateEdge(noisy, me, 1.0f, 1000, Edge::Source::Mirrored);
    graph.updateEdge(stable, me, 1.0f, 1000, Edge::Source::Mirrored);

    NeighborGraph::RoutePolicy publishes;
    publishes.publishes = [](void *, NodeNum) { return true; };
    graph.clearCache();
    Route quiet = graph.calculateRoute(dest, 1000, publishes);
    TEST_ASSERT_EQUAL_UINT32(noisy, quiet.nextHop);
    TEST_ASSERT_EQUAL_UINT16(200, quiet.costFixed);

    const NodeEdges *destEdges = graph.getEdgesFrom(dest);
    TEST_ASSERT_NOT_NULL(destEdges);
    bool stamped = false;
    for (uint8_t i = 0; i < destEdges->edgeCount; i++) {
        if (destEdges->edges[i].to == noisy) {
            const_cast<Edge &>(destEdges->edges[i]).etxVariance = 20;
            stamped = true;
        }
    }
    TEST_ASSERT_TRUE(stamped);
    graph.clearCache();
    Route swinging = graph.calculateRoute(dest, 1000, publishes);
    TEST_ASSERT_EQUAL_UINT32(stable, swinging.nextHop);
    TEST_ASSERT_EQUAL_UINT16(250, swinging.costFixed);
}

static void test_silence_variance_follows_age_bands()
{
    constexpr uint32_t T = NeighborGraph::TOPOLOGY_BROADCAST_SECS;
    TEST_ASSERT_EQUAL_UINT16(0, Edge::silenceEtxFixedFromAge(T / 2 - 1, T));
    TEST_ASSERT_EQUAL_UINT16(0, Edge::silenceEtxFixedFromAge(T / 2, T));
    const uint16_t slight = Edge::silenceEtxFixedFromAge(T / 2 + T / 8, T);
    TEST_ASSERT_TRUE(slight > 0 && slight < 50);
    const uint16_t mid = Edge::silenceEtxFixedFromAge(T + T / 2, T);
    TEST_ASSERT_EQUAL_UINT16(325, mid);
    TEST_ASSERT_TRUE(mid > slight);
    TEST_ASSERT_EQUAL_UINT16(1275, Edge::silenceEtxFixedFromAge(2 * T, T));
    TEST_ASSERT_EQUAL_UINT8(255, Edge::silenceVarianceByteFromAge(2 * T, T));
}

static void test_delivery_cost_rises_with_silence_on_our_rx_edge()
{
    constexpr NodeNum ME = 0xAA0000AA;
    constexpr NodeNum PEER = 0xBB0000BB;
    constexpr uint32_t T = NeighborGraph::TOPOLOGY_BROADCAST_SECS;
    initGraphTestNodeDb(ME);
    NeighborGraph graph;
    const uint32_t t0 = 5'000'000;
    graph.updateEdge(ME, PEER, 2.0f, t0, Edge::Source::Reported);
    Edge *rx = const_cast<Edge *>(edgeBetween(graph, ME, PEER));
    TEST_ASSERT_NOT_NULL(rx);
    rx->lastHeardSecs = t0;
    const float inside = graph.deliveryHopCost(PEER, ME, false, t0 + T / 4);
    const float loud = graph.deliveryHopCost(PEER, ME, false, t0 + T + 1);
    const float saturated = graph.deliveryHopCost(PEER, ME, false, t0 + 2 * T);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 2.0f, inside);
    TEST_ASSERT_TRUE(loud > inside);
    TEST_ASSERT_TRUE(saturated > loud);
}

static void test_last_heard_follows_the_on_air_transmitter()
{
    constexpr NodeNum ME = 0xAA0000AA;
    constexpr NodeNum PEER = 0xBB0000BB;
    constexpr NodeNum HUB = 0xCC0000CC;
    initGraphTestNodeDb(ME);
    NeighborGraph graph;
    DirectNeighborSignal signals[NEIGHBOR_GRAPH_MAX_EDGES_PER_NODE] = {};
    uint8_t signalCount = 0;
    refreshReportedDirectNeighborObservation(&graph, signals, signalCount, NEIGHBOR_GRAPH_MAX_EDGES_PER_NODE, ME, PEER,
                                             -70, 8.0f, 1000);
    refreshReportedDirectNeighborObservation(&graph, signals, signalCount, NEIGHBOR_GRAPH_MAX_EDGES_PER_NODE, ME, HUB, -72,
                                             7.0f, 2000);
    refreshReportedDirectNeighborObservation(&graph, signals, signalCount, NEIGHBOR_GRAPH_MAX_EDGES_PER_NODE, ME, HUB, -65,
                                             9.0f, 3000);
    const Edge *peer = edgeBetween(graph, ME, PEER);
    const Edge *hub = edgeBetween(graph, ME, HUB);
    TEST_ASSERT_NOT_NULL(peer);
    TEST_ASSERT_NOT_NULL(hub);
    TEST_ASSERT_EQUAL_UINT32(1000, peer->lastHeardSecs);
    TEST_ASSERT_EQUAL_UINT32(3000, hub->lastHeardSecs);
    TEST_ASSERT_NULL(edgeBetween(graph, ME, 0xDD0000DD));
}

static void test_in_window_hear_does_not_fold_silence_but_etx_jump_still_raises()
{
    constexpr NodeNum ME = 0xAA0000AA;
    constexpr NodeNum PEER = 0xBB0000BB;
    constexpr uint32_t T = NeighborGraph::TOPOLOGY_BROADCAST_SECS;
    initGraphTestNodeDb(ME);
    NeighborGraph graph;
    DirectNeighborSignal signals[NEIGHBOR_GRAPH_MAX_EDGES_PER_NODE] = {};
    uint8_t signalCount = 0;
    const uint32_t t0 = 4'000'000;
    refreshReportedDirectNeighborObservation(&graph, signals, signalCount, NEIGHBOR_GRAPH_MAX_EDGES_PER_NODE, ME, PEER,
                                             -70, 8.0f, t0);
    const uint8_t quiet = edgeBetween(graph, ME, PEER)->etxVariance;
    refreshReportedDirectNeighborObservation(&graph, signals, signalCount, NEIGHBOR_GRAPH_MAX_EDGES_PER_NODE, ME, PEER,
                                             -70, 8.0f, t0 + T / 4);
    TEST_ASSERT_EQUAL_UINT8(quiet, edgeBetween(graph, ME, PEER)->etxVariance);
    refreshReportedDirectNeighborObservation(&graph, signals, signalCount, NEIGHBOR_GRAPH_MAX_EDGES_PER_NODE, ME, PEER,
                                             -90, -15.0f, t0 + T / 4 + 1);
    TEST_ASSERT_GREATER_THAN(quiet, edgeBetween(graph, ME, PEER)->etxVariance);
}

static void test_silence_fold_keeps_scar_after_a_long_gap_packet()
{
    constexpr NodeNum ME = 0xAA0000AA;
    constexpr NodeNum PEER = 0xBB0000BB;
    constexpr uint32_t T = NeighborGraph::TOPOLOGY_BROADCAST_SECS;
    initGraphTestNodeDb(ME);
    NeighborGraph graph;
    DirectNeighborSignal signals[NEIGHBOR_GRAPH_MAX_EDGES_PER_NODE] = {};
    uint8_t signalCount = 0;
    const uint32_t t0 = 1'000'000;
    TEST_ASSERT_EQUAL_INT(EDGE_NEW, refreshReportedDirectNeighborObservation(
                                        &graph, signals, signalCount, NEIGHBOR_GRAPH_MAX_EDGES_PER_NODE, ME, PEER, -70,
                                        8.0f, t0));
    const float fresh = graph.deliveryHopCost(PEER, ME, false, t0);
    TEST_ASSERT_EQUAL_INT(EDGE_NO_CHANGE, refreshReportedDirectNeighborObservation(
                                              &graph, signals, signalCount, NEIGHBOR_GRAPH_MAX_EDGES_PER_NODE, ME, PEER,
                                              -70, 8.0f, t0 + 2 * T + 1));
    const float after = graph.deliveryHopCost(PEER, ME, false, t0 + 2 * T + 1);
    TEST_ASSERT_TRUE(after > fresh);
}

static void test_packed_variance_stays_stored_while_silence_is_live()
{
    constexpr NodeNum ME = 0xAA0000AA;
    constexpr NodeNum PEER = 0xBB0000BB;
    constexpr uint32_t T = NeighborGraph::TOPOLOGY_BROADCAST_SECS;
    initGraphTestNodeDb(ME);
    NeighborGraph graph;
    graph.updateEdge(ME, PEER, 2.0f, 1000, Edge::Source::Reported);
    Edge *rx = const_cast<Edge *>(edgeBetween(graph, ME, PEER));
    TEST_ASSERT_NOT_NULL(rx);
    rx->lastHeardSecs = 1000;
    const uint32_t silentAt = 1000 + 2 * T;
    TEST_ASSERT_EQUAL_UINT8(0, rx->etxVariance);
    TEST_ASSERT_GREATER_THAN(0, rx->effectiveVarianceByte(silentAt, T, true));
    uint8_t entry[PACKED_NEIGHBOR_ENTRY_SIZE] = {};
    encodePackedNeighborEntry(entry, PEER, -70, 8, false, false, rx->etxVariance);
    TEST_ASSERT_EQUAL_UINT8(rx->etxVariance, entry[7]);
    TEST_ASSERT_NOT_EQUAL(rx->effectiveVarianceByte(silentAt, T, true), entry[7]);
}

static void test_variance_outranks_a_slightly_better_mean_when_a_neighbour_is_silent()
{
    constexpr NodeNum ME = 0xAA0000AA;
    constexpr NodeNum QUIET = 0xBB0000BB;
    constexpr NodeNum NOISY = 0xCC0000CC;
    constexpr NodeNum DEST = 0xDD0000DD;
    constexpr uint32_t T = NeighborGraph::TOPOLOGY_BROADCAST_SECS;
    const uint32_t t0 = 10'000'000;
    initGraphTestNodeDb(ME);
    NeighborGraph graph;
    graph.updateEdge(ME, QUIET, 1.8f, t0, Edge::Source::Reported);
    graph.updateEdge(ME, NOISY, 2.0f, t0, Edge::Source::Reported);
    const_cast<Edge *>(edgeBetween(graph, ME, QUIET))->lastHeardSecs = t0;
    const_cast<Edge *>(edgeBetween(graph, ME, NOISY))->lastHeardSecs = t0 + T;
    graph.updateEdge(QUIET, DEST, 1.0f, t0, Edge::Source::Mirrored);
    graph.updateEdge(NOISY, DEST, 1.2f, t0, Edge::Source::Mirrored);
    graph.updateEdge(DEST, QUIET, 1.0f, t0, Edge::Source::Mirrored);
    graph.updateEdge(DEST, NOISY, 1.2f, t0, Edge::Source::Mirrored);
    graph.clearCache();
    Route route = graph.calculateRoute(DEST, t0 + 2 * T + 1);
    TEST_ASSERT_EQUAL_UINT32(NOISY, route.nextHop);
}

static void test_egress_silence_applies_only_after_we_have_heard_them()
{
    constexpr NodeNum ME = 0xAA0000AA;
    constexpr NodeNum HUB = 0xBB0000BB;
    constexpr NodeNum DEST = 0xCC0000CC;
    initGraphTestNodeDb(ME);
    NeighborGraph graph;
    graph.updateEdge(ME, HUB, 5.0f, 0, Edge::Source::Reported);
    graph.updateEdge(HUB, ME, 1.0f, 0, Edge::Source::Mirrored);
    graph.updateEdge(HUB, DEST, 1.0f, 0, Edge::Source::Mirrored);
    graph.updateEdge(DEST, HUB, 1.0f, 0, Edge::Source::Mirrored);
    NeighborGraph::RoutePolicy publishes;
    publishes.publishes = [](void *, NodeNum) { return true; };
    graph.clearCache();
    Route unheard = graph.calculateRoute(DEST, 0, publishes);
    TEST_ASSERT_EQUAL_UINT32(HUB, unheard.nextHop);
    TEST_ASSERT_EQUAL_UINT16(200, unheard.costFixed);
    const_cast<Edge *>(edgeBetween(graph, ME, HUB))->lastHeardSecs = 1;
    graph.clearCache();
    Route heard = graph.calculateRoute(DEST, 1, publishes);
    TEST_ASSERT_EQUAL_UINT32(HUB, heard.nextHop);
    TEST_ASSERT_TRUE(heard.costFixed > unheard.costFixed);
}

static void test_unverified_priced_hop_saturates_instead_of_wrapping()
{
    constexpr NodeNum ME = 0xAA0000AA;
    initGraphTestNodeDb(ME);
    NeighborGraph graph;
    Edge edge;
    edge.to = 0xBB0000BB;
    edge.etxFixed = 20000;
    edge.source = Edge::Source::Mirrored;
    TEST_ASSERT_EQUAL_UINT16(0xFFFE, graph.pricedHopCostFixed(edge, edge.to, ME, 0, true));
}

// Feed one neighbour list so the sender is recorded as an SR publisher and the edge exists.
static void fillTopoPacket(meshtastic_MeshPacket &mp, meshtastic_SignalRoutingInfo &info, NodeNum from, NodeNum neighbor,
                           bool hearsUs, uint32_t packetId, uint8_t hopStart, uint8_t hopLimit, uint8_t relayByte)
{
    uint8_t packed[32] = {};
    size_t packedLen = buildPackedBuffer(packed, sizeof(packed), neighbor, -90, 5, false, hearsUs, 0);
    info = meshtastic_SignalRoutingInfo_init_zero;
    info.packed_neighbors.size = packedLen;
    memcpy(info.packed_neighbors.bytes, packed, packedLen);

    uint8_t payload[96];
    pb_ostream_t stream = pb_ostream_from_buffer(payload, sizeof(payload));
    TEST_ASSERT_TRUE(pb_encode(&stream, &meshtastic_SignalRoutingInfo_msg, &info));

    mp = meshtastic_MeshPacket_init_zero;
    mp.from = from;
    mp.to = NODENUM_BROADCAST;
    mp.id = packetId;
    mp.hop_start = hopStart;
    mp.hop_limit = hopLimit;
    mp.relay_node = relayByte;
    mp.rx_rssi = -80;
    mp.rx_snr = 6;
    mp.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    mp.decoded.portnum = meshtastic_PortNum_SIGNAL_ROUTING_APP;
    mp.decoded.payload.size = stream.bytes_written;
    memcpy(mp.decoded.payload.bytes, payload, stream.bytes_written);
}

static void ingestOneNeighbor(SignalRoutingModule &module, NodeNum from, NodeNum neighbor, bool hearsUs, uint32_t packetId)
{
    meshtastic_MeshPacket mp = meshtastic_MeshPacket_init_zero;
    meshtastic_SignalRoutingInfo info = meshtastic_SignalRoutingInfo_init_zero;
    fillTopoPacket(mp, info, from, neighbor, hearsUs, packetId, 0, 0, static_cast<uint8_t>(from & 0xFF));
    module.preProcessSignalRoutingPacket(&mp);
}

// The confirmed route runs through a hop that hears the previous transmitter. That hop is only
// stampable once it also hears us; a backup with nothing else to name must stay silent.
static void test_a_backup_does_not_relay_when_its_next_hop_cannot_hear_it()
{
    constexpr NodeNum me = 0x0A0B0C0D;
    constexpr NodeNum previous = 0x63dc8f8c;
    constexpr NodeNum hop = 0x32aca541;
    constexpr NodeNum dest = 0xee594922;
    initGraphTestNodeDb(me);
    config.device.role = meshtastic_Config_DeviceConfig_Role_CLIENT;

    class GraphWriter : public SignalRoutingModule {
    public:
        void hear(NodeNum n, int32_t rssi, float snr) { updateNeighborInfo(n, rssi, snr, millis() / 1000); }
        void note(NodeNum from, NodeNum to, bool hearsUs) { updateGraphWithNeighbor(from, to, -90, 5, hearsUs); }
    };
    GraphWriter module;
    // RF-heard L1s (Reported). Mirrored-only me→hop is not an L1 under the horizon gate, so
    // topology from dest/previous would be refused or park hop as list-downstream.
    module.hear(hop, -90, 5.0f);
    module.hear(previous, -90, 5.0f);
    // Reachability before the published lists, or those lists are dropped.
    module.note(me, hop, false);
    module.note(hop, dest, true);
    module.note(me, previous, true);
    ingestOneNeighbor(module, dest, hop, true, 1);
    ingestOneNeighbor(module, previous, hop, true, 2);

    bool verified = true;
    NodeNum refused = module.getNextHop(dest, previous, previous, false, &verified);
    TEST_ASSERT_EQUAL_UINT32(me, refused);
    TEST_ASSERT_FALSE(verified);

    meshtastic_MeshPacket uni = meshtastic_MeshPacket_init_zero;
    uni.from = previous;
    uni.to = dest;
    uni.id = 0x839bbed0;
    uni.next_hop = static_cast<uint8_t>(hop & 0xFF);
    uni.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    TEST_ASSERT_FALSE(module.shouldRelayUnicastForCoordination(&uni));

    module.note(me, hop, true);
    verified = false;
    TEST_ASSERT_EQUAL_UINT32(hop, module.getNextHop(dest, previous, previous, false, &verified));
    TEST_ASSERT_TRUE(verified);
    // The packet already names that hop. Stamping it again is not another path.
    uni.hop_limit = 3;
    uni.hop_start = 3;
    TEST_ASSERT_FALSE(module.shouldRelayUnicastForCoordination(&uni));
    // A different designated hop, and our route names someone else: take the later slot.
    uni.next_hop = static_cast<uint8_t>(previous & 0xFF);
    uni.id = 0x839bbed1;
    TEST_ASSERT_TRUE(module.shouldRelayUnicastForCoordination(&uni));
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
    TEST_ASSERT_EQUAL_UINT16((100 + 200) * UNVERIFIED_HOP_COST_FACTOR, route.costFixed);
    TEST_ASSERT_EQUAL_UINT8(2, route.hops);

    // A confirmed path three hops long beats the two-hop unconfirmed one. Each hop is the
    // receiver's measurement; hearsUs on the reverse edge is not that measurement.
    graph.updateEdge(gateway, me, 1.0f, 1000, Edge::Source::Mirrored);
    graph.updateEdge(gateway, far, 3.0f, 1000, Edge::Source::Mirrored);
    graph.setEdgeHearsUs(gateway, far, true);
    graph.updateEdge(far, gateway, 3.0f, 1000, Edge::Source::Mirrored);
    graph.updateEdge(hub, far, 3.0f, 1000, Edge::Source::Mirrored);
    graph.clearCache();
    route = graph.calculateRoute(hub, 1000, policy);
    TEST_ASSERT_TRUE(route.verified);
    TEST_ASSERT_EQUAL_UINT32(gateway, route.nextHop);
    TEST_ASSERT_EQUAL_UINT8(3, route.hops);
    TEST_ASSERT_EQUAL_UINT16(100 + 300 + 300, route.costFixed);
}

static void test_self_coverage_is_who_hears_the_relay()
{
    constexpr NodeNum me = 0x0A0B0C0D;
    constexpr NodeNum peer = 0x11111111;
    constexpr NodeNum a = 0x22222222;
    constexpr NodeNum b = 0x33333333;
    constexpr NodeNum c = 0x44444444;
    initGraphTestNodeDb(me);

    NeighborGraph graph;
    graph.updateEdge(me, peer, 1.0f, 1000, Edge::Source::Reported);
    graph.updateEdge(me, a, 1.5f, 1000, Edge::Source::Reported);
    // b relayed us once: we have a Mirrored RX edge, not a listing from b.
    graph.updateEdge(me, b, 1.5f, 1000, Edge::Source::Mirrored);
    graph.updateEdge(peer, a, 1.5f, 1000, Edge::Source::Reported);
    graph.updateEdge(peer, b, 1.5f, 1000, Edge::Source::Mirrored);
    graph.updateEdge(peer, c, 1.5f, 1000, Edge::Source::Mirrored);
    for (NodeNum n : {peer, a}) {
        graph.setEdgeHearsUs(me, n, true);
    }
    for (NodeNum n : {a, b, c}) {
        graph.setEdgeHearsUs(peer, n, true);
    }

    NeighborGraph::CoveragePolicy policy;
    policy.publishesTopology = [](void *, NodeNum) { return true; };
    policy.poorLinkEtx = 7.0f;

    NodeNum out[NODE_SET_MAX];
    size_t n = graph.getCoverageIfRelays(me, out, NODE_SET_MAX, nullptr, 0, me, &policy);
    TEST_ASSERT_EQUAL_UINT32(2, n);
    for (size_t i = 0; i < n; i++) {
        TEST_ASSERT_NOT_EQUAL(b, out[i]);
        TEST_ASSERT_NOT_EQUAL(c, out[i]);
    }
    TEST_ASSERT_EQUAL_UINT32(3, graph.getCoverageIfRelays(peer, out, NODE_SET_MAX, nullptr, 0, me, &policy));

    NodeSet candidates;
    candidates.insert(me);
    candidates.insert(peer);
    NodeSet covered;
    RelayCandidate best = graph.findBestRelayCandidate(candidates, covered, 1, 0x10, false, 0, me, &policy);
    TEST_ASSERT_EQUAL_UINT32(peer, best.nodeId);
    TEST_ASSERT_EQUAL_UINT32(3, best.coverageCount);
}

static void test_a_candidates_coverage_is_who_listed_it()
{
    // Far's topology named the peer; the peer's own list never names far. Coverage of the peer
    // includes far because far can hear it. Coverage of us does not, and ranking follows that set.
    constexpr NodeNum me = 0x0A0B0C0D;
    constexpr NodeNum peer = 0x11111111;
    constexpr NodeNum far = 0x22222222;
    constexpr NodeNum shared = 0x33333333;
    initGraphTestNodeDb(me);

    NeighborGraph graph;
    graph.updateEdge(me, peer, 1.0f, 1000, Edge::Source::Reported);
    graph.updateEdge(me, shared, 1.0f, 1000, Edge::Source::Reported);
    graph.updateEdge(me, far, 1.0f, 1000, Edge::Source::Reported);
    graph.updateEdge(peer, shared, 1.0f, 1000, Edge::Source::Mirrored);
    graph.updateEdge(far, peer, 1.2f, 1000, Edge::Source::Mirrored);
    graph.setEdgeHearsUs(me, peer, true);
    graph.setEdgeHearsUs(me, shared, true);
    graph.setEdgeHearsUs(peer, shared, true);

    NeighborGraph::CoveragePolicy policy;
    policy.publishesTopology = [](void *, NodeNum) { return true; };
    policy.poorLinkEtx = 7.0f;

    NodeNum out[NODE_SET_MAX];
    size_t peerCov = graph.getCoverageIfRelays(peer, out, NODE_SET_MAX, nullptr, 0, me, &policy);
    bool peerCoversFar = false;
    for (size_t i = 0; i < peerCov; i++) {
        if (out[i] == far) {
            peerCoversFar = true;
        }
    }
    TEST_ASSERT_TRUE(peerCoversFar);

    size_t myCov = graph.getCoverageIfRelays(me, out, NODE_SET_MAX, nullptr, 0, me, &policy);
    for (size_t i = 0; i < myCov; i++) {
        TEST_ASSERT_NOT_EQUAL(far, out[i]);
    }

    NodeSet candidates;
    candidates.insert(me);
    candidates.insert(peer);
    NodeSet covered;
    covered.insert(shared);
    covered.insert(peer);
    RelayCandidate best = graph.findBestRelayCandidate(candidates, covered, 1, 0x10, false, 0, me, &policy);
    TEST_ASSERT_EQUAL_UINT32(peer, best.nodeId);
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
    // Hearing u mints an Inferred reverse. That is the symmetry guess, not u listing the peer.
    graph.updateEdge(u, peer, 1.5f, 1000, Edge::Source::Inferred);
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

static void test_unique_coverage_of_a_publisher_that_listed_us()
{
    // Delivery to a publisher is its list, not hearsUs on our RX edge. We heard the mute (so its
    // listing can be stored) but it has not been flagged; naming us is enough.
    constexpr NodeNum me = 0x0A0B0C0D;
    constexpr NodeNum mute = 0x33333333;
    initGraphTestNodeDb(me);

    NeighborGraph graph;
    graph.updateEdge(me, mute, 1.0f, 1000, Edge::Source::Reported);
    graph.updateEdge(mute, me, 1.0f, 1000, Edge::Source::Mirrored);

    NeighborGraph::CoveragePolicy policy;
    policy.me = me;
    policy.meRelays = true;
    policy.poorLinkEtx = 7.0f;
    policy.publishesTopology = [](void *, NodeNum n) { return n == mute; };

    TEST_ASSERT_TRUE(graph.knownToHear(me, mute));
    TEST_ASSERT_EQUAL_UINT32(mute, graph.uniqueCoverageNeighbor(me, nullptr, 0, 7.0f, &policy));
    NodeNum out[NODE_SET_MAX];
    TEST_ASSERT_EQUAL_UINT32(1, graph.getCoverageIfRelays(me, out, NODE_SET_MAX, nullptr, 0, me, &policy));
    TEST_ASSERT_EQUAL_UINT32(mute, out[0]);
}

static void test_unique_coverage_skips_a_publisher_that_did_not_list_us()
{
    constexpr NodeNum me = 0x0A0B0C0D;
    constexpr NodeNum mute = 0x33333333;
    initGraphTestNodeDb(me);

    NeighborGraph graph;
    graph.updateEdge(me, mute, 1.0f, 1000, Edge::Source::Reported);
    graph.updateEdge(mute, me, 1.0f, 1000, Edge::Source::Inferred);

    NeighborGraph::CoveragePolicy policy;
    policy.me = me;
    policy.meRelays = true;
    policy.poorLinkEtx = 7.0f;
    policy.publishesTopology = [](void *, NodeNum n) { return n == mute; };

    TEST_ASSERT_FALSE(graph.knownToHear(me, mute));
    TEST_ASSERT_EQUAL_UINT32(0, graph.coverageOwner(mute, policy));
    TEST_ASSERT_EQUAL_UINT32(0, graph.uniqueCoverageNeighbor(me, nullptr, 0, 7.0f, &policy));
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

// A neighbour we hear on RF must not be parked downstream of a peer that lists them.
static void test_topology_does_not_park_a_heard_neighbour_as_downstream()
{
    constexpr NodeNum me = 0xB781E8BC;
    constexpr NodeNum peer = 0xEE594922;
    constexpr NodeNum heard = 0x63DC8F8C;
    initGraphTestNodeDb(me);
    config.device.role = meshtastic_Config_DeviceConfig_Role_CLIENT;

    class Harness : public SignalRoutingModule {
    public:
        NeighborGraph *graph() { return routingGraph; }
    };
    Harness module;
    TEST_ASSERT_NOT_NULL(module.graph());

    module.updateNeighborInfo(peer, -70, 8.0f, 1000);
    module.updateNeighborInfo(heard, -75, 11.0f, 1000);
    // Stale row as if the old them→us check had parked them behind the peer.
    module.graph()->updateDownstream(heard, peer, 2.0f, millis() / 1000);
    TEST_ASSERT_EQUAL_UINT32(peer, module.graph()->getDownstreamRelay(heard));

    ingestOneNeighbor(module, peer, heard, true, 0xA11);
    TEST_ASSERT_FALSE(module.graph()->isDownstream(heard));
    TEST_ASSERT_EQUAL_UINT32(0, module.graph()->getDownstreamRelay(heard));
}

// A node we do not hear, listed with hearsUs, is recorded as list-downstream of the topology sender.
// (Module ingest of a synthetic protobuf is covered elsewhere; this pins the write the merge uses.)
static void test_topology_still_learns_downstream_for_nodes_we_do_not_hear()
{
    constexpr NodeNum me = 0xB781E8BC;
    constexpr NodeNum peer = 0xEE594922;
    constexpr NodeNum remote = 0x0D0E0F10;
    initGraphTestNodeDb(me);

    NeighborGraph graph;
    const uint32_t now = millis() / 1000;
    graph.updateEdge(me, peer, 1.0f, now, Edge::Source::Reported);
    // Same call the topology merge uses when !hasDirectConnection && hearsUs (past depth).
    graph.updateDownstreamListed(remote, peer, 2.0f, now, /*evenIfRelayHasEdge=*/true);
    TEST_ASSERT_TRUE(graph.isDownstream(remote));
    TEST_ASSERT_EQUAL_UINT32(peer, graph.getDownstreamRelay(remote));
    NodeNum listParent = 0;
    uint16_t listCost = 0;
    TEST_ASSERT_TRUE(graph.listDownstream(remote, listParent, listCost));
    TEST_ASSERT_EQUAL_UINT32(peer, listParent);

    // Hearing them later must drop that row (same clear the merge runs on HAS direct).
    graph.clearDownstreamForDestination(remote);
    TEST_ASSERT_FALSE(graph.isDownstream(remote));
}

// A neighbour we used to hear, now heard only through a relay, must leave our direct set
// immediately and sit behind that relay — otherwise unicasts keep aiming at a dead last hop.
static void test_a_relayed_former_neighbour_becomes_downstream()
{
    constexpr NodeNum me = 0x0A0B0C0D;
    constexpr NodeNum traveller = 0x11111111;
    constexpr NodeNum relay = 0x22222222;
    initGraphTestNodeDb(me);

    NeighborGraph graph;
    graph.updateEdge(me, traveller, 1.0f, 1000, Edge::Source::Reported);
    graph.updateEdge(traveller, me, 1.0f, 1000, Edge::Source::Mirrored);
    graph.setEdgeHearsUs(me, traveller, true);
    graph.updateEdge(me, relay, 1.0f, 1000, Edge::Source::Reported);
    graph.setEdgeHearsUs(me, relay, true);
    graph.updateEdge(relay, traveller, 1.2f, 1000, Edge::Source::Mirrored);
    graph.updateEdge(traveller, relay, 1.2f, 1000, Edge::Source::Mirrored);
    graph.setEdgeHearsUs(relay, traveller, true);

    NeighborGraph::RoutePolicy publishes;
    publishes.publishes = [](void *, NodeNum) { return true; };
    graph.clearCache();
    TEST_ASSERT_EQUAL_UINT32(traveller, graph.calculateRoute(traveller, 1000, publishes).nextHop);

    TEST_ASSERT_TRUE(graph.retractDirectLink(me, traveller));
    const uint32_t now = millis() / 1000;
    graph.updateDownstreamExclusive(traveller, relay, 1.5f, now, true);

    const NodeEdges *self = graph.getEdgesFrom(me);
    TEST_ASSERT_NOT_NULL(self);
    bool stillDirect = false;
    for (uint8_t i = 0; i < self->edgeCount; i++) {
        if (self->edges[i].to == traveller) {
            stillDirect = true;
            break;
        }
    }
    TEST_ASSERT_FALSE(stillDirect);
    TEST_ASSERT_TRUE(graph.isDownstream(traveller));
    TEST_ASSERT_EQUAL_UINT32(relay, graph.getDownstreamRelay(traveller));

    graph.clearCache();
    Route viaRelay = graph.calculateRoute(traveller, 2000, publishes);
    TEST_ASSERT_EQUAL_UINT32(relay, viaRelay.nextHop);

    graph.updateEdge(me, traveller, 1.0f, 3000, Edge::Source::Reported);
    graph.setEdgeHearsUs(me, traveller, true);
    graph.clearDownstreamForDestination(traveller);
    TEST_ASSERT_FALSE(graph.isDownstream(traveller));
    graph.clearCache();
    TEST_ASSERT_EQUAL_UINT32(traveller, graph.calculateRoute(traveller, 3000, publishes).nextHop);
}

static void test_chain_walks_to_the_first_hearable_hop()
{
    constexpr NodeNum me = 0xAA0000AA;
    constexpr NodeNum hub = 0xF60000F6;
    constexpr NodeNum parent = 0x11000011;
    constexpr NodeNum mid = 0x33000033;
    constexpr NodeNum dest = 0x22000022;
    initGraphTestNodeDb(me);

    NeighborGraph graph;
    const uint32_t now = millis() / 1000;
    graph.updateEdge(me, hub, 1.2f, now, Edge::Source::Reported);
    graph.updateDownstream(dest, mid, 1.5f, now);
    graph.updateDownstream(mid, parent, 1.5f, now);
    graph.updateDownstream(parent, hub, 2.0f, now);

    ChainEgress chain = graph.downstreamChainEgress(dest, me);
    TEST_ASSERT_EQUAL_UINT32(hub, chain.node);
    TEST_ASSERT_EQUAL_UINT8(3, chain.hops);
    TEST_ASSERT_EQUAL_UINT16(150 + 150 + 200, chain.costFixed);

    ChainEgress toParent = graph.downstreamChainEgress(parent, me);
    TEST_ASSERT_EQUAL_UINT32(hub, toParent.node);
    TEST_ASSERT_EQUAL_UINT8(1, toParent.hops);
    TEST_ASSERT_EQUAL_UINT16(200, toParent.costFixed);

    NeighborGraph atHub;
    initGraphTestNodeDb(hub);
    atHub.updateEdge(hub, parent, 1.4f, now, Edge::Source::Reported);
    atHub.updateDownstream(dest, mid, 1.5f, now);
    atHub.updateDownstream(mid, parent, 1.5f, now);
    ChainEgress hubAppoints = atHub.downstreamChainEgress(dest, hub);
    TEST_ASSERT_EQUAL_UINT32(parent, hubAppoints.node);
    TEST_ASSERT_EQUAL_UINT8(2, hubAppoints.hops);
    TEST_ASSERT_EQUAL_UINT16(150 + 150, hubAppoints.costFixed);
}

static void test_chain_returns_none_on_a_cycle_or_a_broken_path()
{
    constexpr NodeNum me = 0xAA0000AA;
    initGraphTestNodeDb(me);

    NeighborGraph cycle;
    const uint32_t now = millis() / 1000;
    cycle.updateDownstream(0x22000022, 0x11000011, 1.0f, now);
    cycle.updateDownstream(0x11000011, 0x22000022, 1.0f, now);
    TEST_ASSERT_EQUAL_UINT32(0, cycle.downstreamChainEgress(0x22000022, me).node);

    NeighborGraph broken;
    broken.updateDownstream(0x22000022, 0x11000011, 1.0f, now);
    TEST_ASSERT_EQUAL_UINT32(0, broken.downstreamChainEgress(0x22000022, me).node);
}

static void test_downstream_chain_appoints_the_neighbour_we_hear()
{
    constexpr NodeNum me = 0xAA0000AA;
    constexpr NodeNum hub = 0xF60000F6;
    constexpr NodeNum parent = 0x11000011;
    constexpr NodeNum dest = 0x22000022;
    const uint32_t now = millis() / 1000;
    initGraphTestNodeDb(me);

    NeighborGraph graph;
    graph.updateEdge(me, hub, 1.2f, now, Edge::Source::Reported);
    graph.updateDownstream(dest, parent, 2.0f, now);
    graph.updateDownstream(parent, hub, 2.0f, now);
    graph.clearCache();
    Route route = graph.calculateRoute(dest, now);
    TEST_ASSERT_EQUAL_UINT32(hub, route.nextHop);
    TEST_ASSERT_FALSE(route.verified);
    TEST_ASSERT_EQUAL_UINT16(120 + 200 + 200, route.costFixed);

    initGraphTestNodeDb(hub);
    NeighborGraph atHub;
    atHub.updateEdge(hub, parent, 1.4f, now, Edge::Source::Reported);
    atHub.updateDownstream(dest, parent, 2.0f, now);
    atHub.clearCache();
    Route hubRoute = atHub.calculateRoute(dest, now);
    TEST_ASSERT_EQUAL_UINT32(parent, hubRoute.nextHop);
    TEST_ASSERT_FALSE(hubRoute.verified);
    TEST_ASSERT_EQUAL_UINT16(140 + 200, hubRoute.costFixed);
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

void test_a_dropped_young_node_is_not_a_coverage_target()
{
    // A young node publishes no topology, so coverage treats it as stock and may elect an
    // owner. If the limiter is dropping that traffic, the owner is not a coverage target.
    constexpr NodeNum me = 0x0A0B0C0D;
    constexpr NodeNum silent = 0x22222222;
    initGraphTestNodeDb(me);
    NeighborGraph graph;
    graph.updateEdge(me, silent, 1.5f, 1000, Edge::Source::Reported);
    static NodeNum blocked = silent;
    NeighborGraph::CoveragePolicy policy;
    policy.me = me;
    policy.meRelays = true;
    policy.poorLinkEtx = 7.0f;
    TEST_ASSERT_TRUE(graph.admitsCoverage(me, silent, 7.0f, &policy));
    policy.isDroppedCoverageTarget = [](void *, NodeNum n) { return n == blocked; };
    TEST_ASSERT_FALSE_MESSAGE(graph.admitsCoverage(me, silent, 7.0f, &policy),
                              "dropped traffic is nobody's to carry");
    NodeNum coveredBy[1] = {0x33333333};
    TEST_ASSERT_EQUAL_UINT32(0, graph.uniqueCoverageNeighbor(me, coveredBy, 1, 7.0f, &policy));
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
    TEST_ASSERT_FALSE(srTopologyVersionInWindow(1, 98));   // rebooted peer: large behind / boot / silence
    TEST_ASSERT_TRUE(srTopologyVersionLargeBehind(1, 26));
    TEST_ASSERT_FALSE(srTopologyVersionLargeBehind(25, 26));
    TEST_ASSERT_TRUE(srTopologyVersionClimbing(2, 1));
    TEST_ASSERT_TRUE(srTopologyVersionClimbing(3, 1));
    TEST_ASSERT_FALSE(srTopologyVersionClimbing(1, 1));
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
    // A few counts behind last is a delayed old list, not a reboot.
    TEST_ASSERT_EQUAL(SrTopologyVerdict::Stale, srTopologyVersionVerdict(25, 26, 5000, 6000, resync, false));
    TEST_ASSERT_EQUAL(SrTopologyVerdict::Stale, srTopologyVersionVerdict(25, 26, 5000, 5000 + resync - 1, resync, false));
    // Far behind last is a restarted counter (Inno 26→1). Apply on the first list.
    TEST_ASSERT_EQUAL(SrTopologyVerdict::CounterReset, srTopologyVersionVerdict(1, 26, 5000, 6000, resync, false));
    TEST_ASSERT_EQUAL(SrTopologyVerdict::CounterReset, srTopologyVersionVerdict(2, 26, 5000, 6000, resync, false));
    // ...until two silent intervals, or its version-0 boot broadcast (small behind still waits).
    TEST_ASSERT_EQUAL(SrTopologyVerdict::SilenceResync, srTopologyVersionVerdict(25, 26, 5000, 5000 + resync, resync, false));
    TEST_ASSERT_EQUAL(SrTopologyVerdict::BootReset, srTopologyVersionVerdict(0, 26, 5000, 6000, resync, true));
    // After a boot reset the first neighbour list is version 1, not another 0.
    TEST_ASSERT_EQUAL(SrTopologyVerdict::Accept, srTopologyVersionVerdict(1, 0, 5000, 6000, resync, false));
    // A header-only version-0 report without the boot flag, only a few behind, is delayed-old.
    TEST_ASSERT_EQUAL(SrTopologyVerdict::Stale, srTopologyVersionVerdict(0, 5, 5000, 6000, resync, false));
    // millis() wrap: an accept just before the wrap is still recent after it; 26→1 is still a restart.
    TEST_ASSERT_EQUAL(SrTopologyVerdict::CounterReset, srTopologyVersionVerdict(1, 26, 0xFFFFF000u, 1000, resync, false));
    // A lost boot broadcast when last is still nearby: climbing rejects re-base, including a missed list.
    TEST_ASSERT_EQUAL(SrTopologyVerdict::RestartClimb, srTopologyVersionVerdict(2, 5, 5000, 6000, resync, false, true, 1));
    TEST_ASSERT_EQUAL(SrTopologyVerdict::RestartClimb, srTopologyVersionVerdict(3, 5, 5000, 6000, resync, false, true, 1));
    TEST_ASSERT_EQUAL(SrTopologyVerdict::Stale, srTopologyVersionVerdict(1, 5, 5000, 6000, resync, false, true, 1));
    TEST_ASSERT_EQUAL(SrTopologyVerdict::RestartClimb, srTopologyVersionVerdict(0, 5, 5000, 6000, resync, false, true, 255));
    // Originator still talking: a complete list rebases on the first packet when the jump is small.
    TEST_ASSERT_EQUAL(SrTopologyVerdict::DirectResync,
                      srTopologyVersionVerdict(12, 13, 5000, 6000, resync, false, false, 0, true));
    // Relayed copies keep the stale reject so a delayed old list cannot clobber a newer one.
    TEST_ASSERT_EQUAL(SrTopologyVerdict::Stale,
                      srTopologyVersionVerdict(12, 13, 5000, 6000, resync, false, false, 0, false));
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

// +17 dB of margin or more is the saturation point; going further must not lower the ETX any more,
// at a fast preset (SF7, threshold -7.5) or a slow one (SF12, threshold -20).
void test_margin_saturates_at_the_top_regardless_of_preset()
{
    float atCap = NeighborGraph::calculateETX(-60, -7.5f + 17.0f, meshtastic_Config_LoRaConfig_ModemPreset_SHORT_FAST);
    float pastCap = NeighborGraph::calculateETX(-60, 20.0f, meshtastic_Config_LoRaConfig_ModemPreset_SHORT_FAST);
    TEST_ASSERT_EQUAL_FLOAT(atCap, pastCap);

    float atCapSlow = NeighborGraph::calculateETX(-60, -20.0f + 17.0f, meshtastic_Config_LoRaConfig_ModemPreset_LONG_SLOW);
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

// Margin pinned at the saturation point (SNR +7.0 at SHORT_SLOW is margin +17.0, so delivery
// probability is flat at 0.95) isolates the RSSI term. At RSSI -110 dBm, strictly
// between rssiFactorBreakDbm's -120 and -60: t = (-110 - (-120)) / 60 = 1/6,
// rssiFactor = 0.90 + (1/6)*0.10 = 0.91667, prob = 0.95 * 0.91667 = 0.87083,
// ETX = 1/0.87083 = 1.14833. Moving rssiFactorBreakDbm[0] from -120 to -100 puts -110
// at-or-below the new breakpoint, so the RSSI factor collapses to the flat 0.90 and ETX becomes
// 1.16959 instead.
void test_weak_rssi_at_saturated_margin_pins_the_rssi_floor_breakpoint()
{
    float etx = NeighborGraph::calculateETX(-110, 7.0f, meshtastic_Config_LoRaConfig_ModemPreset_SHORT_SLOW);
    TEST_ASSERT_FLOAT_WITHIN(0.0005f, 1.14833f, etx);
}

static void test_purge_for_preset_change_drops_neighbours_and_derived_state()
{
    constexpr NodeNum me = 0xAAAAAAAA;
    constexpr NodeNum peer = 0xBBBBBBBB;
    constexpr NodeNum far = 0xCCCCCCCC;
    initGraphTestNodeDb(me);

    // Write the downstream entry on the clock the reader uses. `updateDownstream` takes an
    // explicit timestamp but `isDownstream` ages entries against `millis()`, so a synthetic
    // future timestamp underflows the unsigned age and the entry reads as long expired — the
    // setup would then assert nothing about the purge.
    NeighborGraph graph;
    const uint32_t nowSecs = millis() / 1000;
    graph.updateEdge(me, peer, 1.0f, nowSecs, Edge::Source::Reported);
    graph.updateDownstream(far, peer, 2.0f, nowSecs);
    graph.recordNodeTransmission(peer, 42, nowSecs);
    Route routed = graph.calculateRoute(peer, nowSecs);
    TEST_ASSERT_NOT_EQUAL(0, routed.nextHop);
    TEST_ASSERT_GREATER_THAN(0, graph.countDirectNeighbors());
    TEST_ASSERT_TRUE(graph.isDownstream(far));
    TEST_ASSERT_TRUE(graph.hasNodeTransmitted(peer, 42, nowSecs));
    TEST_ASSERT_NOT_EQUAL(0, graph.getCachedRoute(peer, nowSecs).nextHop);

    graph.purgeForPresetChange();

    TEST_ASSERT_EQUAL(0, graph.countDirectNeighbors());
    TEST_ASSERT_EQUAL_UINT32(0, graph.getNodeCount());
    TEST_ASSERT_FALSE(graph.isDownstream(far));
    TEST_ASSERT_FALSE(graph.hasNodeTransmitted(peer, 42, nowSecs + 1));
    TEST_ASSERT_EQUAL_UINT32(0, graph.calculateRoute(peer, nowSecs + 1).nextHop);
    TEST_ASSERT_EQUAL_UINT32(0, graph.getCachedRoute(peer, nowSecs + 1).nextHop);
}

// Window positions are one slot time apart, not one half-airtime: a reservation orders our
// expectation of a stock draw, and spacing that by an airtime we are not sending claims a
// precision we do not have. Origin is 2·CWmax·slot_time (160 for slot=10).
static void test_reservations_sit_one_slot_apart()
{
    SrPositionAllocator a(10, 100, 160, 15);
    TEST_ASSERT_EQUAL_UINT32(0, a.takeReserved());
    TEST_ASSERT_EQUAL_UINT32(10, a.takeReserved());
    TEST_ASSERT_EQUAL_UINT32(20, a.takeReserved());
}

// Rungs take window positions while a half-airtime still fits; when the window is full they spill
// past the transition. An empty window therefore places the first ranked position at 0, not at
// the origin — that is what makes a top-ranked SR ROUTER early. Non-ROUTER roles use takeLateRung.
static void test_rungs_take_window_positions_while_a_half_airtime_fits()
{
    // Window width 150 ms (15 × 10); half-airtime 50 so three ranked positions fit.
    SrPositionAllocator a(10, 50, 160, 15);
    TEST_ASSERT_EQUAL_UINT32(0, a.takeRung());
    TEST_ASSERT_EQUAL_UINT32(50, a.takeRung());
    TEST_ASSERT_EQUAL_UINT32(100, a.takeRung());
}

// Non-ROUTER SR-active roles always sit at/after the preset origin (relay floor), never in the
// early window — even when the window is empty.
static void test_late_rungs_never_enter_the_early_window()
{
    SrPositionAllocator a(10, 50, 160, 15);
    TEST_ASSERT_EQUAL_UINT32(160, a.takeLateRung());
    TEST_ASSERT_EQUAL_UINT32(210, a.takeLateRung());
    SrPositionAllocator afterEarly(10, 50, 160, 15);
    TEST_ASSERT_EQUAL_UINT32(0, afterEarly.takeRung());
    TEST_ASSERT_TRUE(afterEarly.takeLateRung() >= 160);
}

// One reservation then ranked positions: they share the window in order.
static void test_reservations_and_ranked_positions_interleave_in_the_window()
{
    SrPositionAllocator a(10, 50, 160, 15);
    TEST_ASSERT_EQUAL_UINT32(0, a.takeReserved());
    TEST_ASSERT_EQUAL_UINT32(10, a.takeRung());
    TEST_ASSERT_EQUAL_UINT32(60, a.takeRung());
}

// Rungs live on the ladder past the transition once the window cannot hold another half-airtime.
// Reserved positions can push the first spill later, never earlier.
static void test_rungs_spill_past_the_transition_when_the_window_is_full()
{
    SrPositionAllocator a(10, 100, 160, 15);
    TEST_ASSERT_EQUAL_UINT32(0, a.takeRung());
    // Still room in a 150 ms window for another 100 ms? cursor at 100, width 150 — yes at 100.
    TEST_ASSERT_EQUAL_UINT32(100, a.takeRung());
    // cursor 200 >= 150: spill. Last placed at 100, cleared by half = 200, max with origin 160.
    TEST_ASSERT_EQUAL_UINT32(200, a.takeRung());

    // The same ladder with a reservation ahead of it. Admission is by cursor position, not by
    // fit, so the reservation's own slot time shifts every position after it by 10 ms and the
    // spill lands at 210 rather than the 200 above: a reservation pushes the ladder out, never in.
    SrPositionAllocator held(10, 100, 160, 15);
    TEST_ASSERT_EQUAL_UINT32(0, held.takeReserved());
    TEST_ASSERT_EQUAL_UINT32(10, held.takeRung());
    TEST_ASSERT_EQUAL_UINT32(110, held.takeRung());
    TEST_ASSERT_EQUAL_UINT32(210, held.takeRung());
}

// The empty case is stated separately: folding it into the formula would leave the last position
// at zero and degenerate to a bare half-airtime.
static void test_an_empty_window_puts_the_first_rung_at_the_transition()
{
    SrPositionAllocator a(89, 5702, 1424, 15);
    TEST_ASSERT_EQUAL_UINT32(1424, a.firstRungMs());
    TEST_ASSERT_EQUAL_UINT32(1424, a.firstFreeMs());
}

// The window holds its positions and no more; anything further spills past the transition.
static void test_the_window_never_holds_more_than_its_positions()
{
    SrPositionAllocator a(1, 100, 16, 15);
    for (int i = 0; i < 15; i++) {
        TEST_ASSERT_TRUE(a.takeReserved() < 15);
    }
    TEST_ASSERT_TRUE(a.takeReserved() >= 16);
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

static void test_radio_reconfigured_purges_only_when_the_preset_changes()
{
    initGraphTestNodeDb(0xAAAAAAAA);
    config.lora.modem_preset = meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST;
    SignalRoutingModule module;
    module.rememberRelayIdentity(0xBBBB00BB, 0xBB);
    module.radioReconfigured();
    TEST_ASSERT_GREATER_THAN(0, module.relayIdentityCacheSize());
    module.radioReconfigured();
    TEST_ASSERT_GREATER_THAN(0, module.relayIdentityCacheSize());

    config.lora.modem_preset = meshtastic_Config_LoRaConfig_ModemPreset_SHORT_SLOW;
    module.radioReconfigured();
    TEST_ASSERT_EQUAL_UINT8(0, module.relayIdentityCacheSize());
    TEST_ASSERT_TRUE(module.bootBroadcastPending());
    TEST_ASSERT_EQUAL_UINT8(0, module.publishedTopologyVersion());

    module.rememberRelayIdentity(0xBBBB00BB, 0xBB);
    module.radioReconfigured();
    TEST_ASSERT_GREATER_THAN(0, module.relayIdentityCacheSize());

    config.lora.modem_preset = meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST;
    module.radioReconfigured();
    TEST_ASSERT_EQUAL_UINT8(0, module.relayIdentityCacheSize());
    TEST_ASSERT_TRUE(module.bootBroadcastPending());
}

// MeshModule::callModules only delivers undecoded frames to modules with encryptedOk. Without it,
// a direct LoRa hearing whose payload does not decrypt never reaches handleReceived, so the sender
// never enters the graph. Pin the admission flag and the observation path that records the sender.
static void test_undecoded_direct_frame_is_recorded_as_a_neighbour()
{
    constexpr NodeNum me = 0xAAAAAAAA;
    constexpr NodeNum stranger = 0x46ce027c;
    initGraphTestNodeDb(me);

    class ObservingModule : public SignalRoutingModule {
    public:
        bool admitsEncrypted() const { return encryptedOk; }
        ProcessMessage observe(const meshtastic_MeshPacket &mp) { return handleReceived(mp); }
    };
    ObservingModule module;
    TEST_ASSERT_TRUE(module.admitsEncrypted());

    meshtastic_MeshPacket mp = {};
    mp.from = stranger;
    mp.to = NODENUM_BROADCAST;
    mp.id = 0x9c1d4e21;
    mp.hop_start = 0;
    mp.hop_limit = 0;
    mp.relay_node = static_cast<uint8_t>(stranger & 0xFF);
    mp.rx_rssi = -80;
    mp.rx_snr = 5.0f;
    mp.via_mqtt = false;
    mp.which_payload_variant = meshtastic_MeshPacket_encrypted_tag;
    mp.channel = 0xf0;

    TEST_ASSERT_TRUE(SignalRoutingModule::isDirectPacket(mp));
    TEST_ASSERT_EQUAL_UINT32(0, module.resolveRelayIdentity(mp.relay_node));
    module.observe(mp);
    TEST_ASSERT_EQUAL_UINT32(stranger, module.resolveRelayIdentity(mp.relay_node));
}

static NeighborGraph seedUnicastFieldGraph(NodeNum me, NodeNum peer, NodeNum gw, NodeNum dest)
{
    const uint32_t now = millis() / 1000;
    NeighborGraph graph;
    graph.updateEdge(me, gw, 1.5f, now, Edge::Source::Reported);
    graph.setEdgeHearsUs(me, gw, true);
    graph.updateEdge(me, peer, 1.0f, now, Edge::Source::Reported);
    graph.setEdgeHearsUs(me, peer, true);
    graph.updateDownstream(dest, gw, 2.0f, now);
    return graph;
}

void test_hop_health_two_misses_make_suspect()
{
    HopHealth h;
    const NodeNum dest = 0xEE0000EE;
    const NodeNum hop = 0xBB0000BB;
    TEST_ASSERT_FALSE(h.isSuspect(dest, hop, 1000));
    TEST_ASSERT_FALSE(h.recordMiss(dest, hop, 1000));
    TEST_ASSERT_FALSE(h.isSuspect(dest, hop, 1100));
    TEST_ASSERT_TRUE(h.recordMiss(dest, hop, 2000));
    TEST_ASSERT_TRUE(h.isSuspect(dest, hop, 2100));
}

void test_hop_health_success_resets_and_ttl_expires()
{
    HopHealth h;
    const NodeNum dest = 1;
    const NodeNum hop = 2;
    h.recordMiss(dest, hop, 10);
    h.recordMiss(dest, hop, 20);
    TEST_ASSERT_TRUE(h.isSuspect(dest, hop, 30));
    TEST_ASSERT_TRUE(h.recordSuccess(dest, hop));
    TEST_ASSERT_FALSE(h.isSuspect(dest, hop, 60));

    const uint32_t nearWrap = 0xFFFFFFFFu - 1000;
    h.recordMiss(dest, hop, nearWrap);
    const uint32_t lastMiss = nearWrap + 1;
    h.recordMiss(dest, hop, lastMiss);
    TEST_ASSERT_TRUE(h.isSuspect(dest, hop, lastMiss + 2));
    TEST_ASSERT_FALSE(h.isSuspect(dest, hop, lastMiss + HOP_HEALTH_SUSPECT_TTL_MS));
}

void test_hop_health_misses_older_than_ttl_do_not_count()
{
    HopHealth h;
    const NodeNum dest = 1;
    const NodeNum hop = 2;
    h.recordMiss(dest, hop, 0);
    // A single miss from one TTL ago is not consecutive with this one.
    TEST_ASSERT_FALSE(h.recordMiss(dest, hop, HOP_HEALTH_SUSPECT_TTL_MS));
    TEST_ASSERT_FALSE(h.isSuspect(dest, hop, HOP_HEALTH_SUSPECT_TTL_MS + 1));
    const uint32_t t = HOP_HEALTH_SUSPECT_TTL_MS + 10;
    TEST_ASSERT_TRUE(h.recordMiss(dest, hop, t));
    // After expiry a suspect pair starts over and reports when it becomes suspect again.
    const uint32_t later = t + HOP_HEALTH_SUSPECT_TTL_MS;
    TEST_ASSERT_FALSE(h.isSuspect(dest, hop, later));
    TEST_ASSERT_FALSE(h.recordMiss(dest, hop, later));
    TEST_ASSERT_TRUE(h.recordMiss(dest, hop, later + 1));
}

void test_hop_health_evicts_oldest()
{
    HopHealth h;
    for (uint8_t i = 0; i < HOP_HEALTH_MAX_ENTRIES; i++) {
        h.recordMiss(0x1000 + i, 0x2000 + i, 1000 + i);
    }
    TEST_ASSERT_FALSE(h.isSuspect(0x1000, 0x2000, 2000)); // one miss only, but entry exists
    h.recordMiss(0xDEAD, 0xBEEF, 50000);
    // Oldest (0x1000) evicted: two misses cannot make it suspect anymore without a fresh entry.
    h.recordMiss(0x1000, 0x2000, 50001);
    TEST_ASSERT_FALSE(h.isSuspect(0x1000, 0x2000, 50002));
}

void test_route_exclusion_yields_alternate()
{
    constexpr NodeNum me = 0xAA0000AA;
    constexpr NodeNum hopA = 0xBB0000BB;
    constexpr NodeNum hopB = 0xCC0000CC;
    constexpr NodeNum dest = 0xDD0000DD;
    initGraphTestNodeDb(me);
    NeighborGraph graph;
    const uint32_t now = millis() / 1000;
    graph.updateEdge(me, hopA, 1.0f, now, Edge::Source::Reported);
    graph.setEdgeHearsUs(me, hopA, true);
    graph.updateEdge(me, hopB, 1.5f, now, Edge::Source::Reported);
    graph.setEdgeHearsUs(me, hopB, true);
    graph.updateEdge(hopA, dest, 1.0f, now, Edge::Source::Mirrored);
    graph.updateEdge(hopB, dest, 1.0f, now, Edge::Source::Mirrored);
    graph.updateEdge(dest, hopA, 1.0f, now, Edge::Source::Mirrored);
    graph.updateEdge(dest, hopB, 1.0f, now, Edge::Source::Mirrored);

    NeighborGraph::RoutePolicy policy;
    Route primary = graph.calculateRoute(dest, now, policy);
    TEST_ASSERT_EQUAL_UINT32(hopA, primary.nextHop);

    NodeNum excluded[1] = {hopA};
    policy.excluded = excluded;
    policy.excludedCount = 1;
    Route alt = graph.calculateRoute(dest, now, policy);
    TEST_ASSERT_EQUAL_UINT32(hopB, alt.nextHop);

    NodeNum both[2] = {hopA, hopB};
    policy.excluded = both;
    policy.excludedCount = 2;
    Route none = graph.calculateRoute(dest, now, policy);
    TEST_ASSERT_EQUAL_UINT32(0, none.nextHop);

    // Destination itself is never an intermediate hop filter target in Dijkstra.
    NodeNum exclDest[1] = {dest};
    policy.excluded = exclDest;
    policy.excludedCount = 1;
    Route still = graph.calculateRoute(dest, now, policy);
    TEST_ASSERT_EQUAL_UINT32(hopA, still.nextHop);
}

void test_is_direct_packet_originator_rule()
{
    meshtastic_MeshPacket mp = {};
    mp.from = 0x12345678;
    mp.hop_start = 3;
    mp.hop_limit = 3;
    mp.relay_node = 0;
    TEST_ASSERT_TRUE(SignalRoutingModule::isDirectPacket(mp));
    mp.relay_node = 0x78;
    TEST_ASSERT_TRUE(SignalRoutingModule::isDirectPacket(mp));
    mp.relay_node = 0xAB;
    TEST_ASSERT_FALSE(SignalRoutingModule::isDirectPacket(mp));
    mp.relay_node = 0;
    mp.hop_limit = 2;
    TEST_ASSERT_FALSE(SignalRoutingModule::isDirectPacket(mp));
}

void test_named_forward_followup_waits_for_our_airtime_and_slot_one()
{
    initGraphTestNodeDb(0xAAAAAAAA);
    SignalRoutingModule module;
    const uint32_t airtime = 400;
    const uint32_t delay = module.namedForwardFollowupDelayMs(0, airtime, 0);
    // Counted from handing our frame to the radio: our own airtime comes before the hop's wait.
    TEST_ASSERT_TRUE(delay >= airtime + module.nextHopCarryWaitMs(0, airtime, 0));
    // Our airtime, then slot 1 opens no earlier than one more leader airtime, then its own airtime.
    TEST_ASSERT_TRUE(delay >= 3 * airtime);
}

// The redirect names only an SR-active alternate whose byte differs from the silent hop's.
static void test_alternate_next_hop_requires_a_distinct_byte()
{
    constexpr NodeNum me = 0x0A0B0C0D;
    constexpr NodeNum silent = 0xBB0000BB;
    constexpr NodeNum alt = 0xCC0000CC;
    constexpr NodeNum dest = 0xEE0000EE;
    initGraphTestNodeDb(me);
    config.device.role = meshtastic_Config_DeviceConfig_Role_CLIENT;

    class GraphWriter : public SignalRoutingModule {
    public:
        void note(NodeNum from, NodeNum to, bool hearsUs) { updateGraphWithNeighbor(from, to, -90, 5, hearsUs); }
    };
    GraphWriter module;
    module.note(me, silent, true);
    module.note(me, alt, false);
    // alt's own list makes it an SR-active publisher; its links are noted after it, because a
    // list replaces the publisher's edges.
    ingestOneNeighbor(module, alt, dest, true, 1);
    module.note(alt, me, true);
    module.note(alt, dest, true);
    ingestOneNeighbor(module, dest, alt, true, 2);
    module.note(me, alt, true);

    const NodeNum excluded[1] = {silent};
    TEST_ASSERT_EQUAL_UINT32(alt, module.alternateNextHop(dest, excluded, 1, static_cast<uint8_t>(silent & 0xFF)));
    // Same byte as the silent hop: a frame naming it would read as that hop again.
    TEST_ASSERT_EQUAL_UINT32(0, module.alternateNextHop(dest, excluded, 1, static_cast<uint8_t>(alt & 0xFF)));
}

// Mirrors MeshRustic unicast_relay::tests for the shared cancel predicate.
void test_unicast_dupe_cancel_predicate()
{
    constexpr NodeNum me = 0x046b553a;
    constexpr NodeNum peer = 0xbdacce55;
    constexpr NodeNum gw = 0x63dc8f8c;
    constexpr NodeNum dest = 0x32aca541;
    const uint32_t now = millis() / 1000;
    const NeighborGraph::RoutePolicy policy;
    initGraphTestNodeDb(me);

    {
        NeighborGraph graph = seedUnicastFieldGraph(me, peer, gw, dest);
        TEST_ASSERT_TRUE(graph.unicastDupeCancels(me, dest, 0x30, gw, 0, policy));
        TEST_ASSERT_FALSE(graph.unicastDupeCancels(me, dest, 0x30, peer, 0, policy));
        TEST_ASSERT_TRUE(graph.unicastDupeCancels(me, dest, 0x30, 0, 0, policy));
        TEST_ASSERT_TRUE(graph.unicastDupeCancels(me, dest, 0x30, Edge::PLACEHOLDER_NODE_BASE | 0x99, 0, policy));
    }

    {
        NeighborGraph graph = seedUnicastFieldGraph(me, peer, gw, dest);
        graph.updateEdge(me, dest, 1.2f, now, Edge::Source::Reported);
        graph.setEdgeHearsUs(me, dest, true);
        TEST_ASSERT_FALSE(graph.unicastDupeCancels(me, dest, 0x31, peer, 0, policy));
        TEST_ASSERT_FALSE(graph.unicastDupeCancels(me, dest, 0x31, 0, 0, policy));
    }

    {
        NeighborGraph graph = seedUnicastFieldGraph(me, peer, gw, dest);
        graph.updateEdge(peer, gw, 1.5f, now, Edge::Source::Reported);
        graph.setEdgeHearsUs(peer, gw, true);
        TEST_ASSERT_FALSE(graph.unicastDupeCancels(me, dest, 0x32, peer, gw, policy));
        TEST_ASSERT_TRUE(graph.unicastDupeCancels(me, dest, 0x33, peer, gw, policy));
    }
}

void test_unicast_last_hop_slots_and_dupe_flags()
{
    constexpr NodeNum me = 0x046b553a;
    constexpr NodeNum peer = 0xbdacce55;
    constexpr NodeNum gw = 0x63dc8f8c;
    constexpr NodeNum dest = 0x32aca541;
    constexpr NodeNum extra = 0x11111111;
    const uint32_t now = millis() / 1000;
    const NeighborGraph::RoutePolicy policy;
    initGraphTestNodeDb(me);

    {
        UnicastCandidate cands[3] = {{me, 100}, {peer, 100}, {gw, UNICAST_DOWNSTREAM_TIER}};
        uint8_t count = 3;
        TEST_ASSERT_TRUE(unicastKeepLastHopSlots(cands, count, me, false));
        TEST_ASSERT_EQUAL_UINT8(2, count);
        TEST_ASSERT_EQUAL_UINT32(me, cands[0].nodeId);
        TEST_ASSERT_EQUAL_UINT32(gw, cands[1].nodeId);
    }
    {
        UnicastCandidate cands[2] = {{peer, 100}, {me, 100}};
        uint8_t count = 2;
        TEST_ASSERT_FALSE(unicastKeepLastHopSlots(cands, count, me, false));
    }
    {
        UnicastCandidate cands[3] = {{peer, 100}, {me, 100}, {extra, 120}};
        uint8_t count = 3;
        TEST_ASSERT_TRUE(unicastKeepLastHopSlots(cands, count, me, true));
        TEST_ASSERT_EQUAL_UINT8(2, count);
        TEST_ASSERT_EQUAL_UINT32(peer, cands[0].nodeId);
        TEST_ASSERT_EQUAL_UINT32(me, cands[1].nodeId);
    }

    {
        NeighborGraph graph = seedUnicastFieldGraph(me, peer, gw, dest);
        graph.updateEdge(me, dest, 1.2f, now, Edge::Source::Reported);
        graph.setEdgeHearsUs(me, dest, true);
        UnicastSlotFlags flags;
        flags.lastHopBackup = true;
        TEST_ASSERT_FALSE_MESSAGE(graph.unicastDupeCancels(me, dest, 0x40, peer, dest, policy, flags, 0),
                                  "another last hop is not dest's ACK");
        TEST_ASSERT_TRUE(graph.unicastDupeCancels(me, dest, 0x40, dest, dest, policy, flags, 0));
    }

    {
        NeighborGraph graph = seedUnicastFieldGraph(me, peer, gw, dest);
        UnicastSlotFlags flags;
        flags.nonfinalFlood = true;
        flags.nominatedNextHop = gw;
        TEST_ASSERT_FALSE_MESSAGE(graph.unicastDupeCancels(me, dest, 0x40, peer, gw, policy, flags, (uint8_t)(peer & 0xFF)),
                                  "a named same-hop SR that cannot finish is not dest's ACK");
        TEST_ASSERT_TRUE(graph.unicastDupeCancels(me, dest, 0x40, gw, gw, policy, flags, 0));
        TEST_ASSERT_TRUE(graph.unicastDupeCancels(me, dest, 0x40, dest, gw, policy, flags, 0));
        TEST_ASSERT_TRUE_MESSAGE(graph.unicastDupeCancels(me, dest, 0x40, peer, gw, policy, flags, 0),
                                 "another flood copy cancels");
    }
}

// Strong last hop names the destination; a link past the poor-link ceiling does not.
// hasStrongHopTo is the same ceiling for intermediate (non-last) onward hops.
void test_strong_delivery_hop_respects_poor_link_ceiling()
{
    constexpr NodeNum me = 0xAA0000AA;
    constexpr NodeNum strong = 0xBB0000BB;
    constexpr NodeNum weak = 0xCC0000CC;
    initGraphTestNodeDb(me);
    config.device.role = meshtastic_Config_DeviceConfig_Role_CLIENT;

    class GraphWriter : public SignalRoutingModule {
    public:
        void hear(NodeNum n, int32_t rssi, float snr) { updateNeighborInfo(n, rssi, snr, millis() / 1000); }
        bool strong(NodeNum dest) const { return hasStrongDeliveryHop(dest); }
        bool strongHop(NodeNum peer) const { return hasStrongHopTo(peer); }
        NeighborGraph *g() { return routingGraph; }
    };
    GraphWriter module;
    // Excellent link: well under the default poor-link ceiling (7.0).
    module.hear(strong, -50, 12.0f);
    module.g()->setEdgeHearsUs(me, strong, true);
    // Priced last hop past the ceiling (force the measured ETX; RSSI/SNR alone under SF7 can
    // still land below 7.0).
    module.hear(weak, -95, 3.0f);
    module.g()->setEdgeHearsUs(me, weak, true);
    module.g()->updateEdge(me, weak, 12.0f, millis() / 1000, Edge::Source::Reported);
    module.g()->setEdgeHearsUs(me, weak, true);

    TEST_ASSERT_TRUE(module.hasPricedDeliveryHop(strong));
    TEST_ASSERT_TRUE(module.strong(strong));
    TEST_ASSERT_TRUE(module.strongHop(strong));
    TEST_ASSERT_TRUE(module.hasPricedDeliveryHop(weak));
    TEST_ASSERT_FALSE(module.strong(weak));
    TEST_ASSERT_FALSE(module.strongHop(weak));
}

// Named-backup cancel: strong designated copies cancel without finish proof; weak floods do not
// (field: MB59 flooded next=0 and silenced backups that would have stamped Czar→city).
void test_unicast_named_backup_cancels_on_strong_designated_copy_not_weak_flood()
{
    constexpr NodeNum me = 0x5879fa8f;   // angl
    constexpr NodeNum peer = 0x979ed146; // Dura
    constexpr NodeNum gw = 0x63dc8f8c;   // Czar (designated)
    constexpr NodeNum dest = 0xee594922; // MR22 (unknown to our empty graph)
    constexpr uint8_t onward = 0x6c;     // FCM6-class onward stamp
    const NeighborGraph::RoutePolicy policy;
    initGraphTestNodeDb(me);

    NeighborGraph graph; // empty: no edges, cannot unicastCanFinish anyone toward dest
    UnicastSlotFlags flags;
    flags.designatedNextHop = (uint8_t)(gw & 0xFF);
    flags.armedHopLimit = 7;

    TEST_ASSERT_FALSE(graph.unicastCanFinish(gw, dest, policy));
    TEST_ASSERT_FALSE_MESSAGE(
        graph.unicastDupeCancels(me, dest, 0x50, 0, 0, policy, flags, 0, (uint8_t)(gw & 0xFF), 6),
        "weak flood from designated hop must not cancel the named backup");
    TEST_ASSERT_FALSE_MESSAGE(graph.unicastDupeCancels(me, dest, 0x51, gw, 0, policy, flags, 0, 0, 6),
                              "resolved designated hop flooding next=0 must not cancel");
    TEST_ASSERT_TRUE_MESSAGE(
        graph.unicastDupeCancels(me, dest, 0x54, 0, 0, policy, flags, onward, (uint8_t)(gw & 0xFF), 6),
        "strong designated copy cancels without finish proof");
    TEST_ASSERT_TRUE_MESSAGE(graph.unicastDupeCancels(me, dest, 0x55, gw, 0, policy, flags, onward, 0, 6),
                             "resolved designated hop with onward stamp cancels without finish proof");
    TEST_ASSERT_TRUE_MESSAGE(
        graph.unicastDupeCancels(me, dest, 0x52, peer, 0, policy, flags, (uint8_t)(gw & 0xFF), (uint8_t)(peer & 0xFF), 5),
        "lower hop_limit still naming the designation means that hop progressed");
    TEST_ASSERT_FALSE_MESSAGE(
        graph.unicastDupeCancels(me, dest, 0x53, peer, 0, policy, flags, 0, (uint8_t)(peer & 0xFF), 6),
        "an unrelated peer copy does not cancel on designation alone when finish/rank are unknown");
}

// Undesignated flood with no stampable path: broadcast coverage, not delay-0. A node that adds
// nothing beyond the originator's TX stays silent (Lab: angl on Dura→MR22).
static void test_undesignated_flood_without_route_stays_silent_when_covered()
{
    constexpr NodeNum me = 0x5879fa8f;
    constexpr NodeNum source = 0x979ed146;
    constexpr NodeNum dest = 0xee594922;
    initGraphTestNodeDb(me);
    config.device.role = meshtastic_Config_DeviceConfig_Role_CLIENT;

    class GraphWriter : public SignalRoutingModule {
    public:
        void hear(NodeNum n, int32_t rssi, float snr) { updateNeighborInfo(n, rssi, snr, millis() / 1000); }
    };
    GraphWriter module;
    module.hear(source, -40, 12.0f);

    meshtastic_MeshPacket uni = meshtastic_MeshPacket_init_zero;
    uni.from = source;
    uni.to = dest;
    uni.id = 0x5d2e81fc;
    uni.next_hop = 0;
    uni.hop_limit = 7;
    uni.hop_start = 7;
    uni.relay_node = static_cast<uint8_t>(source & 0xFF);
    uni.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    TEST_ASSERT_FALSE_MESSAGE(module.shouldRelayUnicastForCoordination(&uni),
                              "no unique coverage beyond the originator");
}

// Designated with no onward route: do not relay the data (NO_ROUTE is sent when routingModule is live).
static void test_designated_hop_with_no_route_does_not_relay()
{
    constexpr NodeNum me = 0x0A0B0C0D;
    constexpr NodeNum source = 0x979ed146;
    constexpr NodeNum dest = 0xee594922;
    initGraphTestNodeDb(me);
    config.device.role = meshtastic_Config_DeviceConfig_Role_CLIENT;

    class GraphWriter : public SignalRoutingModule {
    public:
        void hear(NodeNum n, int32_t rssi, float snr) { updateNeighborInfo(n, rssi, snr, millis() / 1000); }
    };
    GraphWriter module;
    module.hear(source, -40, 12.0f);

    meshtastic_MeshPacket uni = meshtastic_MeshPacket_init_zero;
    uni.from = source;
    uni.to = dest;
    uni.id = 0x7a02;
    uni.next_hop = static_cast<uint8_t>(me & 0xFF);
    uni.hop_limit = 3;
    uni.hop_start = 3;
    uni.relay_node = static_cast<uint8_t>(source & 0xFF);
    uni.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    TEST_ASSERT_FALSE_MESSAGE(module.shouldRelayUnicastForCoordination(&uni),
                              "designated hop with no route must not relay data");
}

static void test_neighbour_that_does_not_hear_the_transmitter_gets_no_slot()
{
    constexpr NodeNum me = 0xAA0000AA;
    constexpr NodeNum hub = 0xBB0000BB;
    constexpr NodeNum tx = 0x11000011;
    constexpr NodeNum dest = 0x22000022;
    initGraphTestNodeDb(me);
    config.device.role = meshtastic_Config_DeviceConfig_Role_CLIENT;

    {
        NeighborGraph graph;
        graph.updateEdge(me, hub, 1.5f, 0, Edge::Source::Reported);
        graph.setEdgeHearsUs(me, hub, true);
        graph.updateEdge(me, tx, 1.5f, 0, Edge::Source::Reported);
        TEST_ASSERT_TRUE(graph.unicastHeardTransmitter(tx, me, me));
        TEST_ASSERT_FALSE(graph.unicastHeardTransmitter(tx, hub, me));
        graph.updateEdge(hub, tx, 1.2f, 0, Edge::Source::Mirrored);
        TEST_ASSERT_TRUE(graph.unicastHeardTransmitter(tx, hub, me));
    }

    class GraphWriter : public SignalRoutingModule {
    public:
        void note(NodeNum from, NodeNum to, bool hearsUs) { updateGraphWithNeighbor(from, to, -90, 5, hearsUs); }
    };
    GraphWriter module;
    module.note(me, hub, true);
    module.note(me, tx, false);
    ingestOneNeighbor(module, hub, dest, true, 1);
    TEST_ASSERT_TRUE(module.isSignalRoutingNode(hub));

    meshtastic_MeshPacket uni = meshtastic_MeshPacket_init_zero;
    uni.from = tx;
    uni.to = dest;
    uni.id = 0x40;
    uni.hop_start = 3;
    uni.hop_limit = 3;
    uni.relay_node = static_cast<uint8_t>(tx & 0xFF);
    uni.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    TEST_ASSERT_TRUE(module.shouldRelayUnicastForCoordination(&uni));
    const uint32_t floorMs = module.ladderTransitionMs();
    TEST_ASSERT_TRUE(module.pendingRelayDelayMs >= floorMs);
    TEST_ASSERT_TRUE(module.pendingRelayDelayMs < floorMs + 200);
}

// Local Routing ACK: originator and dest share a measured hearsUs link, and we hear dest
// directly — suppress (desk A↔B). A hub with an onward path must still carry a multi-hop ACK.
static void test_routing_ack_retraces_local_link_but_hub_relays_remote()
{
    constexpr NodeNum me = 0x0A0B0C0D;
    constexpr NodeNum src = 0xDD0000DD;
    constexpr NodeNum dest = 0xEE0000EE;
    initGraphTestNodeDb(me);
    config.device.role = meshtastic_Config_DeviceConfig_Role_CLIENT;

    class GraphWriter : public SignalRoutingModule {
    public:
        void note(NodeNum from, NodeNum to, bool hearsUs) { updateGraphWithNeighbor(from, to, -70, 8, hearsUs); }
        void hear(NodeNum n, int32_t rssi, float snr) { updateNeighborInfo(n, rssi, snr, millis() / 1000); }
    };
    GraphWriter local;
    local.hear(src, -70, 8.0f);
    local.hear(dest, -70, 8.0f);
    local.note(src, dest, true);
    local.note(src, me, true);
    ingestOneNeighbor(local, src, dest, true, 1);

    meshtastic_MeshPacket ack = meshtastic_MeshPacket_init_zero;
    ack.from = src;
    ack.to = dest;
    ack.id = 0x7007;
    ack.hop_start = 3;
    ack.hop_limit = 3;
    ack.relay_node = 0;
    ack.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    ack.decoded.portnum = meshtastic_PortNum_ROUTING_APP;
    ack.decoded.request_id = 0x1234;
    TEST_ASSERT_FALSE(local.shouldRelayUnicastForCoordination(&ack));

    constexpr NodeNum hub = 0x1080006C;
    constexpr NodeNum leaf = 0x49B5E08C;
    constexpr NodeNum city = 0x63DC8F8C;
    constexpr NodeNum orig = 0x979ED146;
    initGraphTestNodeDb(hub);
    GraphWriter bridge;
    bridge.hear(leaf, -70, 8.0f);
    bridge.hear(city, -95, 6.0f);
    bridge.note(city, orig, true);
    bridge.note(city, hub, true);
    bridge.note(leaf, orig, false);
    bridge.note(leaf, hub, true);
    bridge.note(orig, city, true);
    ingestOneNeighbor(bridge, city, orig, true, 2);
    ingestOneNeighbor(bridge, leaf, hub, true, 3);
    ingestOneNeighbor(bridge, orig, city, true, 4);

    meshtastic_MeshPacket remoteAck = meshtastic_MeshPacket_init_zero;
    remoteAck.from = leaf;
    remoteAck.to = orig;
    remoteAck.id = 0x7008;
    remoteAck.hop_start = 3;
    remoteAck.hop_limit = 3;
    remoteAck.relay_node = 0;
    remoteAck.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    remoteAck.decoded.portnum = meshtastic_PortNum_ROUTING_APP;
    remoteAck.decoded.request_id = 0x2a29669d;
    TEST_ASSERT_TRUE(bridge.shouldRelayUnicastForCoordination(&remoteAck));
}

// Hearer-priced L2→L1 keeps the publisher when the bootstrap L1→L2 listing expires.
static void test_hearer_list_keeps_publisher_when_bootstrap_bridge_expires()
{
    constexpr NodeNum me = 0xAA0000AA;
    constexpr NodeNum l1 = 0xBB0000BB;
    constexpr NodeNum l2 = 0xCC0000CC;
    initGraphTestNodeDb(me);
    NeighborGraph graph;
    const uint32_t t0 = 1000;
    graph.updateEdge(me, l1, 1.0f, t0, Edge::Source::Reported);
    graph.updateEdge(l1, l2, 2.0f, t0, Edge::Source::Mirrored);
    TEST_ASSERT_TRUE(graph.ensurePublisher(l2, NodeClass::L2, l1, t0));
    graph.updateEdge(l2, l1, 2.0f, t0, Edge::Source::Mirrored);
    TEST_ASSERT_NOT_NULL(graph.getEdgesFrom(l2));

    TEST_ASSERT_TRUE(graph.removeEdgeAndPrune(l1, l2, 2000));
    TEST_ASSERT_NOT_NULL(graph.getEdgesFrom(l2));
    TEST_ASSERT_NOT_NULL(edgeBetween(graph, l2, l1));
    TEST_ASSERT_FALSE(graph.isDownstream(l2));
}

// Both bridges gone demotes the far publisher behind the L1 that bridged it.
static void test_both_bridges_gone_demotes_far_publisher()
{
    constexpr NodeNum me = 0xAA0000AA;
    constexpr NodeNum l1 = 0xBB0000BB;
    constexpr NodeNum l2 = 0xCC0000CC;
    initGraphTestNodeDb(me);
    NeighborGraph graph;
    // Use wall-clock seconds: getDownstreamRelay ages against millis()/1000.
    const uint32_t t0 = millis() / 1000;
    graph.updateEdge(me, l1, 1.0f, t0, Edge::Source::Reported);
    graph.updateEdge(l1, l2, 2.0f, t0, Edge::Source::Mirrored);
    graph.setEdgeHearsUs(l1, l2, true);
    TEST_ASSERT_TRUE(graph.ensurePublisher(l2, NodeClass::L2, l1, t0));
    TEST_ASSERT_EQUAL_UINT32(l1, graph.getEdgesFrom(l2)->parentHint);
    graph.updateEdge(l2, l1, 2.0f, t0, Edge::Source::Mirrored);
    TEST_ASSERT_EQUAL_UINT32(l1, graph.getEdgesFrom(l2)->parentHint);
    TEST_ASSERT_TRUE(graph.removeEdge(l1, l2));
    TEST_ASSERT_TRUE(graph.removeEdge(l2, l1));
    // Demotion stamps lastUpdate with this clock; getDownstreamRelay ages against millis()/1000.
    // A future stamp underflows the unsigned age check and looks instantly stale.
    const uint32_t now = millis() / 1000;
    TEST_ASSERT_TRUE(graph.pruneUnreachableFromRoot(now));
    TEST_ASSERT_NULL(graph.getEdgesFrom(l2));
    TEST_ASSERT_EQUAL_UINT32(l1, graph.getDownstreamRelay(l2));
}

// Orphan via L1 is an unverified gateway handoff (no Dijkstra to dest).
static void test_orphan_via_l1_is_unverified_gateway_handoff()
{
    constexpr NodeNum me = 0xAA0000AA;
    constexpr NodeNum gw = 0xBB0000BB;
    constexpr NodeNum far = 0xCC0000CC;
    initGraphTestNodeDb(me);
    NeighborGraph graph;
    const uint32_t now = millis() / 1000;
    graph.updateEdge(me, gw, 1.0f, now, Edge::Source::Reported);
    graph.setL1(gw);
    graph.updateDownstream(far, gw, 2.0f, now);
    TEST_ASSERT_NULL(graph.getEdgesFrom(far));
    graph.clearCache();
    Route route = graph.calculateRoute(far, now);
    TEST_ASSERT_EQUAL_UINT32(gw, route.nextHop);
    TEST_ASSERT_FALSE(route.verified);
    TEST_ASSERT_TRUE(route.mode == RouteMode::OrphanGateway);
}

// An L2 publisher must not own a silent neighbour of ours (L1 fence).
static void test_l2_publisher_does_not_own_a_local_silent_neighbour()
{
    constexpr NodeNum me = 0xAA0000AA;
    constexpr NodeNum l1 = 0xBB0000BB;
    constexpr NodeNum l2 = 0xCC0000CC;
    constexpr NodeNum silent = 0xDD0000DD;
    initGraphTestNodeDb(me);
    NeighborGraph graph;
    const uint32_t t0 = 1000;
    graph.updateEdge(me, l1, 1.0f, t0, Edge::Source::Reported);
    graph.updateEdge(me, silent, 3.0f, t0, Edge::Source::Reported);
    graph.updateEdge(l1, l2, 1.5f, t0, Edge::Source::Mirrored);
    graph.setEdgeHearsUs(l1, l2, true);
    TEST_ASSERT_TRUE(graph.ensurePublisher(l2, NodeClass::L2, l1, t0));
    graph.updateEdge(l2, silent, 1.0f, t0, Edge::Source::Mirrored);

    NeighborGraph::CoveragePolicy policy;
    policy.me = me;
    policy.meRelays = true;
    policy.poorLinkEtx = 7.0f;
    policy.isSrActive = [](void *, NodeNum) { return true; };
    TEST_ASSERT_EQUAL_UINT32(me, graph.coverageOwner(silent, policy));
}

// Relayed copy must not orphan-steal a list-downstream destination.
static void test_relayed_copy_does_not_steal_list_downstream()
{
    constexpr NodeNum me = 0xAA0000AA;
    constexpr NodeNum l1 = 0xBB0000BB;
    constexpr NodeNum l3 = 0xDD0000DD;
    constexpr NodeNum y = 0xEE0000EE;
    initGraphTestNodeDb(me);
    NeighborGraph graph;
    const uint32_t now = millis() / 1000;
    graph.updateEdge(me, l1, 1.0f, now, Edge::Source::Reported);
    graph.setEdgeHearsUs(me, l1, true);
    TEST_ASSERT_TRUE(graph.ensurePublisher(l3, NodeClass::L3, l1, now));
    graph.updateDownstreamListed(y, l3, 2.0f, now, true);
    NodeNum listParent = 0;
    uint16_t listCost = 0;
    TEST_ASSERT_TRUE(graph.listDownstream(y, listParent, listCost));
    TEST_ASSERT_EQUAL_UINT32(l3, listParent);

    // steal_ok is false when dest is list-parked behind a live ball parent.
    const bool destInBall = graph.getEdgesFrom(y) != nullptr;
    const bool listParked = graph.listDownstream(y, listParent, listCost);
    const bool wasDirect = false;
    const bool stealOk = wasDirect || (!destInBall && !listParked);
    TEST_ASSERT_FALSE(stealOk);
    TEST_ASSERT_EQUAL_UINT32(l3, graph.getDownstreamRelay(y));
}

// List-downstream behind a ball parent composes Dijkstra to M + hop cost, verified.
static void test_list_downstream_route_is_verified_compose()
{
    constexpr NodeNum me = 0xAA0000AA;
    constexpr NodeNum hub = 0xF60000F6;
    constexpr NodeNum parent = 0x11000011;
    constexpr NodeNum dest = 0x22000022;
    const uint32_t now = millis() / 1000;
    initGraphTestNodeDb(me);

    NeighborGraph graph;
    graph.updateEdge(me, hub, 1.2f, now, Edge::Source::Reported);
    graph.updateEdge(hub, me, 1.2f, now, Edge::Source::Mirrored);
    graph.updateEdge(hub, parent, 1.4f, now, Edge::Source::Mirrored);
    graph.updateEdge(parent, hub, 1.4f, now, Edge::Source::Mirrored);
    TEST_ASSERT_TRUE(graph.ensurePublisher(parent, NodeClass::L2, hub, now));
    graph.updateDownstreamListed(dest, parent, 2.0f, now, true);
    graph.clearCache();
    Route route = graph.calculateRoute(dest, now);
    TEST_ASSERT_EQUAL_UINT32(hub, route.nextHop);
    TEST_ASSERT_TRUE(route.verified);
    TEST_ASSERT_TRUE(route.mode == RouteMode::ListDownstream);
    Route miss = graph.getCachedRoute(0x99000099, now);
    TEST_ASSERT_EQUAL_UINT32(0, miss.nextHop);
    TEST_ASSERT_FALSE(miss.verified);
}

static void test_sender_horizon_ok_requires_anchor()
{
    constexpr NodeNum me = 0xAA0000AA;
    constexpr NodeNum l1 = 0xBB0000BB;
    constexpr NodeNum far = 0xFF0000FF;
    constexpr NodeNum y = 0xEE0000EE;
    initGraphTestNodeDb(me);
    NeighborGraph graph;
    const uint32_t now = millis() / 1000;
    graph.updateEdge(me, l1, 1.0f, now, Edge::Source::Reported);
    graph.setL1(l1);
    NodeNum listedFar[] = {y};
    TEST_ASSERT_FALSE(graph.senderHorizonOk(far, false, false, listedFar, 1));
    NodeNum listedL1[] = {l1};
    TEST_ASSERT_TRUE(graph.senderHorizonOk(far, false, false, listedL1, 1));
    TEST_ASSERT_TRUE(graph.senderHorizonOk(far, false, true, listedFar, 1));
}

// Asymmetric L1→FAR (hearsUs=0) must not unlock FAR via reachableViaHearsUs / senderHorizonOk.
static void test_asymmetric_edge_does_not_unlock_horizon()
{
    constexpr NodeNum me = 0xAA0000AA;
    constexpr NodeNum l1 = 0xBB0000BB;
    constexpr NodeNum far = 0xF10000F1;
    constexpr NodeNum y = 0xEE0000EE;
    initGraphTestNodeDb(me);
    NeighborGraph graph;
    const uint32_t now = millis() / 1000;
    graph.updateEdge(me, l1, 1.0f, now, Edge::Source::Reported);
    graph.setL1(l1);
    graph.updateEdge(l1, far, 2.0f, now, Edge::Source::Mirrored);
    // hearsUs left false — asymmetric listing still stores the edge, but must not unlock horizon.
    TEST_ASSERT_NOT_NULL(edgeBetween(graph, l1, far));
    TEST_ASSERT_FALSE(graph.reachableViaHearsUs(far));
    NodeNum listedY[] = {y};
    TEST_ASSERT_FALSE(graph.senderHorizonOk(far, false, false, listedY, 1));
}

// First ingest of a far sender that names both an L1 and an L2 must class as L2, not L3.
static void test_far_sender_naming_l1_and_l2_is_l2_not_l3()
{
    constexpr NodeNum me = 0xAA0000AA;
    constexpr NodeNum l1 = 0xBB0000BB;
    constexpr NodeNum l2 = 0xCC0000CC;
    constexpr NodeNum far = 0xF10000F1;
    initGraphTestNodeDb(me);
    config.device.role = meshtastic_Config_DeviceConfig_Role_CLIENT;

    class GraphWriter : public SignalRoutingModule {
    public:
        void hear(NodeNum n, int32_t rssi, float snr) { updateNeighborInfo(n, rssi, snr, millis() / 1000); }
        NeighborGraph *g() { return routingGraph; }
        void ingestBoth(const meshtastic_MeshPacket &mp, meshtastic_SignalRoutingInfo *info)
        {
            preProcessSignalRoutingPacket(&mp);
            handleReceivedProtobuf(mp, info);
        }
    };
    GraphWriter module;
    module.hear(l1, -70, 8.0f);
    ingestOneNeighbor(module, l1, l2, true, 1);
    TEST_ASSERT_TRUE(module.g()->getNodeClass(l2) == NodeClass::L2);

    uint8_t packed[64] = {};
    writePackedTopologyHeader(packed, 2, true);
    encodePackedNeighborEntry(packed + PACKED_NEIGHBOR_HEADER_SIZE, l1, -75, 7, true, true, 0);
    encodePackedNeighborEntry(packed + PACKED_NEIGHBOR_HEADER_SIZE + PACKED_NEIGHBOR_ENTRY_SIZE, l2, -80, 5, true,
                              true, 0);
    const size_t packedLen = PACKED_NEIGHBOR_HEADER_SIZE + 2 * PACKED_NEIGHBOR_ENTRY_SIZE;

    meshtastic_SignalRoutingInfo info = meshtastic_SignalRoutingInfo_init_zero;
    info.packed_neighbors.size = packedLen;
    memcpy(info.packed_neighbors.bytes, packed, packedLen);

    uint8_t payload[96];
    pb_ostream_t stream = pb_ostream_from_buffer(payload, sizeof(payload));
    TEST_ASSERT_TRUE(pb_encode(&stream, &meshtastic_SignalRoutingInfo_msg, &info));

    meshtastic_MeshPacket mp = meshtastic_MeshPacket_init_zero;
    mp.from = far;
    mp.to = NODENUM_BROADCAST;
    mp.id = 0x52;
    mp.hop_start = 3;
    mp.hop_limit = 2;
    mp.relay_node = static_cast<uint8_t>(l1 & 0xFF);
    mp.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    mp.decoded.portnum = meshtastic_PortNum_SIGNAL_ROUTING_APP;
    mp.decoded.payload.size = stream.bytes_written;
    memcpy(mp.decoded.payload.bytes, payload, stream.bytes_written);

    module.ingestBoth(mp, &info);
    TEST_ASSERT_TRUE_MESSAGE(module.g()->getNodeClass(far) == NodeClass::L2,
                             "naming an L1 makes the sender L2 even when the list also names an L2");
}

static void test_unqualified_far_topology_does_not_steal_list_downstream()
{
    constexpr NodeNum me = 0xAA0000AA;
    constexpr NodeNum l1 = 0xBB0000BB;
    constexpr NodeNum l3 = 0xDD0000DD;
    constexpr NodeNum y = 0xEE0000EE;
    constexpr NodeNum far = 0xF10000F1;
    initGraphTestNodeDb(me);
    config.device.role = meshtastic_Config_DeviceConfig_Role_CLIENT;

    class GraphWriter : public SignalRoutingModule {
    public:
        void hear(NodeNum n, int32_t rssi, float snr) { updateNeighborInfo(n, rssi, snr, millis() / 1000); }
        NeighborGraph *g() { return routingGraph; }
        void ingestBoth(const meshtastic_MeshPacket &mp, meshtastic_SignalRoutingInfo *info)
        {
            preProcessSignalRoutingPacket(&mp);
            handleReceivedProtobuf(mp, info);
        }
    };
    GraphWriter module;
    module.hear(l1, -70, 8.0f);
    TEST_ASSERT_TRUE(module.g()->ensurePublisher(l3, NodeClass::L3, l1, millis() / 1000));
    module.g()->updateDownstreamListed(y, l3, 2.0f, millis() / 1000, true);
    TEST_ASSERT_EQUAL_UINT32(l3, module.g()->getDownstreamRelay(y));

    meshtastic_MeshPacket mp = meshtastic_MeshPacket_init_zero;
    meshtastic_SignalRoutingInfo info = meshtastic_SignalRoutingInfo_init_zero;
    fillTopoPacket(mp, info, far, y, true, 0x51, 3, 2, static_cast<uint8_t>(l1 & 0xFF));
    module.ingestBoth(mp, &info);

    TEST_ASSERT_EQUAL_UINT32(l3, module.g()->getDownstreamRelay(y));
    TEST_ASSERT_NULL(module.g()->getEdgesFrom(far));
    TEST_ASSERT_NULL(module.g()->getEdgesFrom(y));
}

static void test_relayed_observe_does_not_steal_ball_dest()
{
    constexpr NodeNum me = 0xAA0000AA;
    constexpr NodeNum l1 = 0xBB0000BB;
    constexpr NodeNum dest = 0xCC0000CC;
    initGraphTestNodeDb(me);
    config.device.role = meshtastic_Config_DeviceConfig_Role_CLIENT;

    class GraphWriter : public SignalRoutingModule {
    public:
        void hear(NodeNum n, int32_t rssi, float snr) { updateNeighborInfo(n, rssi, snr, millis() / 1000); }
        NeighborGraph *g() { return routingGraph; }
        ProcessMessage rx(const meshtastic_MeshPacket &mp) { return handleReceived(mp); }
        void note(NodeNum from, NodeNum to, bool hearsUs) { updateGraphWithNeighbor(from, to, -90, 5, hearsUs); }
    };
    GraphWriter module;
    module.hear(l1, -70, 8.0f);
    module.g()->setEdgeHearsUs(me, l1, true);
    TEST_ASSERT_TRUE(module.g()->ensurePublisher(dest, NodeClass::L2, l1, millis() / 1000));
    module.note(dest, l1, true);
    ingestOneNeighbor(module, dest, l1, true, 0x61);

    meshtastic_MeshPacket mp = meshtastic_MeshPacket_init_zero;
    mp.from = dest;
    mp.to = NODENUM_BROADCAST;
    mp.id = 0x62;
    mp.hop_start = 3;
    mp.hop_limit = 2;
    mp.relay_node = static_cast<uint8_t>(l1 & 0xFF);
    mp.rx_rssi = -80;
    mp.rx_snr = 6;
    mp.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    mp.decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
    module.rx(mp);

    TEST_ASSERT_NOT_NULL(module.g()->getEdgesFrom(dest));
    TEST_ASSERT_NOT_EQUAL(l1, module.g()->getDownstreamRelay(dest));
}

static void test_topology_pack_prefers_hears_us_and_sr_over_better_etx()
{
    constexpr NodeNum me = 0xAA0000AA;
    constexpr NodeNum strongMute = 0xB1000001;
    constexpr NodeNum midHears = 0xB2000002;
    constexpr NodeNum weakSr = 0xB3000003;
    initGraphTestNodeDb(me);
    config.device.role = meshtastic_Config_DeviceConfig_Role_CLIENT;

    class GraphWriter : public SignalRoutingModule {
    public:
        void hear(NodeNum n, int32_t rssi, float snr) { updateNeighborInfo(n, rssi, snr, millis() / 1000); }
        NeighborGraph *g() { return routingGraph; }
        uint8_t pack(uint8_t *buf, size_t n) { return packNeighborsForBroadcast(buf, n); }
    };
    GraphWriter module;
    module.hear(strongMute, -50, 12.0f);
    module.hear(midHears, -70, 8.0f);
    module.hear(weakSr, -90, 2.0f);
    module.g()->setEdgeHearsUs(me, midHears, true);
    module.g()->setEdgeHearsUs(me, weakSr, true);
    ingestOneNeighbor(module, weakSr, me, true, 0x71);

    uint8_t buf[PACKED_NEIGHBOR_HEADER_SIZE + 8 * PACKED_NEIGHBOR_ENTRY_SIZE] = {};
    uint8_t n = module.pack(buf, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT8(3, n);
    writePackedTopologyHeader(buf, 1, true);
    PackedNeighborEntry out[8] = {};
    PackedHeader header = {};
    uint8_t count = decodePackedNeighbors(buf, PACKED_NEIGHBOR_HEADER_SIZE + n * PACKED_NEIGHBOR_ENTRY_SIZE, out, 8, &header);
    TEST_ASSERT_EQUAL_UINT8(3, count);
    TEST_ASSERT_EQUAL_UINT32(weakSr, out[0].nodeId);
    TEST_ASSERT_EQUAL_UINT32(midHears, out[1].nodeId);
    TEST_ASSERT_EQUAL_UINT32(strongMute, out[2].nodeId);
}

// Czar and Z00b share 0x8c: both full IDs stay cached; without RX metrics we do not pick one.
static void test_shared_relay_byte_keeps_both_identities_ambiguous_without_rssi()
{
    constexpr NodeNum me = 0x1080006C;
    constexpr NodeNum czar = 0x63DC8F8C;
    constexpr NodeNum zoob = 0x49B5E08C;
    initGraphTestNodeDb(me);

    class GraphWriter : public SignalRoutingModule {
    public:
        void hear(NodeNum n, int32_t rssi, float snr) { updateNeighborInfo(n, rssi, snr, millis() / 1000); }
    };
    GraphWriter module;
    module.hear(czar, -95, 6.0f);
    module.hear(zoob, -70, 8.0f);
    module.rememberRelayIdentity(czar, 0x8C);
    module.rememberRelayIdentity(zoob, 0x8C);
    TEST_ASSERT_EQUAL_UINT32(0, module.resolveRelayIdentity(0x8C));
    TEST_ASSERT_EQUAL_UINT32(zoob, module.resolveRelayIdentity(0x8C, -70, 8.0f));
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
    RUN_TEST(test_a_single_rf_observation_survives_aging);
    RUN_TEST(test_age_edges_keeps_a_heard_neighbour_with_no_published_list);
    RUN_TEST(test_age_edges_keeps_empty_l3_publisher_until_admission_ttl);
    RUN_TEST(test_an_improvement_larger_than_the_bar_is_significant);
    RUN_TEST(test_a_degradation_larger_than_the_bar_is_significant);
    RUN_TEST(test_a_change_just_above_half_is_significant_on_a_quiet_edge);
    RUN_TEST(test_a_change_smaller_than_the_bar_is_not_significant_either_way);
    RUN_TEST(test_variance_raises_the_bar_so_a_noisy_edge_stops_reporting_small_jumps);
    RUN_TEST(test_a_brand_new_edge_is_significant_unconditionally);
    RUN_TEST(test_relay_refresh_skips_without_reported_edge);
    RUN_TEST(test_mirrored_edge_update_does_not_upgrade_reported_edge);
    RUN_TEST(test_topology_listing_us_confirms_sender_hears_us);
    RUN_TEST(test_self_coverage_is_who_hears_the_relay);
    RUN_TEST(test_a_candidates_coverage_is_who_listed_it);
    RUN_TEST(test_route_never_uses_a_one_way_edge);
    RUN_TEST(test_asymmetric_l1_routes_via_l2_hearer_not_our_rx);
    RUN_TEST(test_route_cost_is_measured_at_the_receiver);
    RUN_TEST(test_variance_outranks_a_slightly_better_mean);
    RUN_TEST(test_silence_variance_follows_age_bands);
    RUN_TEST(test_delivery_cost_rises_with_silence_on_our_rx_edge);
    RUN_TEST(test_last_heard_follows_the_on_air_transmitter);
    RUN_TEST(test_in_window_hear_does_not_fold_silence_but_etx_jump_still_raises);
    RUN_TEST(test_silence_fold_keeps_scar_after_a_long_gap_packet);
    RUN_TEST(test_packed_variance_stays_stored_while_silence_is_live);
    RUN_TEST(test_variance_outranks_a_slightly_better_mean_when_a_neighbour_is_silent);
    RUN_TEST(test_egress_silence_applies_only_after_we_have_heard_them);
    RUN_TEST(test_unverified_priced_hop_saturates_instead_of_wrapping);
    RUN_TEST(test_a_backup_does_not_relay_when_its_next_hop_cannot_hear_it);
    RUN_TEST(test_inbound_gateway_is_the_fallback_only_without_a_confirmed_path);
    RUN_TEST(test_topology_listing_peer_confirms_peer_hears_sender);
    RUN_TEST(test_covers_requires_evidence_and_a_sound_link);
    RUN_TEST(test_can_deliver_is_optimistic_only_for_silent_nodes);
    RUN_TEST(test_coverage_owner_is_the_best_link_then_the_lowest_id);
    RUN_TEST(test_ownership_stops_at_the_coverage_ceiling);
    RUN_TEST(test_a_publisher_has_no_owner);
    RUN_TEST(test_unique_coverage_of_a_publisher_that_listed_us);
    RUN_TEST(test_unique_coverage_skips_a_publisher_that_did_not_list_us);
    RUN_TEST(test_a_publisher_we_stopped_hearing_is_nobodys_target);
    RUN_TEST(test_routing_through_us_confirms_the_sender_hears_us);
    RUN_TEST(test_a_guess_never_outranks_or_prices_a_measurement);
    RUN_TEST(test_a_silent_publisher_loses_our_direct_link);
    RUN_TEST(test_topology_does_not_park_a_heard_neighbour_as_downstream);
    RUN_TEST(test_topology_still_learns_downstream_for_nodes_we_do_not_hear);
    RUN_TEST(test_a_relayed_former_neighbour_becomes_downstream);
    RUN_TEST(test_chain_walks_to_the_first_hearable_hop);
    RUN_TEST(test_chain_returns_none_on_a_cycle_or_a_broken_path);
    RUN_TEST(test_downstream_chain_appoints_the_neighbour_we_hear);
    RUN_TEST(test_admits_coverage_credits_an_owned_neighbour);
    RUN_TEST(test_a_dropped_young_node_is_not_a_coverage_target);
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
    RUN_TEST(test_reservations_sit_one_slot_apart);
    RUN_TEST(test_rungs_take_window_positions_while_a_half_airtime_fits);
    RUN_TEST(test_late_rungs_never_enter_the_early_window);
    RUN_TEST(test_reservations_and_ranked_positions_interleave_in_the_window);
    RUN_TEST(test_rungs_spill_past_the_transition_when_the_window_is_full);
    RUN_TEST(test_an_empty_window_puts_the_first_rung_at_the_transition);
    RUN_TEST(test_the_window_never_holds_more_than_its_positions);
    RUN_TEST(test_modem_preset_observer_ignores_first_sighting_and_repeats);
    RUN_TEST(test_preset_change_resets_topology_version_and_relay_identity);
    RUN_TEST(test_radio_reconfigured_purges_only_when_the_preset_changes);
    RUN_TEST(test_undecoded_direct_frame_is_recorded_as_a_neighbour);
    RUN_TEST(test_unicast_dupe_cancel_predicate);
    RUN_TEST(test_unicast_last_hop_slots_and_dupe_flags);
    RUN_TEST(test_strong_delivery_hop_respects_poor_link_ceiling);
    RUN_TEST(test_unicast_named_backup_cancels_on_strong_designated_copy_not_weak_flood);
    RUN_TEST(test_undesignated_flood_without_route_stays_silent_when_covered);
    RUN_TEST(test_designated_hop_with_no_route_does_not_relay);
    RUN_TEST(test_neighbour_that_does_not_hear_the_transmitter_gets_no_slot);
    RUN_TEST(test_hop_health_two_misses_make_suspect);
    RUN_TEST(test_hop_health_success_resets_and_ttl_expires);
    RUN_TEST(test_hop_health_misses_older_than_ttl_do_not_count);
    RUN_TEST(test_hop_health_evicts_oldest);
    RUN_TEST(test_route_exclusion_yields_alternate);
    RUN_TEST(test_is_direct_packet_originator_rule);
    RUN_TEST(test_named_forward_followup_waits_for_our_airtime_and_slot_one);
    RUN_TEST(test_alternate_next_hop_requires_a_distinct_byte);
    RUN_TEST(test_routing_ack_retraces_local_link_but_hub_relays_remote);
    RUN_TEST(test_shared_relay_byte_keeps_both_identities_ambiguous_without_rssi);
    RUN_TEST(test_hearer_list_keeps_publisher_when_bootstrap_bridge_expires);
    RUN_TEST(test_both_bridges_gone_demotes_far_publisher);
    RUN_TEST(test_orphan_via_l1_is_unverified_gateway_handoff);
    RUN_TEST(test_l2_publisher_does_not_own_a_local_silent_neighbour);
    RUN_TEST(test_relayed_copy_does_not_steal_list_downstream);
    RUN_TEST(test_list_downstream_route_is_verified_compose);
    RUN_TEST(test_sender_horizon_ok_requires_anchor);
    RUN_TEST(test_asymmetric_edge_does_not_unlock_horizon);
    RUN_TEST(test_far_sender_naming_l1_and_l2_is_l2_not_l3);
    RUN_TEST(test_unqualified_far_topology_does_not_steal_list_downstream);
    RUN_TEST(test_relayed_observe_does_not_steal_ball_dest);
    RUN_TEST(test_topology_pack_prefers_hears_us_and_sr_over_better_etx);

    UNITY_END();
}

void loop() {}

#else

void setup() {}

void loop() {}

#endif // !MESHTASTIC_EXCLUDE_SIGNALROUTING
