#include "RadioLibInterface.h"
#include "MeshTypes.h"
#include "NodeDB.h"
#include "PowerMon.h"
#include "SPILock.h"
#include "Throttle.h"
#include "configuration.h"
#include "error.h"
#include "main.h"
#include "mesh-pb-constants.h"
#include "power.h"
#include <deque>
#include <algorithm>
#include <pb_decode.h>
#include <pb_encode.h>

static bool isUserGeneratedPacket(const meshtastic_MeshPacket *txp)
{
    if (!txp)
        return false;
    if (txp->which_payload_variant != meshtastic_MeshPacket_decoded_tag)
        return false;
    return txp->decoded.portnum == meshtastic_PortNum_TEXT_MESSAGE_APP;
}

#if ARCH_PORTDUINO
#include "PortduinoGlue.h"
#include "meshUtils.h"
#endif
void LockingArduinoHal::spiBeginTransaction()
{
    spiLock->lock();

    ArduinoHal::spiBeginTransaction();
}

void LockingArduinoHal::spiEndTransaction()
{
    ArduinoHal::spiEndTransaction();

    spiLock->unlock();
}
#if ARCH_PORTDUINO
void LockingArduinoHal::spiTransfer(uint8_t *out, size_t len, uint8_t *in)
{
    spi->transfer(out, in, len);
}
#endif

RadioLibInterface *RadioLibInterface::instance;
bool RadioLibInterface::airplaneMode = false;

RadioLibInterface::RadioLibInterface(LockingArduinoHal *hal, RADIOLIB_PIN_TYPE cs, RADIOLIB_PIN_TYPE irq, RADIOLIB_PIN_TYPE rst,
                                     RADIOLIB_PIN_TYPE busy, PhysicalLayer *_iface)
    : NotifiedWorkerThread("RadioIf"), module(hal, cs, irq, rst, busy), iface(_iface)
{
    instance = this;
#if defined(ARCH_STM32WL) && defined(USE_SX1262)
    module.setCb_digitalWrite(stm32wl_emulate_digitalWrite);
    module.setCb_digitalRead(stm32wl_emulate_digitalRead);
#endif
}

#ifdef ARCH_ESP32
// ESP32 doesn't use that flag
#define YIELD_FROM_ISR(x) portYIELD_FROM_ISR()
#else
#define YIELD_FROM_ISR(x) portYIELD_FROM_ISR(x)
#endif

static int8_t clampPowerValue(int8_t value, int8_t minValue, int8_t maxValue)
{
    if (value < minValue)
        return minValue;
    if (value > maxValue)
        return maxValue;
    return value;
}

static int8_t snrToDelta(float snr)
{
    if (snr >= 15.0f)
        return -6;
    if (snr >= 10.0f)
        return -4;
    if (snr >= 6.0f)
        return -2;
    if (snr >= 3.0f)
        return -1;
    if (snr <= -7.0f)
        return 4;
    if (snr <= -3.0f)
        return 2;
    return 0;
}

#include "power.h" // for PowerStatus
#include <cstdint>

uint8_t RadioLibInterface::bumpAttempts(const meshtastic_MeshPacket *txp)
{
    if (!txp)
        return 0;
    for (auto &e : attemptTable) {
        if (e.from == txp->from && e.id == txp->id) {
            if (e.attempts < 255)
                e.attempts++;
            return e.attempts;
        }
    }
    // Insert into first slot (lightweight LRU not needed for small table)
    attemptTable[0].from = txp->from;
    attemptTable[0].id = txp->id;
    attemptTable[0].attempts = 0;
    return 0;
}

