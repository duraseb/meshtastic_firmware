#include "NextHopRouter.h"
#include "MeshTypes.h"
#include "meshUtils.h"
#if !MESHTASTIC_EXCLUDE_TRACEROUTE
#include "modules/TraceRouteModule.h"
#endif
#if HAS_TRAFFIC_MANAGEMENT
#include "modules/TrafficManagementModule.h"
#endif
#include "NodeDB.h"
#if !MESHTASTIC_EXCLUDE_SIGNALROUTING
#include "SignalRoutingModule.h"
#include "ChannelQoS.h"
#include "NodeRateLimiter.h"
#endif

NextHopRouter::NextHopRouter() {}

PendingPacket::PendingPacket(meshtastic_MeshPacket *p, uint8_t numRetransmissions, bool floodOnLast)
{
    packet = p;
    this->numRetransmissions = numRetransmissions - 1; // We subtract one, because we assume the user just did the first send
    this->floodOnLast = floodOnLast;
}

/**
 * Send a packet
 */
ErrorCode NextHopRouter::send(meshtastic_MeshPacket *p)
{
    // Add any messages _we_ send to the seen message list (so we will ignore all retransmissions we see)
    p->relay_node = nodeDB->getLastByteOfNodeNum(getNodeNum()); // First set the relayer to us
    wasSeenRecently(p);                                         // FIXME, move this to a sniffSent method

    // NodeDB's learned byte is the stock fallback. An originated unicast from an SR node
    // names the graph's next hop instead, so the first transmission already tells stock
    // relays to stand down. A relayed copy is stamped later by sendRelay(); this path is
    // the one ReliableRouter uses for packets we originate (and for our own retries).
    uint8_t nextHopByte = getNextHop(p->to, p->relay_node).value_or(NO_NEXT_HOP_PREFERENCE);
#if !MESHTASTIC_EXCLUDE_SIGNALROUTING
    if (!isBroadcast(p->to) && isFromUs(p) && signalRoutingModule) {
        bool verified = false;
        NodeNum hop = signalRoutingModule->getNextHop(p->to, getNodeNum(), getNodeNum(), false, &verified);
        if (verified && hop != 0 && hop != getNodeNum()) {
            nextHopByte = nodeDB->getLastByteOfNodeNum(hop);
        }
    }
#endif
    p->next_hop = nextHopByte;
    LOG_DEBUG("Setting next hop for packet with dest %x to %x", p->to, p->next_hop);

    // If it's from us, ReliableRouter already handles retransmissions if want_ack is set. If a next hop is set and hop limit is
    // not 0 or want_ack is set, start retransmissions
    if ((!isFromUs(p) || !p->want_ack) && p->next_hop != NO_NEXT_HOP_PREFERENCE && (p->hop_limit > 0 || p->want_ack))
        startRetransmission(packetPool.allocCopy(*p)); // start retransmission for relayed packet

    return Router::send(p);
}

