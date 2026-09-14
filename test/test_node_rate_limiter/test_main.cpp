#include "TestUtil.h"
#include <unity.h>

#if !MESHTASTIC_EXCLUDE_SIGNALROUTING

#include "mesh/NodeDB.h"
#include "mesh/NodeRateLimiter.h"
#include "mesh/Router.h"
#include "mesh/SignalRoutingModule.h"
#include "airtime.h"
#include "mesh/generated/meshtastic/portnums.pb.h"
#include <cstring>

namespace
{

constexpr NodeNum kLocalNode = 0x11111111;
constexpr NodeNum kRemote = 0x22222222;

class TestNodeDB : public NodeDB
{
  public:
    meshtastic_NodeInfoLite *getMeshNode(NodeNum n) override
    {
        if (hasFavorite && n == favoriteNum) {
            return &favoriteNode;
        }
        return NodeDB::getMeshNode(n);
    }

    void setFavorite(NodeNum n)
    {
        hasFavorite = true;
        favoriteNum = n;
        favoriteNode = meshtastic_NodeInfoLite_init_zero;
        favoriteNode.num = n;
        favoriteNode.is_favorite = true;
    }

    void clearFavorite()
    {
        hasFavorite = false;
        favoriteNum = 0;
        favoriteNode = meshtastic_NodeInfoLite_init_zero;
    }