int8_t RadioLibInterface::selectTxPowerForPacket(const meshtastic_MeshPacket *txp, uint8_t attempts)
{
    // Use configured TX power as ceiling; if unset, fall back to region cap
    int8_t regionCap = myRegion->powerLimit;
    int8_t maxAllowed = config.lora.tx_power ? config.lora.tx_power : (regionCap ? regionCap : power);

    // Always honor regional cap when not licensed
    if (!devicestate.owner.is_licensed && regionCap > 0)
        maxAllowed = std::min<int8_t>(maxAllowed, regionCap);

    // Default to configured power for broadcast/unknown
    int8_t target = maxAllowed;
    if (txp) {
        bool appliedSnr = false;
        // Unicast: bias to direct neighbor SNR if we have it
        if (txp->to && txp->to != NODENUM_BROADCAST && txp->to != NODENUM_BROADCAST_NO_LORA) {
            const meshtastic_NodeInfoLite *node = nodeDB->getMeshNode(txp->to);
            if (node && node->snr != 0.0f) {
                target = clampPowerValue(maxAllowed + snrToDelta(node->snr), -9, maxAllowed);
                int8_t floorPower = maxAllowed - 4;
                if (target < floorPower)
                    target = floorPower;
                appliedSnr = true;
            }
        }

        // Broadcast or unicast without SNR: use strongest heard neighbor as fallback
        if (!appliedSnr) {
            float bestSnr = 0.0f;
            size_t total = nodeDB->getNumMeshNodes();
            for (size_t i = 0; i < total; i++) {
                const meshtastic_NodeInfoLite *n = nodeDB->getMeshNodeByIndex(i);
                if (!n || n->num == nodeDB->getNodeNum())
                    continue;
                if (n->snr != 0.0f && n->snr > bestSnr)
                    bestSnr = n->snr;
            }
            if (bestSnr != 0.0f) {
                target = clampPowerValue(maxAllowed + snrToDelta(bestSnr), -9, maxAllowed);
                int8_t floorPower = maxAllowed - 4;
                if (target < floorPower)
                    target = floorPower;
            }
        }
    }

    // Step-up on retries: +2 dB for 1st retry, +4 dB for 2+ (still capped)
    if (attempts == 1)
        target = clampPowerValue(target + 2, -9, maxAllowed);
    else if (attempts >= 2)
        target = clampPowerValue(target + 4, -9, maxAllowed);

    return target;
}

void RadioLibInterface::applyOutputPower(int8_t newPower)
{
    power = newPower;
    lastTxPowerApplied = newPower;
}

int8_t RadioLibInterface::getLastTxPowerApplied()
{
    if (instance && instance->lastTxPowerApplied != 0)
        return instance->lastTxPowerApplied;
    return 0;
}

void RadioLibInterface::setAirplaneMode(bool enabled)
{
    airplaneMode = enabled;
    if (instance) {
        if (enabled) {
            instance->disabled = true;
            instance->setStandby();
        } else {
            instance->disabled = false;
            instance->startReceive();
            instance->flushAirplaneQueue();
        }
    }
    LOG_INFO("Airplane mode %s", enabled ? "ENABLED" : "DISABLED");
}

bool RadioLibInterface::isAirplaneMode()
{
    return airplaneMode;
}

void RadioLibInterface::enqueueAirplanePacket(meshtastic_MeshPacket *txp)
{
    if (airplaneQueue.size() >= AIRPLANE_QUEUE_MAX) {
        auto *old = airplaneQueue.front();
        airplaneQueue.pop_front();
        LOG_WARN("Airplane queue full, dropping oldest pending TX");
        packetPool.release(old);
    }
    airplaneQueue.push_back(txp);
    LOG_INFO("Queued TX while airplane mode active (queue size %u)", (unsigned)airplaneQueue.size());
}

void RadioLibInterface::flushAirplaneQueue()
{
    bool scheduled = false;
    while (!airplaneQueue.empty()) {
        auto *p = airplaneQueue.front();
        airplaneQueue.pop_front();
        bool dropped = false;
        if (txQueue.enqueue(p, &dropped)) {
            scheduled = true;
            LOG_INFO("Requeued TX from airplane mode (remaining %u, txFree %u)", (unsigned)airplaneQueue.size(),
                     (unsigned)txQueue.getFree());
        } else {
            LOG_WARN("Dropping queued TX from airplane mode (remaining %u, txFree %u)", (unsigned)airplaneQueue.size(),
                     (unsigned)txQueue.getFree());
            packetPool.release(p);
        }
    }
    if (scheduled) {
        startTransmitTimer(false); // send as soon as possible
    }
}