bool NextHopRouter::shouldFilterReceived(const meshtastic_MeshPacket *p)
{
    bool wasFallback = false;
    bool weWereNextHop = false;
    bool wasUpgraded = false;
    bool seenRecently = wasSeenRecently(p, true, &wasFallback, &weWereNextHop,
                                        &wasUpgraded); // Updates history; returns false when an upgrade is detected

    // Handle hop_limit upgrade scenario for rebroadcasters
    if (wasUpgraded && perhapsHandleUpgradedPacket(p)) {
        return true; // we handled it, so stop processing
    }

    if (seenRecently) {
        printPacket("Ignore dupe incoming msg", p);

        if (p->transport_mechanism == meshtastic_MeshPacket_TransportMechanism_TRANSPORT_LORA) {
            rxDupe++;
            // A relay replays its stamped header twice, then floods. Cancelling that series on
            // every duplicate meant a backup that cannot finish delivery also deleted it, so a
            // designated hop that missed the frame had no second chance. Unicast uses the same
            // predicate as the committed-relay cancel: keep the retry unless the duplicate's
            // transmitter can finish, or is ranked ahead of us.
            bool stopRetry = true;
#if !MESHTASTIC_EXCLUDE_SIGNALROUTING
            if (!isBroadcast(p->to) && signalRoutingModule) {
                stopRetry = signalRoutingModule->areAllNeighborsCovered(p);
            }
#endif
            if (stopRetry) {
                stopRetransmission(p->from, p->id);
            }
        }

        // If it was a fallback to flooding, try to relay again
        if (wasFallback) {
            LOG_INFO("Fallback to flooding from relay_node=0x%x", p->relay_node);
            // Check if it's still in the Tx queue, if not, we have to relay it again
            if (!findInTxQueue(p->from, p->id)) {
                reprocessPacket(p);
                perhapsRebroadcast(p);
            }
        } else {
            bool isRepeated = getHopsAway(*p) == 0;
            // Don't treat signal-routed packets as "repeated" - they preserve hop_limit by design
#if !MESHTASTIC_EXCLUDE_SIGNALROUTING
            if (isRepeated && signalRoutingModule && signalRoutingModule->shouldUseSignalBasedRouting(p)) {
                isRepeated = false;
            }
#endif
            // If repeated and not in Tx queue anymore, try relaying again, or if we are the destination, send the ACK again
            if (isRepeated) {
                if (!findInTxQueue(p->from, p->id)) {
                    reprocessPacket(p);
                    if (!perhapsRebroadcast(p) && isToUs(p) && p->want_ack) {
                        sendAckNak(meshtastic_Routing_Error_NONE, getFrom(p), p->id, p->channel, 0);
                    }
                }
            } else if (!weWereNextHop) {
                perhapsCancelDupe(p); // If it's a dupe, cancel relay if we were not explicitly asked to relay
            }
        }
        return true;
    }

    return Router::shouldFilterReceived(p);
}

void NextHopRouter::sniffReceived(const meshtastic_MeshPacket *p, const meshtastic_Routing *c)
{
    NodeNum ourNodeNum = getNodeNum();
    uint8_t ourRelayID = nodeDB->getLastByteOfNodeNum(ourNodeNum);
    bool isAckorReply = (p->which_payload_variant == meshtastic_MeshPacket_decoded_tag) &&
                        (p->decoded.request_id != 0 || p->decoded.reply_id != 0);
    if (isAckorReply) {
        // Update next-hop for the original transmitter of this successful transmission to the relay node, but ONLY if "from"
        // is not 0 (means implicit ACK) and original packet was also relayed by this node, or we sent it directly to the
        // destination
        if (p->from != 0) {
            meshtastic_NodeInfoLite *origTx = nodeDB->getMeshNode(p->from);
            if (origTx) {
                // Either relayer of ACK was also a relayer of the packet, or we were the *only* relayer and the ACK came
                // directly from the destination
                bool wasAlreadyRelayer = wasRelayer(p->relay_node, p->decoded.request_id, p->to);
                bool weWereSoleRelayer = false;
                bool weWereRelayer = wasRelayer(ourRelayID, p->decoded.request_id, p->to, &weWereSoleRelayer);
                if ((weWereRelayer && wasAlreadyRelayer) || (getHopsAway(*p) == 0 && weWereSoleRelayer)) {
                    if (origTx->next_hop != p->relay_node) { // Not already set
                        LOG_INFO("Update next hop of 0x%x to 0x%x based on ACK/reply (was relayer %d we were sole %d)", p->from,
                                 p->relay_node, wasAlreadyRelayer, weWereSoleRelayer);
                        origTx->next_hop = p->relay_node;
                    }
                }
            }
        }
        if (!isToUs(p)) {
            Router::cancelSending(p->to, p->decoded.request_id); // cancel rebroadcast for this DM
            // stop retransmission for the original packet
            stopRetransmission(p->to, p->decoded.request_id); // for original packet, from = to and id = request_id
        }
    }

    perhapsRebroadcast(p);

    // handle the packet as normal
    Router::sniffReceived(p, c);
}

