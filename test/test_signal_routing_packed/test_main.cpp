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

} // namespace

void setUp(void) {}

void tearDown(void) {}

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

    UNITY_END();
}

void loop() {}

#else

void setup() {}

void loop() {}

#endif // !MESHTASTIC_EXCLUDE_SIGNALROUTING
