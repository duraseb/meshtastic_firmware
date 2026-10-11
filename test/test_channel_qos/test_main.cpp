#include "TestUtil.h"
#include "airtime.h"
#include "mesh/ChannelQoS.h"
#include "mesh/Channels.h"
#include <unity.h>

#if !MESHTASTIC_EXCLUDE_SIGNALROUTING

namespace
{
meshtastic_MeshPacket packet(meshtastic_PortNum port, uint8_t channel = 0)
{
    meshtastic_MeshPacket p = meshtastic_MeshPacket_init_zero;
    p.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    p.decoded.portnum = port;
    p.channel = channel;
    return p;
}

void test_congestion_threshold_boundaries()
{
    ChannelQoS qos;
    const meshtastic_PortNum ports[] = {meshtastic_PortNum_TELEMETRY_APP, meshtastic_PortNum_TEXT_MESSAGE_APP,
                                        meshtastic_PortNum_ROUTING_APP};
    const float thresholds[] = {25, 30, 38};
    for (unsigned i = 0; i < 3; ++i) {
        auto p = packet(ports[i], 1);
        ChannelQoS::testChutilOverride = thresholds[i] - 0.1f;
        TEST_ASSERT_TRUE(qos.canRelay(&p));
        ChannelQoS::testChutilOverride = thresholds[i];
        TEST_ASSERT_FALSE(qos.canRelay(&p));
    }
}

void test_encrypted_union_cannot_masquerade_as_critical_payload()
{
    ChannelQoS qos;
    auto p = packet(meshtastic_PortNum_ADMIN_APP);
    p.which_payload_variant = meshtastic_MeshPacket_encrypted_tag;
    ChannelQoS::testChutilOverride = 25;
    TEST_ASSERT_FALSE(qos.canRelay(&p));
}

void test_primary_channel_is_taken_from_configuration()
{
    const auto savedChannels = channelFile;
    const auto savedChannelState = channels;
    channelFile = meshtastic_ChannelFile_init_zero;
    channelFile.channels_count = 3;
    for (unsigned i = 0; i < 3; ++i) {
        channelFile.channels[i].index = i;
        channelFile.channels[i].has_settings = true;
        channelFile.channels[i].role = meshtastic_Channel_Role_SECONDARY;
    }
    auto primary = channels.getByIndex(2);
    primary.index = 2;
    primary.role = meshtastic_Channel_Role_PRIMARY;
    primary.has_settings = true;
    channels.setChannel(primary);
    channels.onConfigChanged();
    const meshtastic_PortNum ports[] = {meshtastic_PortNum_TEXT_MESSAGE_APP, meshtastic_PortNum_TEXT_MESSAGE_COMPRESSED_APP};
    ChannelQoS qos;
    ChannelQoS::testChutilOverride = 100;
    bool primaryPasses = true, secondaryPasses = false;
    for (auto port : ports) {
        auto p = packet(port, 2);
        primaryPasses &= qos.canRelay(&p);
        p.channel = 0;
        secondaryPasses |= qos.canRelay(&p);
    }
    channelFile = savedChannels;
    channels = savedChannelState;
    TEST_ASSERT_TRUE(primaryPasses);
    TEST_ASSERT_FALSE(secondaryPasses);
}

void test_admin_remains_relayable_under_congestion()
{
    auto p = packet(meshtastic_PortNum_ADMIN_APP);
    ChannelQoS::testChutilOverride = 100;
    TEST_ASSERT_TRUE(ChannelQoS().canRelay(&p));
}

void test_missing_airtime_and_null_packet()
{
    const auto saved = airTime;
    airTime = nullptr;
    ChannelQoS::testChutilOverride = -1;
    auto p = packet(meshtastic_PortNum_TELEMETRY_APP);
    const bool permitted = ChannelQoS().canRelay(&p);
    airTime = saved;
    TEST_ASSERT_TRUE(permitted);
    TEST_ASSERT_FALSE(ChannelQoS().canRelay(nullptr));
}
} // namespace

void setUp()
{
    ChannelQoS::testChutilOverride = -1;
}
void tearDown()
{
    ChannelQoS::testChutilOverride = -1;
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_congestion_threshold_boundaries);
    RUN_TEST(test_encrypted_union_cannot_masquerade_as_critical_payload);
    RUN_TEST(test_primary_channel_is_taken_from_configuration);
    RUN_TEST(test_admin_remains_relayable_under_congestion);
    RUN_TEST(test_missing_airtime_and_null_packet);
    exit(UNITY_END());
}

#else
void setUp() {}
void tearDown() {}
void setup()
{
    UNITY_BEGIN();
    exit(UNITY_END());
}
#endif
void loop() {}