/* Check if we should be rebroadcasting this packet if so, do so. */
bool NextHopRouter::perhapsRebroadcast(const meshtastic_MeshPacket *p)
{
    // Check if traffic management wants to exhaust this packet's hops
    bool exhaustHops = false;
#if HAS_TRAFFIC_MANAGEMENT
    if (trafficManagementModule && trafficManagementModule->shouldExhaustHops(*p)) {
        exhaustHops = true;
    }
#endif

    // Allow rebroadcast if hop_limit > 0 OR if we're exhausting hops (which sets hop_limit = 0 but still needs one relay)
    if (!isToUs(p) && !isFromUs(p) && (p->hop_limit > 0 || exhaustHops)) {
        if (p->id != 0) {
            if (isRebroadcaster()) {
                // Stock rule: relay a unicast only when it names nobody or names us. SignalRouting
                // coordinates unicasts by slot instead: a unicast naming another node still reaches
                // shouldRelay(), which reserves slot 0 for the designated node and ranks the rest.
                bool srCoordinatedUnicast = false;
#if !MESHTASTIC_EXCLUDE_SIGNALROUTING
                srCoordinatedUnicast = !isBroadcast(p->to) && signalRoutingModule &&
                                       signalRoutingModule->shouldUseSignalBasedRouting(p);
#endif
                bool srDecidedUnicast = false;
                NodeNum srNextHop = 0;
                if (p->next_hop == NO_NEXT_HOP_PREFERENCE || p->next_hop == nodeDB->getLastByteOfNodeNum(getNodeNum()) ||
                    srCoordinatedUnicast) {

                    // Do not amplify after an inbound rate-limit (dupe/upgrade paths skip handleReceived).
#if !MESHTASTIC_EXCLUDE_SIGNALROUTING
                    if (nodeRateLimiter && nodeRateLimiter->wouldDrop(p)) {
                        LOG_WARN("[RateLimit] Skip rebroadcast of 0x%08x from 0x%08x to 0x%08x (limited)", p->id, p->from, p->to);
                        return false;
                    }
#endif

                    // Channel QoS: gradually drop lower priority relays when channel is congested
#if !MESHTASTIC_EXCLUDE_SIGNALROUTING
                    if (channelQoS && !channelQoS->canRelay(p)) {
                        return false;
                    }
#endif

                    // Signal-based routing: check if we should relay (broadcasts and unicasts)
#if !MESHTASTIC_EXCLUDE_SIGNALROUTING
                    if (signalRoutingModule && signalRoutingModule->shouldUseSignalBasedRouting(p)) {
                        if (!signalRoutingModule->shouldRelay(p)) {
                            // The decision itself logged the packet and the reason.
                            return false;
                        }
                        // Mark this packet as committed so dupe arrivals don't cancel our relay
                        NodeNum heardFrom = signalRoutingModule->resolveHeardFrom(p, p->from);
                        uint32_t relayDelay = signalRoutingModule->pendingRelayDelayMs;
                        signalRoutingModule->pendingRelayDelayMs = 0;
                        signalRoutingModule->commitRelay(p->id, heardFrom, relayDelay);
                        if (!isBroadcast(p->to)) {
                            srDecidedUnicast = true;
                            srNextHop = signalRoutingModule->takePendingUnicastNextHop();
                        }
                    }
#endif

                    meshtastic_MeshPacket *tosend = packetPool.allocCopy(*p); // keep a copy because we will be sending it

#if !MESHTASTIC_EXCLUDE_SIGNALROUTING
                    // Apply slot-based TX delay for SR relays. Always set tx_after slightly
                    // in the future so setTransmitDelay() takes the "remaining > 0" path and
                    // respects SR's intended delay, rather than the "expired" path which draws
                    // a new random Router-class delay on top.
                    if (signalRoutingModule && signalRoutingModule->isCommittedRelay(tosend->id)) {
                        uint32_t delay = signalRoutingModule->getCommittedRelayDelay(tosend->id);
                        tosend->tx_after = millis() + delay + 1;
                    }
#endif
                    LOG_INFO("Rebroadcast received message coming from %x", p->relay_node);
                    bool srPricedLastHop = false;
                    bool srNonfinalFlood = false;
#if !MESHTASTIC_EXCLUDE_SIGNALROUTING
                    if (signalRoutingModule) {
                        UnicastSlotFlags flags = signalRoutingModule->unicastCommitFlags(p->id);
                        srNonfinalFlood = flags.nonfinalFlood;
                        srPricedLastHop = signalRoutingModule->hasPricedDeliveryHop(p->to) && !srNonfinalFlood;
                    }
#endif

                    // If exhausting hops, force hop_limit = 0 regardless of other logic
		    if (exhaustHops) {
                        tosend->hop_limit = 0;
                        LOG_INFO("Traffic management: exhausting hops for 0x%08x, setting hop_limit=0", getFrom(p));
                    } else if (shouldDecrementHopLimit(p)) {
                        // Use shared logic to determine if hop_limit should be decremented
                        tosend->hop_limit--; // bump down the hop count
                    } else {
                        LOG_INFO("favorite-ROUTER/CLIENT_BASE-to-ROUTER/CLIENT_BASE rebroadcast: preserving hop_limit");
                    }
                    // Inverted hop_start < hop_limit is unreadable (stock getHopsAway returns
                    // unknown). Treat travelled as zero without rewriting the remaining budget.
                    if (tosend->hop_start != 0 && tosend->hop_start < tosend->hop_limit) {
                        tosend->hop_start = tosend->hop_limit;
                    }
#if USERPREFS_EVENT_MODE
                    if (tosend->hop_limit > 2) {
                        // if we are "correcting" the hop_limit, "correct" the hop_start by the same amount to preserve hops away.
                        tosend->hop_start -= (tosend->hop_limit - 2);
                        tosend->hop_limit = 2;
                    }
#endif

#if !MESHTASTIC_EXCLUDE_SIGNALROUTING
                    if (srDecidedUnicast) {
                        // SR chose this leg's next hop (or none). Stamp it instead of the NodeDB-learned
                        // value, and never forward the incoming byte: it named us or a node that stayed
                        // silent, and legacy nodes relay a unicast only when the byte is clear or theirs.
                        uint8_t nextHopByte = srNextHop ? nodeDB->getLastByteOfNodeNum(srNextHop) : NO_NEXT_HOP_PREFERENCE;
                        if (srNonfinalFlood) {
                            nextHopByte = NO_NEXT_HOP_PREFERENCE;
                        } else if (srPricedLastHop) {
                            nextHopByte = nodeDB->getLastByteOfNodeNum(p->to);
                        }
                        NextHopRouter::sendRelay(tosend, nextHopByte);
                        return true;
                    }
#endif
                    if (p->next_hop == NO_NEXT_HOP_PREFERENCE) {
                        FloodingRouter::send(tosend);
                    } else {
                        NextHopRouter::send(tosend);
                    }

                    return true;
                }
            } else {
                LOG_DEBUG("No rebroadcast: Role = CLIENT_MUTE or Rebroadcast Mode = NONE");
            }
        } else {
            LOG_DEBUG("Ignore 0 id broadcast");
        }
    }

    return false;
}

