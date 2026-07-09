#include "TestUtil.h"
#include "mesh/graph/NeighborGraph.h"
#include "mesh/SignalRoutingModule.h"
#include <cstring>
#include <unity.h>

#if !MESHTASTIC_EXCLUDE_SIGNALROUTING

namespace
{

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
    RUN_TEST(test_direct_signal_upsert_lookup_and_prune);

    UNITY_END();
}

void loop() {}

#else

void setup() {}

void loop() {}

#endif // !MESHTASTIC_EXCLUDE_SIGNALROUTING