  private:
    bool hasFavorite = false;
    NodeNum favoriteNum = 0;
    meshtastic_NodeInfoLite favoriteNode = meshtastic_NodeInfoLite_init_zero;
};

TestNodeDB *testNodeDB = nullptr;

static bool envReady = false;

static void prepareEnv(uint32_t windowSecs = 1)
{
    if (!envReady) {
        initializeTestEnvironment();
        envReady = true;
    }
    if (!testNodeDB) {
        testNodeDB = new TestNodeDB();
        nodeDB = testNodeDB;
    }
    testNodeDB->clearFavorite();
    myNodeInfo.my_node_num = kLocalNode;

    moduleConfig = meshtastic_LocalModuleConfig_init_zero;
    moduleConfig.has_node_rate_limiter = true;
    moduleConfig.node_rate_limiter = meshtastic_ModuleConfig_NodeRateLimiterConfig_init_zero;
    moduleConfig.node_rate_limiter.enabled = true;
    moduleConfig.node_rate_limiter.window_secs = windowSecs;

    config.device.role = meshtastic_Config_DeviceConfig_Role_CLIENT;
    config.device.rebroadcast_mode = meshtastic_Config_DeviceConfig_RebroadcastMode_ALL;

    signalRoutingModule = nullptr;
    router = nullptr;
    airTime = nullptr;

#if defined(UNIT_TEST)
    NodeRateLimiter::testResolveRelayHook = nullptr;
    NodeRateLimiter::testNodeInGraphHook = nullptr;
    NodeRateLimiter::testGraphHopsHook = nullptr;
    NodeRateLimiter::testChutilOverride = -1.0f;
    NodeRateLimiter::testNowOverride = -1;
#endif
}

static NodeNum resolveHookKeepByte(uint8_t relayByte, int16_t, float)
{
    // Stable non-placeholder id: 0x0A000000 | byte
    return 0x0A000000u | relayByte;
}

static NodeNum resolveHookOnly11(uint8_t relayByte, int16_t, float)
{
    if (relayByte == 0x11) {
        return 0x0A000011u;
    }
    return 0; // unresolved → shared bucket
}

static NodeNum gKeepInGraph = 0;
static bool nodeInGraphHook(NodeNum nodeId)
{
    return nodeId == gKeepInGraph;
}

static uint8_t graphHopsHook(NodeNum nodeId)
{
    if (nodeId == gKeepInGraph) {
        return 1;
    }
    return 0;
}

static meshtastic_MeshPacket makePacket(NodeNum from, NodeNum to, uint8_t hopLimit, uint8_t hopStart, uint8_t relayNode,
                                        meshtastic_PortNum port, uint32_t id)
{
    meshtastic_MeshPacket p = meshtastic_MeshPacket_init_zero;
    p.from = from;
    p.to = to;
    p.id = id;
    p.hop_limit = hopLimit;
    p.hop_start = hopStart;
    p.relay_node = relayNode;
    p.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    p.decoded.portnum = port;
    return p;
}

static void test_other_bucket_trips_at_its_threshold()
{
    prepareEnv();
    NodeRateLimiter limiter;

    // Driven to the configured threshold, not a literal: the number is sized from measured
    // traffic and is expected to move again, while the rule that it trips at the threshold is not.
    // hop_limit 0 => not a rebroadcast candidate; only originator OTHER applies
    for (uint32_t i = 0; i + 1 < NodeRateLimiter::DEFAULT_OTHER_TRIP; i++) {
        auto p = makePacket(kRemote, NODENUM_BROADCAST, 0, 0, 0, meshtastic_PortNum_TELEMETRY_APP, 100 + i);
        TEST_ASSERT_FALSE(limiter.shouldDrop(&p));
    }
    auto pTrip = makePacket(kRemote, NODENUM_BROADCAST, 0, 0, 0, meshtastic_PortNum_TELEMETRY_APP, 200);
    TEST_ASSERT_TRUE(limiter.shouldDrop(&pTrip));
    auto pAfter = makePacket(kRemote, NODENUM_BROADCAST, 0, 0, 0, meshtastic_PortNum_TELEMETRY_APP, 201);
    TEST_ASSERT_TRUE(limiter.shouldDrop(&pAfter));
}

static void test_to_us_is_never_limited()
{
    prepareEnv();
    NodeRateLimiter limiter;

    for (uint32_t i = 0; i < 20; i++) {
        auto p = makePacket(kRemote, kLocalNode, 0, 0, 0, meshtastic_PortNum_TELEMETRY_APP, 200 + i);
        TEST_ASSERT_FALSE(limiter.shouldDrop(&p));
    }
}

static void test_admin_app_counts_as_other_when_not_to_us()
{
    prepareEnv();
    NodeRateLimiter limiter;

    for (uint32_t i = 0; i + 1 < NodeRateLimiter::DEFAULT_OTHER_TRIP; i++) {
        auto p = makePacket(kRemote, NODENUM_BROADCAST, 0, 0, 0, meshtastic_PortNum_ADMIN_APP, 300 + i);
        TEST_ASSERT_FALSE(limiter.shouldDrop(&p));
    }
    auto p4 = makePacket(kRemote, NODENUM_BROADCAST, 0, 0, 0, meshtastic_PortNum_ADMIN_APP, 304);
    TEST_ASSERT_TRUE(limiter.shouldDrop(&p4));
}

static void test_favorite_bypasses_originator_not_relay()
{
    prepareEnv();
    testNodeDB->setFavorite(kRemote);
    NodeRateLimiter limiter;

    // Favorite + rebroadcast candidate: originator bypassed; RELAY charges shared unresolved
    // (signalRoutingModule null). Trip at ~60 * 100ms reference.
    for (uint32_t i = 0; i < 59; i++) {
        auto p = makePacket(kRemote, NODENUM_BROADCAST, 3, 3, 0xAB, meshtastic_PortNum_TELEMETRY_APP, 400 + i);
        TEST_ASSERT_FALSE_MESSAGE(limiter.shouldDrop(&p), "favorite should not trip originator before RELAY");
    }
    auto pTrip = makePacket(kRemote, NODENUM_BROADCAST, 3, 3, 0xAB, meshtastic_PortNum_TELEMETRY_APP, 460);
    TEST_ASSERT_TRUE_MESSAGE(limiter.shouldDrop(&pTrip), "RELAY shared bucket should trip around 60");
}

static void test_rotating_from_hits_shared_relay_bucket()
{
    prepareEnv();
    NodeRateLimiter limiter;

    // Distinct originators stay under OTHER (4); shared unresolved RELAY accumulates.
    for (uint32_t i = 0; i < 59; i++) {
        NodeNum from = 0xA0000000u + i;
        auto p = makePacket(from, NODENUM_BROADCAST, 3, 3, (uint8_t)(0x10 + (i % 200)), meshtastic_PortNum_TELEMETRY_APP,
                            500 + i);
        TEST_ASSERT_FALSE(limiter.shouldDrop(&p));
    }
    auto pTrip =
        makePacket(0xA0000000u + 59, NODENUM_BROADCAST, 3, 3, 0x99, meshtastic_PortNum_TELEMETRY_APP, 559);
    TEST_ASSERT_TRUE(limiter.shouldDrop(&pTrip));
}

static void test_originator_recovers_after_quiet_window()
{
    prepareEnv(1); // 1 s window
    NodeRateLimiter limiter;

    for (uint32_t i = 0; i + 1 < NodeRateLimiter::DEFAULT_OTHER_TRIP; i++) {
        auto p = makePacket(kRemote, NODENUM_BROADCAST, 0, 0, 0, meshtastic_PortNum_TELEMETRY_APP, 600 + i);
        TEST_ASSERT_FALSE(limiter.shouldDrop(&p));
    }
    auto pTrip = makePacket(kRemote, NODENUM_BROADCAST, 0, 0, 0, meshtastic_PortNum_TELEMETRY_APP, 700);
    TEST_ASSERT_TRUE(limiter.shouldDrop(&pTrip));

    // Hysteresis, not a sticky ban: the limiter has to *see* a window come in under half the trip
    // level before it lifts. The window that just ended was the busy one, so the first packet
    // after it is still dropped and starts a fresh window; the one after that quiet window is
    // let through. What matters is that a node which slows down always recovers — under the
    // previous rule every packet reset the window, so one that kept talking never could.
    testDelay(1100);
    auto pFirstAfter = makePacket(kRemote, NODENUM_BROADCAST, 0, 0, 0, meshtastic_PortNum_TELEMETRY_APP, 605);
    TEST_ASSERT_TRUE_MESSAGE(limiter.shouldDrop(&pFirstAfter), "the window that just ended was the busy one");

    testDelay(1100);
    auto pRecovered = makePacket(kRemote, NODENUM_BROADCAST, 0, 0, 0, meshtastic_PortNum_TELEMETRY_APP, 606);
    TEST_ASSERT_FALSE_MESSAGE(limiter.shouldDrop(&pRecovered), "a quiet window must lift the ban");
}

static void test_text_independent_of_other()
{
    prepareEnv();
    NodeRateLimiter limiter;

    for (uint32_t i = 0; i < 4; i++) {
        auto p = makePacket(kRemote, NODENUM_BROADCAST, 0, 0, 0, meshtastic_PortNum_TELEMETRY_APP, 700 + i);
        limiter.shouldDrop(&p);
    }
    // OTHER limited; TEXT should still pass
    auto text = makePacket(kRemote, NODENUM_BROADCAST, 0, 0, 0, meshtastic_PortNum_TEXT_MESSAGE_APP, 710);
    TEST_ASSERT_FALSE(limiter.shouldDrop(&text));
}

static void test_resolved_relay_independent_of_shared()
{
    prepareEnv();
    NodeRateLimiter::testResolveRelayHook = resolveHookOnly11;
    testNodeDB->setFavorite(kRemote);
    NodeRateLimiter limiter;

    // Trip the resolved slot for relay 0x11
    for (uint32_t i = 0; i < 59; i++) {
        auto p = makePacket(kRemote, NODENUM_BROADCAST, 3, 3, 0x11, meshtastic_PortNum_TELEMETRY_APP, 800 + i);
        TEST_ASSERT_FALSE(limiter.shouldDrop(&p));
    }
    auto pTrip = makePacket(kRemote, NODENUM_BROADCAST, 3, 3, 0x11, meshtastic_PortNum_TELEMETRY_APP, 860);
    TEST_ASSERT_TRUE(limiter.shouldDrop(&pTrip));
    TEST_ASSERT_TRUE(limiter.debugTracksRelay(0x0A000011u));
    TEST_ASSERT_TRUE(limiter.debugRelayLimited(0x0A000011u));
    TEST_ASSERT_FALSE(limiter.debugUnresolvedRelayLimited());

    // Different unresolved relay byte must not be blocked by the resolved ban
    auto other = makePacket(kRemote, NODENUM_BROADCAST, 3, 3, 0x22, meshtastic_PortNum_TELEMETRY_APP, 861);
    TEST_ASSERT_FALSE_MESSAGE(limiter.shouldDrop(&other), "shared unresolved must stay independent of resolved slot");
}

static void test_airutil_tightens_relay_trip()
{
    prepareEnv();
    NodeRateLimiter limiter;

    uint32_t tripLow = 0, clearLow = 0;
    NodeRateLimiter::testChutilOverride = 0.0f;
    limiter.debugRelayBudgets(100, tripLow, clearLow);

    uint32_t tripHigh = 0, clearHigh = 0;
    NodeRateLimiter::testChutilOverride = 50.0f;
    limiter.debugRelayBudgets(100, tripHigh, clearHigh);

    TEST_ASSERT_EQUAL_UINT32(6000, tripLow);  // 60 * 100ms
    TEST_ASSERT_EQUAL_UINT32(3000, tripHigh); // 0.5x at 50% chutil
    TEST_ASSERT_TRUE(tripHigh < tripLow);
    TEST_ASSERT_TRUE(clearHigh < clearLow);

    // Floor is 20 packet-eq; with max tighten 0.5x the trip is 30 eq (3000ms), still above floor.
    NodeRateLimiter::testChutilOverride = 100.0f;
    uint32_t tripMaxTighten = 0, clearMaxTighten = 0;
    limiter.debugRelayBudgets(100, tripMaxTighten, clearMaxTighten);
    TEST_ASSERT_EQUAL_UINT32(3000, tripMaxTighten);
    TEST_ASSERT_TRUE(tripMaxTighten >= 2000);
}

static void test_eviction_keeps_in_graph_originator()
{
    prepareEnv();
    gKeepInGraph = 0x1000000Fu;
    NodeRateLimiter::testNodeInGraphHook = nodeInGraphHook;
    NodeRateLimiter::testGraphHopsHook = graphHopsHook;
    NodeRateLimiter limiter;

    for (uint32_t i = 0; i < 16; i++) {
        NodeNum from = 0x10000000u + i;
        auto p = makePacket(from, NODENUM_BROADCAST, 0, 0, 0, meshtastic_PortNum_TELEMETRY_APP, 900 + i);
        TEST_ASSERT_FALSE(limiter.shouldDrop(&p));
    }
    TEST_ASSERT_EQUAL_UINT8(16, limiter.debugOriginatorCount());
    TEST_ASSERT_TRUE(limiter.debugTracksOriginator(gKeepInGraph));

    auto neu = makePacket(0x20000001u, NODENUM_BROADCAST, 0, 0, 0, meshtastic_PortNum_TELEMETRY_APP, 999);
    TEST_ASSERT_FALSE(limiter.shouldDrop(&neu));

    TEST_ASSERT_TRUE_MESSAGE(limiter.debugTracksOriginator(gKeepInGraph), "in-graph originator must survive eviction");
    TEST_ASSERT_TRUE(limiter.debugTracksOriginator(0x20000001u));
    TEST_ASSERT_EQUAL_UINT8(16, limiter.debugOriginatorCount());
}

static void test_relay_eviction_keeps_in_graph_relay()
{
    prepareEnv();
    NodeRateLimiter::testResolveRelayHook = resolveHookKeepByte;
    gKeepInGraph = 0x0A000008u; // relay byte 0x08 — among the eight below
    NodeRateLimiter::testNodeInGraphHook = nodeInGraphHook;
    NodeRateLimiter::testGraphHopsHook = graphHopsHook;
    testNodeDB->setFavorite(kRemote);
    NodeRateLimiter limiter;

    // relay_node must be non-zero (0 skips resolve and lands in shared unresolved)
    for (uint8_t b = 1; b <= 8; b++) {
        auto p = makePacket(kRemote, NODENUM_BROADCAST, 3, 3, b, meshtastic_PortNum_TELEMETRY_APP, 1000 + b);
        TEST_ASSERT_FALSE(limiter.shouldDrop(&p));
    }
    TEST_ASSERT_EQUAL_UINT8(8, limiter.debugRelayCount());
    TEST_ASSERT_TRUE(limiter.debugTracksRelay(gKeepInGraph));

    auto neu = makePacket(kRemote, NODENUM_BROADCAST, 3, 3, 0x80, meshtastic_PortNum_TELEMETRY_APP, 1100);
    TEST_ASSERT_FALSE(limiter.shouldDrop(&neu));

    TEST_ASSERT_TRUE_MESSAGE(limiter.debugTracksRelay(gKeepInGraph), "in-graph relay must survive eviction");
    TEST_ASSERT_TRUE(limiter.debugTracksRelay(0x0A000080u));
    TEST_ASSERT_EQUAL_UINT8(8, limiter.debugRelayCount());
}

static void test_would_drop_without_charging()
{
    prepareEnv();
    NodeRateLimiter limiter;

    for (uint32_t i = 0; i + 1 < NodeRateLimiter::DEFAULT_OTHER_TRIP; i++) {
        auto p = makePacket(kRemote, NODENUM_BROADCAST, 0, 0, 0, meshtastic_PortNum_TELEMETRY_APP, 1200 + i);
        TEST_ASSERT_FALSE(limiter.shouldDrop(&p));
    }
    auto pTrip = makePacket(kRemote, NODENUM_BROADCAST, 0, 0, 0, meshtastic_PortNum_TELEMETRY_APP, 1300);
    TEST_ASSERT_TRUE(limiter.shouldDrop(&pTrip));

    auto dupe = makePacket(kRemote, NODENUM_BROADCAST, 0, 0, 0, meshtastic_PortNum_TELEMETRY_APP, 1204);
    TEST_ASSERT_TRUE_MESSAGE(limiter.wouldDrop(&dupe), "wouldDrop must see originator ban");
    // wouldDrop must not extend sticky window forever by itself — another shouldDrop would;
    // here we only assert the gate sees the ban for rebroadcast suppression.
    TEST_ASSERT_TRUE(limiter.wouldDrop(&dupe));
}

static void test_relay_recovers_below_clear()
{
    prepareEnv(1); // 1 s window
    NodeRateLimiter::testResolveRelayHook = resolveHookOnly11;
    testNodeDB->setFavorite(kRemote); // avoid originator OTHER
    NodeRateLimiter limiter;

    for (uint32_t i = 0; i < 60; i++) {
        auto p = makePacket(kRemote, NODENUM_BROADCAST, 3, 3, 0x11, meshtastic_PortNum_TELEMETRY_APP, 1300 + i);
        limiter.shouldDrop(&p);
    }
    TEST_ASSERT_TRUE(limiter.debugRelayLimited(0x0A000011u));

    // Roll once while still hot (stay limited, count resets).
    testDelay(1100);
    auto hot = makePacket(kRemote, NODENUM_BROADCAST, 3, 3, 0x11, meshtastic_PortNum_TELEMETRY_APP, 1400);
    TEST_ASSERT_TRUE(limiter.shouldDrop(&hot));

    // Under-clear window then roll → lift.
    for (uint32_t i = 0; i < 5; i++) {
        auto p = makePacket(kRemote, NODENUM_BROADCAST, 3, 3, 0x11, meshtastic_PortNum_TELEMETRY_APP, 1410 + i);
        limiter.shouldDrop(&p);
    }
    testDelay(1100);
    auto after = makePacket(kRemote, NODENUM_BROADCAST, 3, 3, 0x11, meshtastic_PortNum_TELEMETRY_APP, 1500);
    TEST_ASSERT_FALSE_MESSAGE(limiter.shouldDrop(&after), "RELAY clear hysteresis should lift");
    TEST_ASSERT_FALSE(limiter.debugRelayLimited(0x0A000011u));
}

static void test_preset_airtime_scales_budget()
{
    prepareEnv();
    NodeRateLimiter limiter;
    NodeRateLimiter::testChutilOverride = 0.0f;

    uint32_t trip100 = 0, clear100 = 0;
    limiter.debugRelayBudgets(100, trip100, clear100);
    TEST_ASSERT_EQUAL_UINT32(6000, trip100);

    uint32_t trip200 = 0, clear200 = 0;
    limiter.debugRelayBudgets(200, trip200, clear200);
    TEST_ASSERT_EQUAL_UINT32(12000, trip200); // ceiling

    uint32_t trip50 = 0, clear50 = 0;
    limiter.debugRelayBudgets(50, trip50, clear50);
    TEST_ASSERT_EQUAL_UINT32(3000, trip50);
}

static meshtastic_MeshPacket textFrom(NodeNum from, uint32_t id)
{
    return makePacket(from, NODENUM_BROADCAST, 3, 3, 0, meshtastic_PortNum_TEXT_MESSAGE_APP, id);
}

static bool dropText(NodeRateLimiter &limiter, NodeNum from, uint32_t id)
{
    meshtastic_MeshPacket p = textFrom(from, id);
    return limiter.shouldDrop(&p);
}

static void test_established_originator_is_never_charged_to_young()
{
    prepareEnv();
    NodeRateLimiter::testNowOverride = NodeRateLimiter::WARMUP_MS;
    NodeRateLimiter limiter;
    limiter.debugSetBootMs(0);
    const NodeNum from = 0xAE000001;
    limiter.debugSeedYoung(from, 0);
    TEST_ASSERT_TRUE(limiter.debugTracksYoung(from));
    TEST_ASSERT_FALSE(dropText(limiter, from, 1));
    TEST_ASSERT_FALSE_MESSAGE(limiter.debugTracksYoung(from), "record released at 30 min");
    for (uint32_t i = 0; i < NodeRateLimiter::YOUNG_TRIP; i++) {
        NodeRateLimiter::testNowOverride = NodeRateLimiter::WARMUP_MS + 1 + i;
        TEST_ASSERT_FALSE(limiter.debugIsYoung(from));
        TEST_ASSERT_EQUAL_UINT32(0, limiter.debugYoungCharge());
        dropText(limiter, from, 2 + i);
    }
    TEST_ASSERT_FALSE(limiter.debugYoungLimited());
}

static void test_forged_identities_share_one_young_bucket()
{
    prepareEnv();
    NodeRateLimiter::testNowOverride = NodeRateLimiter::WARMUP_MS;
    NodeRateLimiter limiter;
    limiter.debugSetBootMs(0);
    for (uint32_t i = 0; i + 1 < NodeRateLimiter::YOUNG_TRIP; i++) {
        TEST_ASSERT_FALSE(dropText(limiter, 0xF1000000 + i, i + 1));
    }
    TEST_ASSERT_TRUE_MESSAGE(dropText(limiter, 0xF1000000 + NodeRateLimiter::YOUNG_TRIP - 1, 99),
                             "a flood of distinct young originators trips the shared bucket");
    TEST_ASSERT_TRUE(limiter.debugYoungLimited());

    NodeRateLimiter established;
    established.debugSetBootMs(0);
    for (uint32_t i = 0; i < NodeRateLimiter::YOUNG_TRIP; i++) {
        established.debugSeedYoung(0xE1000000 + i, 0);
        TEST_ASSERT_FALSE_MESSAGE(dropText(established, 0xE1000000 + i, i + 1),
                                  "the same volume from established originators must not trip young");
    }
    TEST_ASSERT_FALSE(established.debugYoungLimited());
}

static void test_young_record_is_released_after_thirty_minutes()
{
    prepareEnv();
    NodeRateLimiter::testNowOverride = NodeRateLimiter::WARMUP_MS;
    NodeRateLimiter limiter;
    limiter.debugSetBootMs(0);
    const NodeNum from = 0xAA0000AA;
    TEST_ASSERT_FALSE(dropText(limiter, from, 1));
    TEST_ASSERT_TRUE(limiter.debugTracksYoung(from));
    TEST_ASSERT_EQUAL_UINT8(1, limiter.debugYoungCount());
    NodeRateLimiter::testNowOverride = NodeRateLimiter::WARMUP_MS + NodeRateLimiter::YOUNG_AGE_MS;
    TEST_ASSERT_FALSE(dropText(limiter, from, 2));
    TEST_ASSERT_FALSE_MESSAGE(limiter.debugTracksYoung(from), "first-sighting record is deleted once the node is no longer young");
    TEST_ASSERT_EQUAL_UINT8(0, limiter.debugYoungCount());
}

static void test_no_record_fails_open_with_room_and_closed_when_full()
{
    prepareEnv();
    NodeRateLimiter::testNowOverride = NodeRateLimiter::WARMUP_MS;
    NodeRateLimiter limiter;
    limiter.debugSetBootMs(0);
    TEST_ASSERT_FALSE_MESSAGE(limiter.debugIsYoung(0xB1000001), "no record and room in the table means established");
    for (uint8_t i = 0; i < NodeRateLimiter::MAX_YOUNG_ENTRIES; i++) {
        limiter.debugSeedYoung(0xB2000000 + i, NodeRateLimiter::WARMUP_MS);
    }
    TEST_ASSERT_TRUE(limiter.debugYoungCount() == NodeRateLimiter::MAX_YOUNG_ENTRIES);
    TEST_ASSERT_TRUE_MESSAGE(limiter.debugIsYoung(0xB3000001), "no record and a full table means young");
    TEST_ASSERT_FALSE(limiter.debugTracksYoung(0xB3000001));
}

static void test_young_bucket_is_not_enforced_during_warmup()
{
    prepareEnv();
    NodeRateLimiter::testNowOverride = 0;
    NodeRateLimiter limiter;
    for (uint32_t i = 0; i < NodeRateLimiter::YOUNG_TRIP + 4; i++) {
        NodeRateLimiter::testNowOverride = i;
        TEST_ASSERT_FALSE_MESSAGE(dropText(limiter, 0xC1000000 + i, i + 1),
                                  "warmup replaces persistence: do not drop the mesh as young after boot");
    }
    TEST_ASSERT_FALSE(limiter.debugYoungLimited());
}

static void test_young_bucket_clears_below_clear_without_a_silent_window()
{
    prepareEnv();
    NodeRateLimiter::testNowOverride = NodeRateLimiter::WARMUP_MS;
    NodeRateLimiter limiter;
    limiter.debugSetBootMs(0);
    for (uint32_t i = 0; i < NodeRateLimiter::YOUNG_TRIP; i++) {
        dropText(limiter, 0xD1000000 + i, i + 1);
    }
    TEST_ASSERT_TRUE(limiter.debugYoungLimited());
    const uint32_t windowMs = 1000; // prepareEnv window_secs=1
    const uint32_t busy = NodeRateLimiter::WARMUP_MS + NodeRateLimiter::YOUNG_TRIP;
    for (uint32_t i = 0; i < 20; i++) {
        NodeRateLimiter::testNowOverride = busy + i;
        dropText(limiter, 0xD2000000 + i, 100 + i);
    }
    const uint32_t roll = busy + 20 + windowMs;
    NodeRateLimiter::testNowOverride = roll;
    TEST_ASSERT_TRUE(dropText(limiter, 0xD3000001, 200));
    for (uint32_t i = 0; i < 5; i++) {
        NodeRateLimiter::testNowOverride = roll + 1 + i;
        dropText(limiter, 0xD4000000 + i, 300 + i);
    }
    NodeRateLimiter::testNowOverride = roll + 1 + windowMs;
    TEST_ASSERT_FALSE_MESSAGE(dropText(limiter, 0xD5000001, 400),
                              "young bucket must lift when the prior window is under the clear threshold");
    TEST_ASSERT_FALSE(limiter.debugYoungLimited());
}

static void test_undecodable_traffic_does_not_charge_young()
{
    prepareEnv();
    NodeRateLimiter::testNowOverride = NodeRateLimiter::WARMUP_MS;
    NodeRateLimiter limiter;
    limiter.debugSetBootMs(0);
    for (uint32_t i = 0; i < NodeRateLimiter::YOUNG_TRIP; i++) {
        meshtastic_MeshPacket p = makePacket(0x11000000 + i, NODENUM_BROADCAST, 3, 3, 0, meshtastic_PortNum_TEXT_MESSAGE_APP, i);
        p.which_payload_variant = meshtastic_MeshPacket_encrypted_tag;
        TEST_ASSERT_FALSE_MESSAGE(limiter.shouldDrop(&p), "undecodable frames belong to UNKNOWN, not young");
    }
    TEST_ASSERT_FALSE(limiter.debugYoungLimited());
    TEST_ASSERT_EQUAL_UINT8(0, limiter.debugYoungCount());
}

static void test_young_announce_respects_refractory_and_congestion()
{
    prepareEnv();
    NodeRateLimiter::testNowOverride = NodeRateLimiter::WARMUP_MS;
    NodeRateLimiter limiter;
    limiter.debugSetBootMs(0);
    for (uint32_t i = 0; i < NodeRateLimiter::YOUNG_TRIP; i++) {
        dropText(limiter, 0xA1000000 + i, i + 1);
    }
    NodeNum ids[4] = {};
    uint8_t n = 0;
    TEST_ASSERT_TRUE(limiter.debugTakeAnnounce(ids, n));
    TEST_ASSERT_TRUE(n <= 4);

    NodeRateLimiter::testNowOverride = NodeRateLimiter::WARMUP_MS + 1000 + 10;
    for (uint32_t i = 0; i < NodeRateLimiter::YOUNG_TRIP; i++) {
        dropText(limiter, 0xA2000000 + i, 50 + i);
    }
    TEST_ASSERT_FALSE_MESSAGE(limiter.debugTakeAnnounce(ids, n), "refractory is independent of how often the bucket trips");

    NodeRateLimiter congested;
    congested.debugSetBootMs(0);
    NodeRateLimiter::testChutilOverride = 40.0f;
    NodeRateLimiter::testNowOverride = NodeRateLimiter::WARMUP_MS;
    for (uint32_t i = 0; i < NodeRateLimiter::YOUNG_TRIP; i++) {
        dropText(congested, 0xA3000000 + i, i + 1);
    }
    TEST_ASSERT_TRUE(congested.debugYoungLimited());
    TEST_ASSERT_FALSE_MESSAGE(congested.debugTakeAnnounce(ids, n), "suppressed while channel utilisation is high");

    NodeRateLimiter::testChutilOverride = 0.0f;
    NodeRateLimiter::testNowOverride = 0;
    NodeRateLimiter relayBusy;
    for (uint32_t i = 0; i < 60; i++) {
        NodeRateLimiter::testNowOverride = i;
        auto p = makePacket(0xA4000000 + i, NODENUM_BROADCAST, 3, 3, 0x10, meshtastic_PortNum_TELEMETRY_APP, 600 + i);
        relayBusy.shouldDrop(&p);
    }
    TEST_ASSERT_TRUE(relayBusy.debugUnresolvedRelayLimited());
    TEST_ASSERT_FALSE(relayBusy.debugYoungLimited());
    NodeRateLimiter::testNowOverride = NodeRateLimiter::WARMUP_MS;
    for (uint32_t i = 0; i < NodeRateLimiter::YOUNG_TRIP; i++) {
        dropText(relayBusy, 0xA5000000 + i, 700 + i);
    }
    TEST_ASSERT_TRUE(relayBusy.debugYoungLimited());
    TEST_ASSERT_FALSE_MESSAGE(relayBusy.debugTakeAnnounce(ids, n), "suppressed while the RELAY bucket is limiting");
}

} // namespace