ErrorCode NextHopRouter::sendRelay(meshtastic_MeshPacket *p, uint8_t nextHop)
{
    p->relay_node = nodeDB->getLastByteOfNodeNum(getNodeNum());
    wasSeenRecently(p);
    p->next_hop = nextHop;
    LOG_DEBUG("Setting SR next hop for relayed packet with dest %x to %x", p->to, p->next_hop);

#if !MESHTASTIC_EXCLUDE_SIGNALROUTING
    UnicastSlotFlags flags{};
    bool destSr = false;
    bool lastHop = false;
    if (signalRoutingModule) {
        flags = signalRoutingModule->unicastCommitFlags(p->id);
        destSr = signalRoutingModule->isSignalRoutingNode(p->to);
        lastHop = signalRoutingModule->hasPricedDeliveryHop(p->to);
    }
    if (p->hop_limit == 0 && !lastHop) {
        return Router::send(p);
    }
    if (lastHop) {
        if (!p->want_ack || destSr || flags.lastHopBackup) {
            return Router::send(p);
        }
        startRetransmission(packetPool.allocCopy(*p), NUM_RELIABLE_RETX, false);
        return Router::send(p);
    }
    if (flags.nonfinalFlood || p->next_hop == NO_NEXT_HOP_PREFERENCE) {
        return Router::send(p);
    }
    PendingPacket *rec = startRetransmission(packetPool.allocCopy(*p), 2, true);
    if (rec && signalRoutingModule && iface) {
        uint32_t airtime = iface->getPacketTime(p);
        rec->nextTxMsec = millis() + signalRoutingModule->nextHopCarryWaitMs(flags.nominatedNextHop, airtime, p->rx_snr);
        setReceivedMessage();
    }
#else
    if (p->next_hop != NO_NEXT_HOP_PREFERENCE && (p->hop_limit > 0 || p->want_ack))
        startRetransmission(packetPool.allocCopy(*p));
#endif
    return Router::send(p);
}