void INTERRUPT_ATTR RadioLibInterface::isrLevel0Common(PendingISR cause)
{
    instance->disableInterrupt();

    BaseType_t xHigherPriorityTaskWoken;
    instance->notifyFromISR(&xHigherPriorityTaskWoken, cause, true);

    /* Force a context switch if xHigherPriorityTaskWoken is now set to pdTRUE.
    The macro used to do this is dependent on the port and may be called
    portEND_SWITCHING_ISR. */
    YIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

void INTERRUPT_ATTR RadioLibInterface::isrRxLevel0()
{
    isrLevel0Common(ISR_RX);
}

void INTERRUPT_ATTR RadioLibInterface::isrTxLevel0()
{
    isrLevel0Common(ISR_TX);
}

/** Could we send right now (i.e. either not actively receiving or transmitting)? */
bool RadioLibInterface::canSendImmediately()
{
    // We wait _if_ we are partially though receiving a packet (rather than just merely waiting for one).
    // To do otherwise would be doubly bad because not only would we drop the packet that was on the way in,
    // we almost certainly guarantee no one outside will like the packet we are sending.
    bool busyTx = sendingPacket != NULL;
    bool busyRx = isReceiving && isActivelyReceiving();

    if (busyTx || busyRx) {
        if (busyTx) {
            LOG_WARN("Can not send yet, busyTx");
        }
        // If we've been trying to send the same packet more than one minute and we haven't gotten a
        // TX IRQ from the radio, the radio is probably broken.
        if (busyTx && !Throttle::isWithinTimespanMs(lastTxStart, 60000)) {
            LOG_ERROR("Hardware Failure! busyTx for more than 60s");
            RECORD_CRITICALERROR(meshtastic_CriticalErrorCode_TRANSMIT_FAILED);
            // reboot in 5 seconds when this condition occurs.
            rebootAtMsec = lastTxStart + 65000;
        }
        if (busyRx) {
            LOG_WARN("Can not send yet, busyRx");
        }
        return false;
    } else
        return true;
}

bool RadioLibInterface::receiveDetected(uint16_t irq, ulong syncWordHeaderValidFlag, ulong preambleDetectedFlag)
{
    bool detected = (irq & (syncWordHeaderValidFlag | preambleDetectedFlag));
    // Handle false detections
    if (detected) {
        if (!activeReceiveStart) {
            activeReceiveStart = millis();
        } else if (!Throttle::isWithinTimespanMs(activeReceiveStart, 2 * preambleTimeMsec)) {
            if (!(irq & syncWordHeaderValidFlag)) {
                // The HEADER_VALID flag should be set by now if it was really a packet, so ignore PREAMBLE_DETECTED flag
                activeReceiveStart = 0;
                LOG_DEBUG("Ignore false preamble detection");
                return false;
            } else {
                uint32_t maxPacketTimeMsec = getPacketTime(meshtastic_Constants_DATA_PAYLOAD_LEN + sizeof(PacketHeader));
                if (!Throttle::isWithinTimespanMs(activeReceiveStart, maxPacketTimeMsec)) {
                    // We should have gotten an RX_DONE IRQ by now if it was really a packet, so ignore HEADER_VALID flag
                    activeReceiveStart = 0;
                    LOG_DEBUG("Ignore false header detection");
                    return false;
                }
            }
        }
    }
    return detected;
}

/// Send a packet (possibly by enquing in a private fifo).  This routine will
/// later free() the packet to pool.  This routine is not allowed to stall because it is called from
/// bluetooth comms code.  If the txmit queue is empty it might return an error
ErrorCode RadioLibInterface::send(meshtastic_MeshPacket *p)
{

#ifndef DISABLE_WELCOME_UNSET

    if (config.lora.region != meshtastic_Config_LoRaConfig_RegionCode_UNSET) {
        if (disabled || !config.lora.tx_enabled) {
            LOG_WARN("send - !config.lora.tx_enabled");
            packetPool.release(p);
            return ERRNO_DISABLED;
        }

    } else {
        LOG_WARN("send - lora tx disabled: Region unset");
        packetPool.release(p);
        return ERRNO_DISABLED;
    }

#else

    if (disabled || !config.lora.tx_enabled) {
        LOG_WARN("send - !config.lora.tx_enabled");
        packetPool.release(p);
        return ERRNO_DISABLED;
    }

#endif

    if (p->to == NODENUM_BROADCAST_NO_LORA) {
        LOG_DEBUG("Drop no-LoRa pkt");
        return ERRNO_SHOULD_RELEASE;
    }

    // Sometimes when testing it is useful to be able to never turn on the xmitter
#ifndef LORA_DISABLE_SENDING
    printPacket("enqueue for send", p);

    LOG_DEBUG("txGood=%d,txRelay=%d,rxGood=%d,rxBad=%d", txGood, txRelay, rxGood, rxBad);
    bool dropped = false;
    ErrorCode res = txQueue.enqueue(p, &dropped) ? ERRNO_OK : ERRNO_UNKNOWN;

    if (dropped) {
        txDrop++;
    }

    if (res != ERRNO_OK) { // we weren't able to queue it, so we must drop it to prevent leaks
        packetPool.release(p);
        return res;
    }

    // set (random) transmit delay to let others reconfigure their radio,
    // to avoid collisions and implement timing-based flooding
    setTransmitDelay();

    return res;
#else
    packetPool.release(p);
    return ERRNO_DISABLED;
#endif
}

meshtastic_QueueStatus RadioLibInterface::getQueueStatus()
{
    meshtastic_QueueStatus qs;

    qs.res = qs.mesh_packet_id = 0;
    qs.free = txQueue.getFree();
    qs.maxlen = txQueue.getMaxLen();

    return qs;
}

bool RadioLibInterface::canSleep()
{
    bool res = txQueue.empty();
    if (!res) { // only print debug messages if we are vetoing sleep
        LOG_DEBUG("Radio wait to sleep, txEmpty=%d", res);
    }
    return res;
}

/** Allow other firmware components to ask whether we are currently sending a packet
Initially implemented to protect T-Echo's capacitive touch button from spurious presses during tx
*/
bool RadioLibInterface::isSending()
{
    return sendingPacket != NULL;
}

/** Attempt to cancel a previously sent packet.  Returns true if a packet was found we could cancel */
bool RadioLibInterface::cancelSending(NodeNum from, PacketId id)
{
    auto p = txQueue.remove(from, id);
    if (p)
        packetPool.release(p); // free the packet we just removed

    bool result = (p != NULL);
    LOG_DEBUG("cancelSending id=0x%x, removed=%d", id, result);
    return result;
}

/** Attempt to find a packet in the TxQueue. Returns true if the packet was found. */
bool RadioLibInterface::findInTxQueue(NodeNum from, PacketId id)
{
    return txQueue.find(from, id);
}

/** radio helper thread callback.
We never immediately transmit after any operation (either Rx or Tx). Instead we should wait a random multiple of
'slotTimes' (see definition in RadioInterface.h) taken from a contention window (CW) to lower the chance of collision.
The CW size is determined by setTransmitDelay() and depends either on the current channel utilization or SNR in case
of a flooding message. After this, we perform channel activity detection (CAD) and reset the transmit delay if it is
currently active.
*/
void RadioLibInterface::onNotify(uint32_t notification)
{
    switch (notification) {
    case ISR_TX:
        handleTransmitInterrupt();
        startReceive();
        setTransmitDelay();
        break;
    case ISR_RX:
        handleReceiveInterrupt();
        startReceive();
        setTransmitDelay();
        break;
    case TRANSMIT_DELAY_COMPLETED:

        // If we are not currently in receive mode, then restart the random delay (this can happen if the main thread
        // has placed the unit into standby)  FIXME, how will this work if the chipset is in sleep mode?
        if (!txQueue.empty()) {
            if (!canSendImmediately()) {
                setTransmitDelay(); // currently Rx/Tx-ing: reset random delay
            } else {
                meshtastic_MeshPacket *txp = txQueue.getFront();
                assert(txp);
                long delay_remaining = txp->tx_after ? txp->tx_after - millis() : 0;
                if (delay_remaining > 0) {
                    // There's still some delay pending on this packet, so resume waiting for it to elapse
                    notifyLater(delay_remaining, TRANSMIT_DELAY_COMPLETED, false);
                } else {
                    if (isChannelActive()) { // check if there is currently a LoRa packet on the channel
                        startReceive();      // try receiving this packet, afterwards we'll be trying to transmit again
                        setTransmitDelay();
                    } else {
                        // Send any outgoing packets we have ready as fast as possible to keep the time between channel scan and
                        // actual transmission as short as possible
                        txp = txQueue.dequeue();
                        assert(txp);
                        startSend(txp);
                        LOG_DEBUG("%d packets remain in the TX queue", txQueue.getMaxLen() - txQueue.getFree());
                    }
                }
            }
        } else {
            // Do nothing, because the queue is empty
        }
        break;
    default:
        assert(0); // We expected to receive a valid notification from the ISR
    }
}

void RadioLibInterface::setTransmitDelay()
{
    meshtastic_MeshPacket *p = txQueue.getFront();
    if (!p) {
        return; // noop if there's nothing in the queue
    }

    // We want all sending/receiving to be done by our daemon thread.
    // We use a delay here because this packet might have been sent in response to a packet we just received.
    // So we want to make sure the other side has had a chance to reconfigure its radio.

    if (p->tx_after) {
        unsigned long add_delay = p->rx_rssi ? getTxDelayMsecWeighted(p) : getTxDelayMsec();
        unsigned long now = millis();
        p->tx_after = min(max(p->tx_after + add_delay, now + add_delay), now + 2 * getTxDelayMsecWeightedWorst(p->rx_snr));
        notifyLater(p->tx_after - now, TRANSMIT_DELAY_COMPLETED, false);
    } else if (p->rx_snr == 0 && p->rx_rssi == 0) {
        /* We assume if rx_snr = 0 and rx_rssi = 0, the packet was generated locally.
         *   This assumption is valid because of the offset generated by the radio to account for the noise
         *   floor.
         */
        startTransmitTimer(true);
    } else {
        // If there is a SNR, start a timer scaled based on that SNR.
        LOG_DEBUG("rx_snr found. hop_limit:%d rx_snr:%f", p->hop_limit, p->rx_snr);
        startTransmitTimerRebroadcast(p);
    }
}

void RadioLibInterface::startTransmitTimer(bool withDelay)
{
    // If we have work to do and the timer wasn't already scheduled, schedule it now
    if (!txQueue.empty()) {
        uint32_t delay = !withDelay ? 1 : getTxDelayMsec();
        notifyLater(delay, TRANSMIT_DELAY_COMPLETED, false); // This will implicitly enable
    }
}

void RadioLibInterface::startTransmitTimerRebroadcast(meshtastic_MeshPacket *p)
{
    // If we have work to do and the timer wasn't already scheduled, schedule it now
    if (!txQueue.empty()) {
        uint32_t delay = getTxDelayMsecWeighted(p);
        notifyLater(delay, TRANSMIT_DELAY_COMPLETED, false); // This will implicitly enable
    }
}

/**
 * If the packet is not already in the late rebroadcast window, move it there
 */
void RadioLibInterface::clampToLateRebroadcastWindow(NodeNum from, PacketId id)
{
    // Look for non-late packets only, so we don't do this twice!
    meshtastic_MeshPacket *p = txQueue.remove(from, id, true, false);
    if (p) {
        p->tx_after = millis() + getTxDelayMsecWeightedWorst(p->rx_snr);
        bool dropped = false;
        if (txQueue.enqueue(p, &dropped)) {
            LOG_DEBUG("Move existing queued packet to the late rebroadcast window %dms from now", p->tx_after - millis());
        } else {
            packetPool.release(p);
        }
        if (dropped) {
            txDrop++;
        }
    }
}

/**
 * If there is a packet pending TX in the queue with a worse hop limit, remove it pending replacement with a better version
 * @return Whether a pending packet was removed
 */
bool RadioLibInterface::removePendingTXPacket(NodeNum from, PacketId id, uint32_t hop_limit_lt)
{
    meshtastic_MeshPacket *p = txQueue.remove(from, id, true, true, hop_limit_lt);
    if (p) {
        LOG_DEBUG("Dropping pending-TX packet 0x%08x with hop limit %d", p->id, p->hop_limit);
        packetPool.release(p);
        return true;
    }
    return false;
}

/**
 * Remove a packet that is eligible for replacement from the TX queue
 */
// void RadioLibInterface::removePending

void RadioLibInterface::handleTransmitInterrupt()
{
    // This can be null if we forced the device to enter standby mode.  In that case
    // ignore the transmit interrupt
    if (sendingPacket)
        completeSending();
    powerMon->clearState(meshtastic_PowerMon_State_Lora_TXOn); // But our transmitter is definitely off now
}

void RadioLibInterface::completeSending()
{
    // We are careful to clear sending packet before calling printPacket because
    // that can take a long time
    auto p = sendingPacket;
    sendingPacket = NULL;

    if (p) {
        // Packet has been sent, count it toward our TX airtime utilization.
        uint32_t xmitMsec = getPacketTime(p);
        airTime->logAirtime(TX_LOG, xmitMsec);

        txGood++;
        if (!isFromUs(p))
            txRelay++;
        printPacket("Completed sending", p);

        // We are done sending that packet, release it
        packetPool.release(p);
    }
}

void RadioLibInterface::handleReceiveInterrupt()
{
    // when this is called, we should be in receive mode - if we are not, just jump out instead of bombing. Possible Race
    // Condition?
    if (!isReceiving) {
        LOG_ERROR("handleReceiveInterrupt called when not in rx mode, which shouldn't happen");
        return;
    }

    isReceiving = false;

    // read the number of actually received bytes
    size_t length = iface->getPacketLength();

    uint32_t rxMsec = getPacketTime(length, true);

#ifndef DISABLE_WELCOME_UNSET
    if (config.lora.region == meshtastic_Config_LoRaConfig_RegionCode_UNSET) {
        LOG_WARN("lora rx disabled: Region unset");
        airTime->logAirtime(RX_ALL_LOG, rxMsec);
        return;
    }
#endif

    int state = iface->readData((uint8_t *)&radioBuffer, length);
#if ARCH_PORTDUINO
    if (portduino_config.logoutputlevel == level_trace) {
        printBytes("Raw incoming packet: ", (uint8_t *)&radioBuffer, length);
    }
#endif
    if (state != RADIOLIB_ERR_NONE) {
        LOG_ERROR("Ignore received packet due to error=%d (maybe to=0x%08x, from=0x%08x, flags=0x%02x)", state,
                  radioBuffer.header.to, radioBuffer.header.from, radioBuffer.header.flags);
        rxBad++;

        airTime->logAirtime(RX_ALL_LOG, rxMsec);

    } else {
        // Skip the 4 headers that are at the beginning of the rxBuf
        int32_t payloadLen = length - sizeof(PacketHeader);

        // check for short packets
        if (payloadLen < 0) {
            LOG_WARN("Ignore received packet too short");
            rxBad++;
            airTime->logAirtime(RX_ALL_LOG, rxMsec);
        } else {
            rxGood++;
            // altered packet with "from == 0" can do Remote Node Administration without permission
            if (radioBuffer.header.from == 0) {
                LOG_WARN("Ignore received packet without sender");
                return;
            }

            // Note: we deliver _all_ packets to our router (i.e. our interface is intentionally promiscuous).
            // This allows the router and other apps on our node to sniff packets (usually routing) between other
            // nodes.
            meshtastic_MeshPacket *mp = packetPool.allocZeroed();

            // Keep the assigned fields in sync with src/mqtt/MQTT.cpp:onReceiveProto
            mp->from = radioBuffer.header.from;
            mp->to = radioBuffer.header.to;
            mp->id = radioBuffer.header.id;
            mp->channel = radioBuffer.header.channel;
            assert(HOP_MAX <= PACKET_FLAGS_HOP_LIMIT_MASK); // If hopmax changes, carefully check this code
            mp->hop_limit = radioBuffer.header.flags & PACKET_FLAGS_HOP_LIMIT_MASK;
            mp->hop_start = (radioBuffer.header.flags & PACKET_FLAGS_HOP_START_MASK) >> PACKET_FLAGS_HOP_START_SHIFT;
            mp->want_ack = !!(radioBuffer.header.flags & PACKET_FLAGS_WANT_ACK_MASK);
            mp->via_mqtt = !!(radioBuffer.header.flags & PACKET_FLAGS_VIA_MQTT_MASK);
            // If hop_start is not set, next_hop and relay_node are invalid (firmware <2.3)
            mp->next_hop = mp->hop_start == 0 ? NO_NEXT_HOP_PREFERENCE : radioBuffer.header.next_hop;
            mp->relay_node = mp->hop_start == 0 ? NO_RELAY_NODE : radioBuffer.header.relay_node;

            addReceiveMetadata(mp);

            mp->which_payload_variant =
                meshtastic_MeshPacket_encrypted_tag; // Mark that the payload is still encrypted at this point
            assert(((uint32_t)payloadLen) <= sizeof(mp->encrypted.bytes));
            memcpy(mp->encrypted.bytes, radioBuffer.payload, payloadLen);
            mp->encrypted.size = payloadLen;

            printPacket("Lora RX", mp);

            airTime->logAirtime(RX_LOG, rxMsec);

            deliverToReceiver(mp);
        }
    }
}

void RadioLibInterface::startReceive()
{
    if (airplaneMode) {
        setStandby();
        return;
    }
    isReceiving = true;
    powerMon->setState(meshtastic_PowerMon_State_Lora_RXOn);
}

void RadioLibInterface::configHardwareForSend()
{
    powerMon->setState(meshtastic_PowerMon_State_Lora_TXOn);
}

void RadioLibInterface::setStandby()
{
    // neither sending nor receiving
    powerMon->clearState(meshtastic_PowerMon_State_Lora_RXOn);
    powerMon->clearState(meshtastic_PowerMon_State_Lora_TXOn);
}

/** start an immediate transmit */
bool RadioLibInterface::startSend(meshtastic_MeshPacket *txp)
{
    if (airplaneMode) {
        if (isUserGeneratedPacket(txp)) {
            enqueueAirplanePacket(txp);
        } else {
            LOG_WARN("Drop Tx packet because airplane mode is active (non-user packet)");
            packetPool.release(txp);
        }
        return false;
    }
    /* NOTE: Minimize the actions before startTransmit() to keep the time between
             channel scan and actual transmit as low as possible to avoid collisions. */
    if (disabled || !config.lora.tx_enabled) {
        LOG_WARN("Drop Tx packet because LoRa Tx disabled");
        packetPool.release(txp);
        return false;
    } else {
        uint8_t attempts = bumpAttempts(txp);
        int8_t perPacketPower = selectTxPowerForPacket(txp, attempts);
        if (perPacketPower != power) {
            applyOutputPower(perPacketPower);
        }
        configHardwareForSend(); // must be after setStandby

        size_t numbytes = beginSending(txp);

        int res = iface->startTransmit((uint8_t *)&radioBuffer, numbytes);
        if (res != RADIOLIB_ERR_NONE) {
            LOG_ERROR("startTransmit failed, error=%d", res);
            RECORD_CRITICALERROR(meshtastic_CriticalErrorCode_RADIO_SPI_BUG);

            // This send failed, but make sure to 'complete' it properly
            completeSending();
            powerMon->clearState(meshtastic_PowerMon_State_Lora_TXOn); // Transmitter off now
            startReceive(); // Restart receive mode (because startTransmit failed to put us in xmit mode)
        } else {
            // Must be done AFTER, starting transmit, because startTransmit clears (possibly stale) interrupt pending register
            // bits
            enableInterrupt(isrTxLevel0);
            lastTxStart = millis();
            printPacket("Started Tx", txp);
        }

        return res == RADIOLIB_ERR_NONE;
    }
}