void setUp(void) {}
void tearDown(void) {}

// An unresolvable relay byte is only evidence when resolution could have worked. After a reboot
// the graph holds no direct neighbours, so nothing resolves — charging the shared bucket then
// trips it on ordinary traffic, and because the limiter drops before the graph observes, the graph
// can never fill and the node never recovers. That loop is what this guard exists to prevent.
static void test_unresolved_relay_is_only_charged_once_resolution_is_possible()
{
    // SignalRouting present, graph still empty: nothing to resolve against, so do not charge.
    TEST_ASSERT_FALSE(NodeRateLimiter::shouldChargeUnresolvedRelay(true, false));
    // SignalRouting present with direct neighbours: an unresolved byte now means something.
    TEST_ASSERT_TRUE(NodeRateLimiter::shouldChargeUnresolvedRelay(true, true));
    // No SignalRouting at all: no resolution mechanism exists, so the shared bucket is the only
    // defence and must still be charged.
    TEST_ASSERT_TRUE(NodeRateLimiter::shouldChargeUnresolvedRelay(false, false));
}

void setup()
{
    UNITY_BEGIN();
    RUN_TEST(test_unresolved_relay_is_only_charged_once_resolution_is_possible);
    RUN_TEST(test_other_bucket_trips_at_its_threshold);
    RUN_TEST(test_to_us_is_never_limited);
    RUN_TEST(test_admin_app_counts_as_other_when_not_to_us);
    RUN_TEST(test_favorite_bypasses_originator_not_relay);
    RUN_TEST(test_rotating_from_hits_shared_relay_bucket);
    RUN_TEST(test_originator_recovers_after_quiet_window);
    RUN_TEST(test_text_independent_of_other);
    RUN_TEST(test_resolved_relay_independent_of_shared);
    RUN_TEST(test_airutil_tightens_relay_trip);
    RUN_TEST(test_eviction_keeps_in_graph_originator);
    RUN_TEST(test_relay_eviction_keeps_in_graph_relay);
    RUN_TEST(test_would_drop_without_charging);
    RUN_TEST(test_relay_recovers_below_clear);
    RUN_TEST(test_preset_airtime_scales_budget);
    RUN_TEST(test_established_originator_is_never_charged_to_young);
    RUN_TEST(test_forged_identities_share_one_young_bucket);
    RUN_TEST(test_young_record_is_released_after_thirty_minutes);
    RUN_TEST(test_no_record_fails_open_with_room_and_closed_when_full);
    RUN_TEST(test_young_bucket_is_not_enforced_during_warmup);
    RUN_TEST(test_young_bucket_clears_below_clear_without_a_silent_window);
    RUN_TEST(test_undecodable_traffic_does_not_charge_young);
    RUN_TEST(test_young_announce_respects_refractory_and_congestion);
    exit(UNITY_END());
}

void loop() {}

#else

void setUp(void) {}
void tearDown(void) {}

void setup()
{
    UNITY_BEGIN();
    UNITY_END();
}

void loop() {}

#endif