/**
 * Get the next hop for a destination, given the relay node
 * @return the node number of the next hop, 0 if no preference (fallback to FloodingRouter)
 */
std::optional<uint8_t> NextHopRouter::getNextHop(NodeNum to, uint8_t relay_node)
{
    if (isBroadcast(to))
        return std::nullopt;

    meshtastic_NodeInfoLite *node = nodeDB->getMeshNode(to);
    if (node && node->next_hop) {
        // We are careful not to return the relay node as the next hop
        if (node->next_hop != relay_node) {
            // LOG_DEBUG("Next hop for 0x%x is 0x%x", to, node->next_hop);
            return node->next_hop;
        } else
            LOG_WARN("Next hop for 0x%x is 0x%x, same as relayer; set no pref", to, node->next_hop);
    }
    return std::nullopt;
}

PendingPacket *NextHopRouter::findPendingPacket(GlobalPacketId key)
{
    auto old = pending.find(key); // If we have an old record, someone messed up because id got reused
    if (old != pending.end()) {
        return &old->second;
    } else
        return NULL;
}

/**
 * Stop any retransmissions we are doing of the specified node/packet ID pair
 */
bool NextHopRouter::stopRetransmission(NodeNum from, PacketId id)
{
    auto key = GlobalPacketId(from, id);
    return stopRetransmission(key);
}

bool NextHopRouter::roleAllowsCancelingFromTxQueue(const meshtastic_MeshPacket *p)
{
    // Return true if we're allowed to cancel a packet in the txQueue (so we may never transmit it even once)

    // Return false for roles like ROUTER, ROUTER_LATE which should always transmit the packet at least once.

    return roleAllowsCancelingDupe(p); // same logic as FloodingRouter::roleAllowsCancelingDupe
}

bool NextHopRouter::stopRetransmission(GlobalPacketId key)
{
    auto old = findPendingPacket(key);
    if (old) {
        auto p = old->packet;
        /* Only when we already transmitted a packet via LoRa, we will cancel the packet in the Tx queue
          to avoid canceling a transmission if it was ACKed super fast via MQTT */
        if (old->numRetransmissions < NUM_RELIABLE_RETX - 1) {
            // We only cancel it if we are the original sender or if we're not a router(_late)
            if (isFromUs(p) || roleAllowsCancelingFromTxQueue(p)) {
                // remove the 'original' (identified by originator and packet->id) from the txqueue and free it
                cancelSending(getFrom(p), p->id);
            }
        }

        // Regardless of whether or not we canceled this packet from the txQueue, remove it from our pending list so it
        // doesn't get scheduled again. (This is the core of stopRetransmission.)
        auto numErased = pending.erase(key);
        assert(numErased == 1);

        // When we remove an entry from pending, always be sure to release the copy of the packet that was allocated in the
        // call to startRetransmission.
        packetPool.release(p);

        return true;
    } else
        return false;
}

/**
 * Add p to the list of packets to retransmit occasionally.  We will free it once we stop retransmitting.
 */
PendingPacket *NextHopRouter::startRetransmission(meshtastic_MeshPacket *p, uint8_t numReTx, bool floodOnLast)
{
    auto id = GlobalPacketId(p);
    auto rec = PendingPacket(p, numReTx, floodOnLast);

    stopRetransmission(getFrom(p), p->id);

    setNextTx(&rec);
    pending[id] = rec;

    return &pending[id];
}

/**
 * Do any retransmissions that are scheduled (FIXME - for the time being called from loop)
 */
int32_t NextHopRouter::doRetransmissions()
{
    uint32_t now = millis();
    int32_t d = INT32_MAX;

    // FIXME, we should use a better datastructure rather than walking through this map.
    // for(auto el: pending) {
    for (auto it = pending.begin(), nextIt = it; it != pending.end(); it = nextIt) {
        ++nextIt; // we use this odd pattern because we might be deleting it...
        auto &p = it->second;

        bool stillValid = true; // assume we'll keep this record around

        // FIXME, handle 51 day rolloever here!!!
        if (p.nextTxMsec <= now) {
            if (p.numRetransmissions == 0) {
                if (isFromUs(p.packet)) {
                    LOG_DEBUG("Reliable send failed, returning a nak for fr=0x%x,to=0x%x,id=0x%x", p.packet->from, p.packet->to,
                              p.packet->id);
                    sendAckNak(meshtastic_Routing_Error_MAX_RETRANSMIT, getFrom(p.packet), p.packet->id, p.packet->channel);
                }
                // Note: we don't stop retransmission here, instead the Nak packet gets processed in sniffReceived
                stopRetransmission(it->first);
                stillValid = false; // just deleted it
            } else {
                LOG_DEBUG("Sending retransmission fr=0x%x,to=0x%x,id=0x%x, tries left=%d", p.packet->from, p.packet->to,
                          p.packet->id, p.numRetransmissions);

                if (!isBroadcast(p.packet->to)) {
                    if (p.numRetransmissions == 1 && p.floodOnLast) {
                        // Last retransmission of a named forward: release to flooding.
                        p.packet->next_hop = NO_NEXT_HOP_PREFERENCE;
                        if (isFromUs(p.packet)) {
                            meshtastic_NodeInfoLite *sentTo = nodeDB->getMeshNode(p.packet->to);
                            if (sentTo) {
                                LOG_INFO("Resetting next hop for packet with dest 0x%x\n", p.packet->to);
                                sentTo->next_hop = NO_NEXT_HOP_PREFERENCE;
                            }
                        }
                        FloodingRouter::send(packetPool.allocCopy(*p.packet));
                    } else if (!isFromUs(p.packet)) {
                        // Replay the header sendRelay already stamped. NextHopRouter::send
                        // would recompute next_hop and startRetransmission() would replace
                        // this record, so the directed tries never happened.
                        FloodingRouter::send(packetPool.allocCopy(*p.packet));
                    } else {
                        NextHopRouter::send(packetPool.allocCopy(*p.packet));
                    }
                } else {
                    // Note: we call the superclass version because we don't want to have our version of send() add a new
                    // retransmission record
                    FloodingRouter::send(packetPool.allocCopy(*p.packet));
                }

                // Queue again
                --p.numRetransmissions;
                setNextTx(&p);
            }
        }

        if (stillValid) {
            // Update our desired sleep delay
            int32_t t = p.nextTxMsec - now;

            d = min(t, d);
        }
    }

    return d;
}

void NextHopRouter::setNextTx(PendingPacket *pending)
{
    assert(iface);
    auto d = iface->getRetransmissionMsec(pending->packet);
    pending->nextTxMsec = millis() + d;
    LOG_DEBUG("Setting next retransmission in %u msecs: ", d);
    printPacket("", pending->packet);
    setReceivedMessage(); // Run ASAP, so we can figure out our correct sleep time
}
