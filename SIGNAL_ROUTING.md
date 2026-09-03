# SignalRouting Algorithm

## Overview

SignalRouting (SR) is an advanced mesh networking protocol for Meshtastic that fundamentally transforms packet routing from traditional flooding-based approaches to intelligent, graph-based coordination. Unlike stock firmware's simple broadcast flooding or basic next-hop forwarding, SR maintains a comprehensive network topology graph using Expected Transmission Count (ETX) metrics to make optimal routing decisions.

### Key Differentiators from Stock Routing

**Stock Firmware Behavior:**
- **Broadcasts**: SNR-based flooding with duplicate suppression - nodes retransmit with random delays, but cancel retransmissions when hearing others transmit first
- **Unicasts**: Simple next-hop forwarding based on learned routes or direct connectivity
- **Coordination**: Limited duplicate detection prevents some redundancy, but still allows broadcast storms in dense networks

**SignalRouting Behavior:**
- **Broadcasts**: Intelligent relay coordination to prevent redundant transmissions while ensuring coverage
- **Unicasts**: Graph-based multi-hop routing with ETX optimization, plus coordinated relay selection through overhearing
- **Coordination**: Deterministic slot-based algorithm where nodes independently compute relay ordering and schedule transmissions

### SR's Dual Nature

SR operates in two complementary modes:

1. **Intelligent Routing**: Uses Dijkstra algorithm on ETX-weighted topology graphs for optimal path selection
2. **Coordinated Relay Selection**: For both broadcasts and unicasts, SR may broadcast packets to coordinate which nodes should relay them, preventing the redundant transmissions that plague traditional flooding approaches

This dual approach provides the reliability of coordinated networking with the efficiency of graph-based routing, significantly improving network performance over both random flooding and simple forwarding schemes.

## Comparison with Other Routing Approaches

### vs Stock Flooding (FloodingRouter)

**Stock Flooding Problems:**
- **Residual Redundancy**: Despite duplicate suppression, broadcast storms still occur in dense networks due to timing windows and SNR variations
- **Inefficiency**: Limited coordination means wasted transmissions when multiple nodes retransmit before suppression takes effect
- **Unpredictability**: SNR-based delays provide basic prioritization but don't account for overall network topology or coverage optimization
- **No Learning**: Static behavior regardless of network conditions

**SignalRouting Solution:**
- **Coordination**: Deterministic slot-based scheduling assigns relay order based on coverage analysis
- **Topology Awareness**: Uses ETX metrics and graph knowledge to select best relay candidates
- **Slot Scheduling**: Nodes independently compute identical relay ordering, eliminating collision between SR nodes
- **Legacy Integration**: Stock ROUTER/REPEATER nodes get earliest slots for compatibility

**Key Advantage**: Transforms chaotic flooding into orchestrated relay selection, significantly reducing redundant transmissions while maintaining reliable coverage.

### vs NextHopRouter

**NextHopRouter Limitations:**
- **Reactive Learning**: Only discovers routes through ACK observations, slow convergence
- **Single Path**: Learns one next-hop per destination, no alternative route awareness
- **No Coordination**: Each node forwards independently, potential for redundant unicast paths
- **Link Quality Ignorance**: Doesn't consider ETX or multi-hop path optimization

**SignalRouting Advantages:**
- **Proactive Topology**: Maintains complete network graph through multiple discovery mechanisms
- **ETX Optimization**: Uses Expected Transmission Count for true link quality assessment
- **Multi-hop Intelligence**: Dijkstra algorithm finds optimal paths across the entire network
- **Coordinated Unicasts**: Enables intelligent relay selection through overhearing unicast transmissions

**Key Advantage**: Transforms unicast routing from simple forwarding to intelligent, network-aware path selection with optional coordination for complex scenarios.

## Node Role Behavior Comparison

### Rebroadcast Behavior by Role

| Role | Stock Flooding | With SignalRouting |
|------|----------------|-------------------|
| **CLIENT_MUTE** | Never rebroadcasts | **Topology broadcasts only** - Announces direct neighbors to help active SR nodes (simplified: no graph maintenance, no complex topology inference) |
| **CLIENT** | Rebroadcasts, can cancel duplicates | Uses SR coordination for broadcasts |
| **ROUTER/ROUTER_LATE** | Always rebroadcasts, never cancels | **Priority relays** - SR gives them highest priority as they always rebroadcast |
| **ROUTER_CLIENT** | Rebroadcasts, can cancel duplicates | Uses SR coordination for broadcasts |
| **REPEATER** | Rebroadcasts, can cancel duplicates | **Priority relays** - SR gives them early slots since they typically rebroadcast |
| **CLIENT_BASE** | Special: acts as ROUTER for favorited nodes | Uses SR coordination for broadcasts |
| **TRACKER/SENSOR/TAK** | Rebroadcasts, can cancel duplicates | Treated as legacy, no SR participation |

**Legacy Node Priority:** SignalRouting prioritizes ROUTER and REPEATER roles because these nodes are configured to always rebroadcast packets. CLIENT_MUTE nodes participate minimally by broadcasting their direct neighbor topology to assist active SignalRouting nodes in making informed unicast routing decisions.

### Retransmission Behavior

| Router Type | Unicast Retransmissions | Notes |
|-------------|------------------------|-------|
| **ReliableRouter** | Up to 3 retransmissions | For want_ack packets only |
| **NextHopRouter** | 2 for intermediate hops, 3 for origin | Route reset on final failure |
| **SignalRouting** | Iterative unicast route selection with fallback strategies | For SR-selected unicast routes |
| **SignalRouting broadcast** | One T1 retransmit if no relay heard within ROUTER_LATE window | Guards against interference/CRC loss at receivers; see T1 Retransmit Insurance |

### Passive Node Behavior

Passive SR nodes (TRACKER, SENSOR, TAK, or non-active-routing configured nodes) participate minimally in SR:

1. **Topology Maintenance**: Only track directly-heard neighbors (via `isDirectPacket()` — hopStart == hopLimit and relay_node matches sender)
2. **SR Broadcasts**: Only process topology broadcasts from direct senders
3. **Node Activity**: Only update activity for direct packets; ignore relayed packets
4. **Graph Scope**: Limited to Level 1 neighbors - no multi-hop topology
5. **Memory Usage**: Minimal - only stores direct connections
6. **Broadcasting**: Send SR broadcasts with direct neighbors only
7. **Routing**: Use standard flooding for routing decisions

**Benefit**: Passive nodes participate in network awareness without the overhead of maintaining complex multi-hop topology, ideal for resource-constrained devices.

### Routing Delays and Timing

| Router Type | Broadcast Delays | Unicast Timing |
|-------------|-----------------|---------------|
| **FloodingRouter** | SNR-based delays (poorer SNR = shorter delay) | Immediate send |
| **NextHopRouter** | SNR-based delays + next hop preference | iface->getRetransmissionMsec() timing |
| **SignalRouting** | Slot-based: half-airtime per slot, deterministic candidate ordering | ETX-based route selection + speculative retransmit |

## Direct Neighbor Detection

### Reliable Detection Method: isDirectPacket()

SignalRouting determines if a packet was received directly from the sender using a two-part check in `isDirectPacket()`:

```cpp
bool SignalRoutingModule::isDirectPacket(const meshtastic_MeshPacket &mp)
{
    if (mp.hop_start != mp.hop_limit)
        return false;
    // If relay_node is set and doesn't match the sender's last byte,
    // a different node relayed this (stock nodes may not decrement hop_limit)
    if (mp.relay_node != 0 && mp.relay_node != (mp.from & 0xFF))
        return false;
    return true;
}
```

**How it works:**
1. **Hop counter check** (`hop_start == hop_limit`): When a sender transmits a packet, `hop_start` is set to the initial hop limit. Each relay decrements `hop_limit`. If they are equal, the packet has not been decremented by any relay.
2. **Relay node check** (`relay_node` vs `from & 0xFF`): If `relay_node` is set and does not match the originator's last byte, a different node relayed this packet. This catches cases where stock firmware nodes relay without decrementing `hop_limit`.

Both checks must pass for the packet to be considered direct. The hop counter alone is necessary but not sufficient because stock nodes may not always decrement `hop_limit` when relaying.

**Impact on Topology:**
- Passive nodes only add neighbors from direct packets (passing both checks) to their graph
- Passive nodes skip all relayed packets
- This ensures passive node topology shows only Level 1 neighbors

ETX measures the expected number of transmissions needed to successfully deliver a packet over a wireless link. It's calculated from RSSI (Received Signal Strength Indicator) and SNR (Signal-to-Noise Ratio) measurements:

- **ETX = 1 / (Delivery Probability)**
- Lower ETX = Better link quality
- ETX of 1.0 = Perfect link (100% delivery probability)
- ETX > 1.0 = Lossy link requiring retransmissions

### Topology Graph

SignalRouting maintains a network topology graph where:
- **Nodes** = Mesh devices
- **Edges** = Wireless links with ETX weights
- **Sources** = Reported (measured by peer) vs Mirrored (estimated from our perspective)

#### Topology Discovery Mechanisms

SignalRouting discovers network topology through multiple channels. The scope of topology discovery depends on the node's routing role:

**Active Routing Nodes (ROUTER, REPEATER, CLIENT, etc.):**
All discovery mechanisms are used to maintain comprehensive network topology:

1. **Direct Neighbor Detection**: When receiving packets directly with signal data (RSSI/SNR), immediate neighbor relationships are established with calculated ETX values

2. **Topology Broadcasts**: Periodically broadcast their complete neighbor list using a packed binary format (8 bytes/neighbor) for comprehensive topology learning from other nodes' perspectives. Each entry carries the neighbor's node ID, RSSI, SNR, capability flags, and ETX variance — giving every node the same link stability view. **Transmit path:** a local `directSignals[]` side table (sized to `NEIGHBOR_GRAPH_MAX_EDGES_PER_NODE`, same as direct edge slots) caches the real measured RSSI/SNR from direct RX (`updateNeighborInfo()` only); topology packing reads from that table rather than approximating signal from stored ETX. **Receive path:** mirrored/downstream links remain ETX-only in RAM; merge still uses `calculateETX(reported_rssi, reported_snr)`. The `hop_limit` of every SR topology packet is capped at `cfgBroadcastMaxHops` (default `SR_BROADCAST_MAX_HOPS`), even if the user has configured a higher limit, to bound topology gossip propagation in large networks.

3. **Relayed Packet Inference**: When receiving a relayed packet, gateway relationships are inferred between the original sender and the relay node. If the relay node is a stock (Legacy) firmware node, a directed edge is also recorded from the relay to the sender — observing a successful relay proves the relay can hear the sender, regardless of the sender's firmware type. The reverse edge is not assumed.

4. **Placeholder System**: Unknown relay nodes are tracked as placeholders until their real identities are discovered through direct contact

5. **Gateway Relationship Tracking**: Downstream relationships learned when packets flow through relay nodes, enabling multi-hop route discovery

**Passive Routing Nodes (TRACKER, SENSOR, TAK, and nodes not configured for active routing):**
Passive nodes maintain a simplified Level 1 topology containing ONLY directly-heard neighbors:

1. **Direct Neighbor Detection**: Only neighbors heard directly (`isDirectPacket()` — hopStart == hopLimit and relay_node matches sender, with signal data) are tracked with measured ETX values

2. **SR Broadcast Reception**: Only process topology broadcasts from direct senders (`isDirectPacket()`), preventing ingestion of remote network topology

3. **Packet Activity Tracking**: Only update node activity for direct packets; relayed packets are ignored

4. **Placeholder System**: Limited to placeholders for direct relay nodes; upstream placeholder resolution skipped

5. **No Gateway Inference**: Downstream relationships and multi-hop topology are not tracked

**Mute Node Topology Sharing**: 
CLIENT_MUTE nodes broadcast their direct neighbor information to help active SignalRouting nodes discover network topology, even though mute nodes don't participate in packet relaying. Active nodes learn about mute node neighbors for discovery purposes but don't consider routing paths through mute nodes since they don't relay. CLIENT_MUTE nodes maintain their direct neighbor graph (add/remove expired connections) but use simplified topology tracking.

**Key Benefit of Passive Mode:**
By limiting topology to Level 1 direct neighbors, passive nodes minimize memory usage and processing overhead while still participating in SR broadcasts with local network knowledge. They don't require complex multi-hop routing calculations and reduce bandwidth consumed by topology learning, making them ideal for resource-constrained devices while still contributing to network awareness.

## Unicast Routing

### Route Calculation Algorithm

SignalRouting calculates unicast routes and coordinates delivery for reliable packet transmission:

```cpp
NodeNum SignalRoutingModule::getNextHop(NodeNum destination, ...) {
    // 1. Check direct connection (1-hop route)
    const NodeEdges* myEdges = routingGraph->getEdgesFrom(myNode);
    for each direct neighbor:
        if (neighbor == destination) return destination; // Direct route

    // 2. Find multi-hop routes using Dijkstra over the full edge graph
    //    Traverses all known edges (direct + topology-learned) for optimal path
    //    Falls back to downstream table only for nodes not in edge graph
    Route route = routingGraph->calculateRoute(destination, currentTime);
    if (route.nextHop != 0) {
        return route.nextHop; // Multi-hop route found
    }

    // 3. Fallback options: gateway routes, opportunistic forwarding
    return findBestFallbackOption(destination, ...);
}

// Special handling for relayed unicast packets to direct neighbors
bool shouldDeliverDirectToNeighbor(NodeNum destination, NodeNum heardFrom) {
    // If we received this as a relayed packet and destination is our direct neighbor,
    // deliver directly since the destination didn't hear the original transmission
    if (heardFrom != sourceNode && isDirectNeighbor(destination)) {
        // Check if better positioned neighbors exist before delivering directly
        if (!hasBetterPositionedNeighborForDirectDelivery(destination, ourRouteCost)) {
            return destination; // Deliver directly
        }
    }
    return 0; // Use normal routing
}
```

### Unicast Route Selection Priority

When deciding whether to use SR coordination for unicast packets:

1. **Any Route Check**: If we have ANY calculated route to the destination (regardless of cost), use SR coordination
2. **Topology Health**: Verify destination is known in the network topology
3. **Gateway Preferences**: Prefer routes through gateways we can reach directly
4. **Next Hop Capability**: Ensure next hop is SR-capable or legacy router
5. **Designated Gateway Check**: Defer to designated gateways when applicable
6. **Opportunistic Forwarding**: Use when topology is unhealthy or routes unavailable

### Unknown Destination Suppression

When a unicast packet arrives for a destination that is not in the SR graph (neither as a direct edge, downstream entry, nor Dijkstra-routable) **and** not in NodeDB, SR suppresses the relay entirely rather than falling back to broadcast-style delivery. Blindly relaying packets for completely unknown nodes wastes airtime with no reasonable chance of delivery.

Destinations that are known through any mechanism are still relayed:
- Present in NodeDB (legacy/stock nodes not in SR graph)
- Present in the downstream table (reachable via a relay's topology report)
- Routable via Dijkstra (direct or multi-hop SR path)

`isNodeRoutable()` filters the intermediate hops Dijkstra may use: SR-passive nodes (CLIENT_MUTE, TRACKER, SENSOR, TAK, CLIENT_HIDDEN) broadcast topology but never relay, so they are never hops, and legacy nodes qualify only as ROUTER/REPEATER/ROUTER_CLIENT. The filter is not applied to the destination, so a passive node stays reachable.

### Last-Hop Unicast Hop Limiting

`getUnicastHopLimitForDirectNeighbor()` limits hops on unicasts destined for a direct `hearsUs` neighbor when stock neighbors are present, preventing unnecessary stock relay. It returns -1 (don't limit) or the hop_limit to set, based on link quality:

| Link quality | ETX | hop_limit | Behavior |
|-------------|-----|-----------|----------|
| Good | < 3.0 | 0 | Direct delivery only, no further relay |
| Marginal | ≥ 3.0 | 1 | Allow one retry relay if our TX is lost |

**Conditions** (all must be true):
1. The packet is a unicast (not broadcast)
2. The destination is a **direct neighbor** with `hearsUs = true`
3. At least one other direct neighbor is **not SR-active** (stock firmware)

When **all** other direct neighbors are SR-active, hop limiting is skipped — SR nodes suppress relays themselves via the slot-based algorithm.

`hop_start` is adjusted to preserve correct `hopsAway` calculation (`hop_start - hop_limit`) for receivers:
- **Originated packets**: `hop_start = hop_limit` (standard convention)
- **Relayed packets**: `hop_start = original_hops_taken + limitedHops + 1`

The check is applied in two places:
- **`NextHopRouter::perhapsRebroadcast()`** — when relaying a unicast we received from another node
- **`Router::send()`** — when originating a unicast ourselves

Log line emitted in both cases:
```
[SR] Limiting hop_limit=<N> for unicast relay 0x<id>: dest is direct hearsUs neighbor, stock neighbors present
[SR] Limiting hop_limit=<N> for originated unicast 0x<id>: dest is direct hearsUs neighbor, stock neighbors present
```

### Speculative Retransmission

For unicast packets, SR coordinates relay decisions on overhearing nodes using the same slot-based algorithm as broadcasts:
- Overhearing nodes independently compute candidate ranking by ETX-to-destination
- The best-positioned node transmits first; others cancel on hearing the dupe
- Originating node retransmission handled by standard Router mechanisms

Unicast relay failures are detected end-to-end: the original sender retransmits if it doesn't receive an ACK, rather than SR immediately trying an alternative relay.

### Unicast Relay Coordination

SignalRouting uses the same slot-based scheduling algorithm for unicasts as for broadcasts. Nodes that overhear a unicast packet independently compute a relay candidate ranking and schedule their TX at the assigned slot delay. If any node transmits before our slot fires, we cancel our queued relay unconditionally.

**Coordination Through Overhearing:**
When a unicast packet is transmitted, nodes that overhear it can participate in relay coordination:

1. **Overhearing Mechanism**: Unicast packets are transmitted with unencrypted headers visible to all nodes
2. **Intermediate Participation**: Non-destination nodes that overhear can run the slot scheduling logic
3. **Distributed Decision**: Each node independently computes the same candidate ordering
4. **Optimal Selection**: Best-positioned relay (lowest ETX to destination) gets the earliest slot

**Slot 0 — Designated Next Hop:**
If the packet carries a `next_hop` field (set by stock NextHopRouter), slot 0 is reserved for that designated node. Any SR node whose last-byte matches `p->next_hop` relays immediately at slot 0. All other SR nodes start their candidate ranking from slot 1. If `next_hop` is not set (`NO_NEXT_HOP_PREFERENCE`), SR candidates start from slot 0.

**Candidate Cost Metric (SR candidates, slot 1+):**
For each candidate (self + SR-active direct neighbors):
- Direct edge to destination → raw `etxFixed` (range ~100–32767)
- Edge to the shared next hop (indirect) → `etxFixed | 0x8000` — always sorts after direct-edge candidates
- No usable path → excluded

This ensures last-hop delivery nodes (direct edge to destination) are always scheduled before intermediate relays, and within each group the lower ETX wins.

**Quick Suppression Checks (before slot scheduling):**
- Source and destination both downstream of the same relay → suppress
- `heardFrom` already has a direct edge or downstream relay path to destination → suppress
- An SR neighbor that covers `heardFrom` can reach destination → suppress

**Dupe Cancellation:**
When a dupe arrives for a committed unicast relay, `areAllNeighborsCovered()` evaluates whether the dupe relayer can reach the destination. If the dupe relayer has a direct edge or downstream path to the destination, our queued TX is canceled — the slot-based ordering guarantees earlier transmitters are better positioned. However, if the dupe relayer cannot reach the destination but we can (direct edge or downstream relay), our relay is kept to ensure delivery.

## Broadcast Routing

### Slot-Based Relay Coordination

SignalRouting uses a deterministic slot-based algorithm to coordinate broadcast relays. Each node independently computes the same candidate ordering and assigns time slots spaced by half the packet airtime. Nodes schedule their TX at their assigned slot and rely on existing dupe suppression: if a node hears the packet relayed before its slot fires, it cancels its queued transmission.

**Algorithm phases:**

0. **Pre-coverage**: heardFrom's direct neighbors with ETX < 7.0 are marked as already covered. Poorly-linked neighbors (ETX ≥ 7.0, i.e. below noise floor) are excluded and may still need relaying. The ETX=40 sentinel marks stale/unknown edges and is always excluded.

1. **Candidate filtering**: Only SR-active neighbors and stock ROUTER/REPEATER/ROUTER_CLIENT nodes are considered relay candidates. Non-SR nodes (CLIENT, CLIENT_MUTE, etc.) are excluded — they either don't relay or relay unpredictably outside SR coordination.

2. **Stock routers first**: Legacy ROUTER/REPEATER/ROUTER_CLIENT neighbors that can hear the transmitter get the earliest slots since they transmit regardless of SR decisions. Stock routers with no evidence of hearing the transmitter (no edge in the topology graph) are skipped — reserving a slot for them would just delay our own TX for nothing. If they already transmitted, their coverage is absorbed.

3. **SR candidate ranking**: Remaining SR-active candidates are iteratively ranked by `findBestRelayCandidate` (most unique coverage, lowest ETX, deterministic node ID tiebreak based on packet ID parity). Each candidate gets the next slot. If it's us, we schedule TX and stop. If a candidate already transmitted, we absorb its coverage without consuming a slot. An earlier slot holder that has not transmitted yet is assumed to relay, so its coverage is absorbed too before the next pick. Our own coverage counts only our **Reported** edges (what we broadcast in topology): peers rank us on what they mirrored from us, and counting our Mirrored edges made every node see more coverage for itself than its neighbours saw for it, so colocated nodes both took slot 0.

4. **Overrides**: Downstream relay obligations and stock coverage needs can force a relay. The stock-coverage fallback is coordinated across the SR peers that took part in the ranking: a mute or legacy neighbour already covered by an earlier slot holder needs nobody else, and each remaining uncovered stock neighbour is owned by exactly one SR node, the lowest node id among us and the peers with an edge to it. Without that rule every SR node beside the same mute neighbour relayed in the same slot.

**Post-cancellation — unique coverage re-evaluation:**
When a dupe arrives, `FloodingRouter::perhapsCancelDupe` is called. For SR committed relays, it first checks whether the packet is still in the TX queue (`findInTxQueue`):

- **Already transmitted** (not in TX queue): the packet has already been sent; nothing to cancel. Log "Already relayed — ignoring dupe" and return immediately. Coverage computation is skipped entirely since it would be wasted work.
- **Still pending** (in TX queue): call `areAllNeighborsCovered(p)` to evaluate whether the dupe relayer now covers all our neighbors. Transmitters are accumulated across all dupes for the same packet (original `heardFrom` + all subsequent dupe relayers). If all neighbors are covered → cancel and clear the committed relay. If we still have unique coverage → keep our scheduled relay.

Note: a packet is removed from the TX queue at `dequeue()`, which happens immediately before `startSend()` — not after transmission completes. So `findInTxQueue` returns false as soon as the packet starts transmitting.

Because Phase 2 assigns each SR candidate a unique sequential slot, two nodes with unique coverage will never collide when both keep their relays.

```
Slot timing example (150ms half-airtime):

  Slot 0 (0ms):    Stock router R1
  Slot 1 (150ms):  Stock router R2
  Slot 2 (300ms):  Best SR candidate (most unique coverage)
  Slot 3 (450ms):  Next SR candidate
  ...
```

**Deterministic tiebreak**: When two candidates have identical coverage and cost, the winner is determined by node ID direction based on packet ID parity (even → lowest ID, odd → highest ID). This distributes relay duty evenly across nodes.

### How Slot Spacing Works

Slots are spaced by half the packet airtime. When slot 0 starts transmitting, slot 1's radio detects the ongoing reception (`busyRx`) and holds. After slot 0's packet is received, dupe detection cancels slot 1's queued relay if the coverage is redundant. Half-airtime spacing provides enough margin for preamble detection while keeping propagation fast.

### Topology Edge Persistence

Mirrored edges (learned from topology broadcasts) are not cleared when a new topology version arrives. They age out naturally. This prevents valid neighbor knowledge from being lost between broadcasts — for example, a stock node that both SR neighbors can hear would otherwise be forgotten each time they exchange topology, causing false "unique coverage" decisions.

### Relay Decision Factors

1. **Bidirectional Link Priority**: Candidates with a confirmed round-trip link (`hearsUs=true`) to the packet source get tier 1; others get tier 0. Tier is the highest-priority criterion — a bidi candidate always wins over a non-bidi candidate regardless of coverage. Within the same tier, normal ranking applies. An ETX ceiling (20.0) prevents marginal links (e.g., ETX=40 at RSSI=-110) from qualifying for the bidi boost. See [Asymmetric Link Handling](#asymmetric-link-handling).
2. **Stock Router Priority**: Legacy routers/repeaters get earliest slots if they can hear the transmitter
3. **Deterministic Ordering**: All nodes compute the same candidate ranking for consistent slot assignment
4. **Coverage-Based Selection**: Candidates ranked by unique coverage count, then ETX quality
5. **Dupe Suppression**: Existing packet deduplication cancels queued relays when earlier slots transmit
6. **Unique Coverage Fallback**: Nodes covering areas no candidate reaches are forced to relay via downstream override or stock coverage checks
7. **Downstream Override**: Nodes recorded as relay for source/destination are forced to relay

### Broadcast Coordination Example

```
Network: A ── B(SR) ── C
           │     │
           D(SR) ── E

Packet from A to BROADCAST (halfAirtime = 150ms):

1. A transmits
2. B and D both hear it, build identical candidate lists
3. B has more unique coverage (covers C and E) → B gets slot 0 (0ms)
4. D gets slot 1 (150ms)
5. B transmits at slot 0, D's radio detects busyRx and holds
6. D receives B's relay as dupe → checks coverage → B covered everything → cancels
7. Result: 1 relay, 0 redundant transmissions
```

**With stock router:**

```
Network: A ── R(stock router) ── C
           │         │
           B(SR) ─── D(SR)

Packet from A to BROADCAST (halfAirtime = 150ms):

1. A transmits
2. R, B, D all hear it
3. R gets slot 0 (0ms) — stock router priority
4. B ranked best SR candidate → slot 1 (150ms)
5. D gets slot 2 (300ms)
6. R transmits at slot 0 (stock behavior)
7. B receives R's relay, checks unique coverage — none → cancels
8. D receives R's relay, checks unique coverage — none → cancels
9. Result: 1 relay from stock router, SR nodes suppressed
```

### T1 Retransmit Insurance

LoRa is half-duplex: a transmitting node cannot hear its own channel while sending, so it has no way to know whether nearby receivers successfully decoded the packet. Radio interference, overlapping transmissions, or transient noise can corrupt a packet's CRC at any receiver — including a neighbor that would have relayed it. T1 retransmit insurance guards against this by scheduling one additional transmission after the initial send, fired only if no relay is heard within the worst-case relay window.

**When T1 fires:**
- T1 is scheduled immediately when `FloodingRouter::send()` forwards a broadcast (originator path) or when `commitRelay()` records a committed SR relay.
- The delay is `getTxDelayMsecWeightedWorst(SNR=+10) + one-packet-airtime` — this covers the longest possible ROUTER_LATE window, so any node that successfully received T0 has already had its full opportunity to relay before T1 goes out.
- T1 only fires if no dupe relay is heard before the timer expires. If at least one neighbor relayed T0 cleanly, T1 is canceled and no extra airtime is used.

**Conditions for T1 to be scheduled:**
1. The packet is a broadcast.
2. The node originated the packet or SR committed it to relay.
3. At least one direct neighbor with `hearsUs=true` exists — confirms we have a known neighbor before spending airtime on the retransmit.
4. T1 retransmit is not disabled via config (`t1_retransmit_enabled`).

**Cancellation:** Most incoming dupes trigger `cancelBroadcastRetransmit()` via `perhapsCancelDupe()` — including committed relays that decide to cancel, already-relayed detection, and non-SR originator dupes. The one exception is when a committed relay has unique coverage and keeps its queued TX: T1 is also preserved in this case, since the retransmit insurance is still needed if our relay fails.

**Graph-based pre-fire cancellation:** When the T1 timer expires, `allHearsUsNeighborsHeardPacket()` checks whether every `hearsUs` neighbor already received the packet — either because they are a known transmitter themselves, or because a known transmitter has a link to them in the SR graph (edge in either direction). If all `hearsUs` neighbors are accounted for, T1 is canceled without retransmitting. This prevents unnecessary T1 retransmits in the common case where neighbors already had the packet from another path and correctly suppressed our relay as a dupe.

**Guard against T2:** When T1 fires, `isRetransmitting = true` is set before calling `router->send()`. This prevents `maybeScheduleBroadcastRetransmit()` from scheduling a second retransmit when T1 re-enters the send path.

### Asymmetric Link Handling

LoRa links are frequently asymmetric — node A can hear node B but B cannot hear A, or the link quality differs significantly in each direction. SR addresses this at multiple levels:

**Downstream assignment**: When processing a topology broadcast from node X listing neighbor Y, SR only marks Y as downstream of X if X reports `hearsUs=true` for Y — meaning Y can hear X. Without this check, SR might route packets to Y via X even though X cannot deliver to Y.

**Authoritative hearsUs override**: A node is authoritative about who it can hear. When node Y broadcasts its topology and does NOT list node X as a neighbor, SR clears the `hearsUs` flag on the X→Y edge — even if X previously claimed bidirectionality. This corrects stale or incorrect `hearsUs` claims. The override may flip-flop until both nodes converge (X stops claiming bidi after observing Y's topology), but each cycle brings the graph closer to ground truth.

**hearsUs from topology listing**: The same authority works in the positive direction. When node Y's topology broadcast lists *us* as a directly heard neighbor, SR sets `hearsUs=true` on our edge to Y. A node only packs neighbors it has a measured direct RSSI/SNR for, so being listed is proof Y heard us on RF. This is the only way an SR-passive neighbor can earn the flag — it broadcasts topology but never relays, so it can never prove bidirectionality by relaying one of our packets. SR-active neighbors gain the flag on their first topology report instead of waiting for that relay to happen.

The flag matters in four places: T1 retransmit insurance (`hasAnyHearsUsNeighbor()` gates it, `allHearsUsNeighborsHeardPacket()` cancels it), last-hop unicast hop limiting, route approval when sender connectivity is otherwise unverified, and our own outbound topology — which is what gives us bidi tier-1 priority in *other* nodes' relay-slot selection when that neighbor is the packet source. It does not affect stock-neighbor coverage, which only counts `Legacy` nodes.

Implemented by `confirmTopologySenderHearsUs()` in `SignalRoutingModule.h`, called from both topology-processing paths. Stale claims are bounded from both sides: the authoritative override clears the flag when Y's next topology omits us, and `pruneCapabilityCache()` clears it when Y stops broadcasting altogether (capability TTL).

The listing is applied to peers as well (`confirmTopologySenderHearsNeighbor()`): every node the sender names that has already reported an edge to the sender gets `hearsUs` on that edge. Two colocated nodes that both just learned a passive neighbour otherwise each modelled the other as not covering it and both took slot 0 for its packets; with the peer's edge flagged from the same report their costs tie and the packet-parity tie-break picks one relayer on both nodes. No edge is invented.

**Bidi slot priority**: In `findBestRelayCandidate()`, candidates with a confirmed bidirectional link (`hearsUs=true`, ETX < 20.0) to the packet source get tier 1 priority. This ensures that on mixed SR/stock branches, the node with round-trip connectivity to the source relays first. Stock nodes on the branch then observe this relay and set their `next_hop` correctly — they never see the asymmetric-link node relay, so they never latch onto an undeliverable gateway.

**Example — mixed branch with asymmetric gateway**:
```
       NodeA (remote node)
      /    ╲
    NodeB   NodeC    NodeB = bidi link to NodeA, slightly weaker local signal
      \    /         NodeC = hears NodeA, but NodeA can't hear NodeC; better local signal
       NodeD
        |
      (branch nodes, mix of SR and stock)
```

Without bidi priority: NodeC relays first (better local ETX), NodeB's relay is canceled as dupe. Branch stock nodes set `next_hop=NodeC`. Unicasts to NodeA via NodeC fail — NodeA can't hear NodeC.

With bidi priority: NodeB gets tier 1 (bidi to NodeA), relays first. Stock nodes set `next_hop=NodeB`. Unicasts to NodeA succeed. NodeC handles other relay duties where its better local signal matters.

## Channel QoS

### Channel-Utilization-Based Relay Gating

`ChannelQoS` gradually drops lower priority relay traffic as channel utilization increases, preserving bandwidth for user messages and critical control traffic. It acts as a prerequisite gate in `perhapsRebroadcast()` — checked before SR's relay decision logic.

**Priority tiers** (lowest dropped first):

| Tier | Packet types | Drop relay above chutil |
|------|-------------|------------------------|
| LOW | Telemetry, position, nodeinfo, unknown/undecoded | 25% |
| MEDIUM | Text on non-primary channel | 30% |
| HIGH | Routing, SR routing, traceroute, ACKs | 38% |
| CRITICAL | Text on primary channel, admin | Never dropped |

Thresholds align with stock firmware's existing channel utilization limits (25% polite, 40% impolite). The QoS filter only gates relay decisions — packets addressed to us are always delivered locally.

`ChannelQoS` is orthogonal to `NodeRateLimiter` (per-node abuse detection) and stock `AirTime` TX blocking (blocks our originations). QoS handles aggregate channel load for relay decisions.

Guarded by `MESHTASTIC_EXCLUDE_CHANNEL_QOS` for memory-constrained targets.

## Inbound Rate Limiting

### Per-Node Packet Buckets

`NodeRateLimiter` protects the node from a single misbehaving or malfunctioning neighbor that floods the mesh. It is checked in `Router::handleReceived()` after decode but before `MeshModule::callModules()`, so a limited packet reaches no module: it is not relayed, not ACKed, and not delivered to the phone.

Each tracked source node gets three independent buckets:

| Bucket | Portnums | Packets per 90 s window |
|--------|----------|------------------------|
| TEXT | `TEXT_MESSAGE_APP`, `TEXT_MESSAGE_COMPRESSED_APP` | 30 |
| ROUTING | `ROUTING_APP`, `SIGNAL_ROUTING_APP`, `TRACEROUTE_APP` | 10 |
| OTHER | every other decoded portnum | 4 |
| UNKNOWN | undecodable packets: no key for the channel, PKI traffic for other nodes | 12 |

A relay without the key cannot tell chat from telemetry, while a key-holding relay judges the same packets by their real port; with the OTHER threshold a private group chatting normally flowed through key holders and died at the first relay without the key, and the TEXT threshold would let encrypted admin and DM traffic for others run at chat rates. UNKNOWN sits between the two and has no `moduleConfig` override; 12 lets a relayed remote-admin Channels screen (nine sequential requests) load within one window.

Once a bucket trips, every further packet in that bucket resets the window, so the source stays limited until it goes quiet for a full window. Up to 16 sources are tracked; when full, the entry with the greatest hop distance is evicted (ties broken by oldest window).

**Exemptions** — the limiter never acts on:

- Packets we originated (`isFromUs`).
- **Packets addressed to us (`isToUs`).** These are never relayed, so they cost no relay airtime, and they are the replies and requests we asked for: admin responses, DMs, ACKs. Dropping one silently kills the ACK and implicit-ACK path and the delivery to the phone. Because `ADMIN_APP` falls in the OTHER bucket, a remote node answering a burst of admin GETs tripped the limit on its 4th reply in 90 s, after which every further reply kept the window reset and remote admin never recovered.
- Nodes marked `is_favorite` in NodeDB.

Thresholds and the window are overridable via `moduleConfig.node_rate_limiter`; guarded by `MESHTASTIC_EXCLUDE_NODE_RATE_LIMITER` for memory-constrained targets.

## Benefits for Mesh Network Reliability

### Improved Deliverability

**Dense Node Scenarios:**
SignalRouting transforms chaotic broadcast flooding into deterministic relay scheduling. In dense networks where multiple nodes hear the same transmissions, SR's slot-based algorithm assigns ordered time slots to relay candidates based on coverage analysis and ETX metrics. Half-airtime slot spacing ensures that when the highest-priority node transmits, others detect the ongoing reception and suppress their own relay. This significantly reduces redundant transmissions compared to traditional SNR-based flooding, while maintaining reliable delivery. Coordination effectiveness depends on topology accuracy, with graceful fallback when graph information is incomplete.

### Mesh Branch Handling

**Gateway-Aware Routing:**
```
Network Gateway
     │
     G (Stock/Legacy Node)
    / \
   /   \
  A ── B
  │    │
  C ── D

SignalRouting considerations:
1. Identifies legacy nodes (G) and gives them priority in relay decisions
2. Routes unicasts through gateways when direct routes aren't available
3. Allows broadcasts from legacy gateways to be relayed by SignalRouting nodes for branch coverage
4. Attempts to minimize redundant relays within branches, but legacy nodes may still flood independently
```

### Reliability Improvements

**Link Quality Awareness:**
- Uses ETX metrics to prefer higher quality links for unicast routing
- Routes around known poor quality links when better alternatives exist
- May adapt to changing conditions through periodic topology updates

**Failure Recovery:**
- Speculative retransmission provides basic recovery for unicast packet loss
- Slot-based scheduling provides natural fallback: if a higher-priority node fails to transmit, the next slot's node simply doesn't hear a dupe and transmits on schedule
- Limited adaptation to sudden link failures without immediate topology updates

**Congestion Control:**
- Deterministic slot scheduling minimizes redundant broadcasts
- Half-airtime spacing between slots ensures physical separation of transmissions
- Downstream override and stock coverage checks ensure nodes with unique coverage still relay

## NeighborGraph Implementation

### Graph Architecture

The `NeighborGraph` class uses fixed-size arrays (~24 KB heap) and runs on all platforms including ESP32-C3, supporting 400+ mesh nodes with no dynamic allocation.

**Memory Structure:**
```cpp
class NeighborGraph {
    NodeEdges neighbors[24];              // Direct neighbor slots (24 nodes × 24 edges each)
    DownstreamEntry downstream[1100];     // Remote node routing table
    RelayState relayStates[32];           // Transmission tracking for contention
    Route routeCache[32];                 // Cached Dijkstra results
    // Total: ~24 KB fixed allocation
};
```

**Core Data Types:**

| Struct | Purpose |
|--------|---------|
| `Edge` | Link to a neighbor with ETX (fixed-point ×100), variance, source (Reported/Mirrored), timestamp |
| `NodeEdges` | A neighbor slot: nodeId + up to 24 edges + last full update time |
| `DownstreamEntry` | Remote node routing: (destination, relay, cost, lastUpdate) |
| `Route` | Cached route result: (destination, nextHop, cost, timestamp) |
| `RelayCandidate` | Relay selection: (nodeId, coverageCount, avgCost, tier) |
| `RelayState` | Tracks which nodes transmitted which packets for dupe detection |

**Graph Limits (compile-time configurable):**

| Parameter | Default | Purpose |
|-----------|---------|---------|
| `NEIGHBOR_GRAPH_MAX_NEIGHBORS` | 24 | Direct neighbor slots |
| `NEIGHBOR_GRAPH_MAX_EDGES_PER_NODE` | 24 | Max edges per neighbor |
| `NEIGHBOR_GRAPH_MAX_DOWNSTREAM` | 1100 | Remote node routing entries |
| `NEIGHBOR_GRAPH_MAX_RELAY_STATES` | 32 | Transmission tracking slots |
| `NEIGHBOR_GRAPH_MAX_CACHED_ROUTES` | 32 | Dijkstra result cache |

**Features:**
- Full Dijkstra algorithm for multi-hop route calculation
- Enhanced relay coordination with coverage analysis
- Downstream routing table for multi-hop node reachability
- Fixed memory footprint, no heap fragmentation
- Runs on all platforms including ESP32-C3 (272 KB RAM)

### Downstream Routing Table

The downstream table tracks which remote (multi-hop) nodes are reachable through which direct neighbor (relay):

```cpp
struct DownstreamEntry {
    NodeNum destination; // Remote node
    NodeNum relay;       // Which direct neighbor reaches it
    uint16_t costFixed;  // Cumulative ETX × 100
    uint32_t lastUpdate; // For aging
};
```

- Updated when SR topology broadcasts report neighbor lists from relay nodes
- Supports multiple relays per destination (entries with different relay fields)
- Used as **fallback only** when Dijkstra over the edge graph cannot reach the destination (e.g., non-SR nodes without topology edges). Dijkstra routes always take priority over downstream estimates.
- During placeholder resolution, entries are transferred to the real node via `transferDownstream()`

### Placeholder System

When a relayed packet arrives from an unknown node (identified only by its 8-bit relay byte), SR creates a **placeholder node** (ID `0xFF0000XX` where `XX` is the relay byte) to preserve topology relationships until the real identity is discovered:

1. **Creation**: When we directly hear a packet relayed by an unknown node
2. **Resolution**: When we later hear that same relay byte from a node whose full ID we now know (via direct contact)
3. **Transfer**: On resolution, all downstream entries are transferred from the placeholder to the real node via `transferDownstream()`, preserving routing continuity
4. **TTL**: Placeholders age out with `cfgNodeTtlSecs` if never resolved

**Duplicate resolution guard**: `resolvePlaceholder()` checks the relay-identity cache before doing any work. If the placeholder's relay byte is already mapped to the same `realNodeId`, the function returns `false` immediately with no log message and no graph operations. Previously the function would fall through and re-execute all transfers and re-log "Resolved placeholder" on every subsequent observation of the same relay, causing spurious log spam and redundant graph writes.

Placeholders are only created for nodes **we** hear directly — not for nodes reported by others in SR broadcasts.

### Relay Byte Matching

Unicast relay coordination uses relay byte matching (last byte of `p->relay_node`) to detect when the designated relay has already transmitted a packet. To prevent false positives from byte collisions across distant nodes, relay byte checks are **guarded by direct-edge verification**:

```cpp
// Only match relay bytes against nodes we have direct edges to
if (relayForDest != 0 && (relayForDest & 0xFF) == p->relay_node &&
    routingGraph->getEdgesFrom(relayForDest) != nullptr) {
    // Relay already transmitted — don't duplicate
    return false;
}
```

### Timing and Synchronization

SignalRouting uses **monotonic time** (time since boot) for all graph operations to ensure consistency:

- **Graph aging**: Edge expiration and node cleanup
- **Route caching**: Cache validity and expiration (`ROUTE_CACHE_TIMEOUT_SECS` — not runtime-configurable)
- **Transmission tracking**: Contention window timing
- **Edge timestamps**: Connection update times

This prevents issues with RTC updates that could cause time jumps, ensuring reliable graph maintenance and routing decisions.

**Hardware Adaptation:**
- **STM32WL**: SR disabled entirely (64KB RAM limit)
- **RP2040**: RAM guard (< 30KB free disables SR)
- **ESP32-C3/ESP32/all others**: NeighborGraph (~24 KB)

## Configuration and Tuning

### Runtime Config (`ModuleConfig.SignalRoutingConfig`)

All SR tuning parameters can be set at runtime via the Meshtastic admin interface or CLI without recompiling. Numeric fields use `0` / `0.0` to mean "use firmware default" (the compile-time constant). Bool fields follow proto3 conventions — see the note below.

| Protobuf field | Type | Firmware default (constant) | Description |
|----------------|------|----------------------------|-------------|
| `enabled` | bool | `true`* | Master enable/disable for SR |
| `t1_retransmit_enabled` | bool | `true`* | Enable T1 retransmit insurance (interference protection) |
| `topology_broadcast_secs` | uint32 | `SIGNAL_ROUTING_BROADCAST_SECS` | Topology broadcast interval |
| `dirty_broadcast_secs` | uint32 | `SIGNAL_ROUTING_DIRTY_BROADCAST_SECS` | Minimum interval before a dirty-topology early broadcast |
| `node_ttl_secs` | uint32 | `NODE_TTL_SECS` | Node aging TTL — nodes not heard within this window are removed from the graph |
| `broadcast_max_hops` | uint32 | `SR_BROADCAST_MAX_HOPS` | `hop_limit` cap applied to SR topology broadcast packets |
| `poor_link_etx_threshold` | float | `7.0` | ETX above which a link is excluded from pre-coverage marking in relay decisions |
| `etx_change_threshold` | float | `NeighborGraph::etxChangeThreshold` (default `1.0`) | Base ETX delta threshold; per-edge `etxVariance` is added so noisy links need bigger jumps to trigger dirty |

\* proto3 booleans default to `false` on the wire. When writing any SR config, always set `enabled=true` and `t1_retransmit_enabled=true` unless you explicitly want those features off. If no SR config is stored (`has_signal_routing=false`), firmware defaults apply (both features on).

### Compile-Time Constants

The default values for the configurable parameters above are defined in `SignalRoutingModule.h` and `NeighborGraph.h`:

```cpp
// SignalRoutingModule.h
#define SIGNAL_ROUTING_BROADCAST_SECS        360   // periodic topology broadcast interval
#define SIGNAL_ROUTING_DIRTY_BROADCAST_SECS  300   // minimum gap before early dirty broadcast (5 min)
#define SR_BROADCAST_MAX_HOPS                  5   // hop_limit cap for topology packets
#define MAX_SIGNAL_ROUTING_NEIGHBORS          28   // neighbors per broadcast payload (packed binary, fits 233-byte limit)

// SignalRoutingModule.h (private, class scope)
static constexpr uint32_t NODE_TTL_SECS = 5400;   // 90 min — graph aging TTL for all nodes
static constexpr uint32_t RELAY_ID_CACHE_TTL_MS = 600 * 1000;  // relay byte → NodeNum cache
static constexpr uint32_t ROUTE_CACHE_TIMEOUT_SECS = 300;      // Dijkstra result cache validity
static constexpr uint32_t CAPABILITY_TTL_SECS = SIGNAL_ROUTING_BROADCAST_SECS * 3 + 10;  // node capability cache

// NeighborGraph.h (private instance variable)
float etxChangeThreshold = 1.0f;   // minimum ETX delta for a significant edge change (base; per-edge etxVariance added)
uint8_t etxVariance;               // EWMA of |ETX change| × 20 on each edge — locally computed, broadcast to all
```

### Prompt Dirty-Topology Rebroadcast (`markTopologyDirty()`)

All topology change sites (new neighbor added, neighbor removed, placeholder resolved, topology received from a direct SR neighbor, etc.) call `markTopologyDirty()` instead of setting `topologyDirty = true` directly. `markTopologyDirty()` does two things atomically:

1. Sets `topologyDirty = true`
2. Calls `setIntervalFromNow(wakeIn)` to wake `runOnce()` as soon as `cfgDirtyBroadcastSecs` has elapsed since the last broadcast (immediately if it already has)

This ensures topology changes are broadcast promptly rather than waiting a full `cfgBroadcastSecs` cycle, while the `cfgDirtyBroadcastSecs` minimum gap prevents broadcast storms from rapid successive changes.

**Topology log does not clear the dirty flag** — the topology dump in `runOnce()` intentionally leaves `topologyDirty` set after logging. This allows the return value calculation at the bottom of `runOnce()` to correctly return the dirty-cycle wakeup interval rather than the full broadcast cycle until the early broadcast has actually fired.

### Node Capability Tracking

SR tracks each node's capability status to make informed routing decisions:

| Status | Description | TTL |
|--------|-------------|-----|
| `SRactive` | Node runs SR and actively participates | `CAPABILITY_TTL_SECS` (not configurable) |
| `Passive` | Node runs SR but in passive mode (TRACKER, SENSOR, etc.) | `CAPABILITY_TTL_SECS` (not configurable) |
| `Legacy` | Stock firmware node, no SR participation | `CAPABILITY_TTL_SECS` (not configurable) |
| `Unknown` | Not yet classified | — |

### Topology Health Assessment

SignalRouting only activates when network has sufficient capable nodes:

```cpp
// For broadcast: requires at least 1 direct SR-capable neighbor
bool topologyHealthy = capableNeighbors >= 1;

// For unicast: requires knowledge of destination node
bool topologyHealthy = nodeDB->getMeshNode(destination) != nullptr;
```

### Unicast Fallback for Unknown Routes

When a unicast packet targets a node not reachable through the SR topology graph, SR applies a three-tier fallback inside `shouldRelay()`, after the `!topologyHealthyForUnicast()` guard:

1. **Known in NodeDB** (e.g., a legacy node not participating in SR) → fall back to broadcast-style relay.
2. **Known as downstream** (reachable via the downstream routing table but not in the edge graph) → fall back to broadcast-style relay.
3. **Completely unknown** (not in graph, not in NodeDB, not in downstream table) → fall back to broadcast-style relay.

All three cases relay broadcast-style, giving every destination a chance to be reached while retaining SR coordination for packets with a known route.

## Real-World Examples

### Urban Mesh Network
```
Coffee Shop ── Street Node ── Park Node
     │             │             │
     └──── Apartment ──── Office ─────
                    │
               Subway Station (Gateway)
```

**SignalRouting Benefits:**
- Coordinates broadcasts to minimize redundant transmissions
- Routes unicasts through optimal paths based on link quality
- Uses gateway relationships for extended network reach
- Adapts routing when network topology changes

### Emergency Response Network
```
Command Post (Gateway)
        │
    ┌───┴───┐
Mobile Unit  Ambulance  Fire Truck
    │          │          │
    └──────┬───┴───┬──────┘
           │
       Incident Site
```

**SignalRouting Benefits:**
- Coordinated broadcast routing ensures full coverage
- ETX-based unicast routing prioritizes reliable communication paths
- Reduces redundant transmissions during critical operations
- Gateway awareness for command centers bridging network segments

### Rural Mesh with Branches
```
Main Road
    │
Farm House ── Barn ── Equipment Shed
    │                    │
    └───────── Grain Silo ──────┐
                                │
                     Network Gateway (Town)
```

**SignalRouting Benefits:**
- Efficient broadcast coverage across multiple structures
- Routes through intermediate nodes for better connectivity
- Gateway integration for external network access
- Topology-aware routing adapts to physical layout

## Algorithm Details

### Slot-Based Relay Selection

SignalRouting uses a deterministic slot-based algorithm for relay coordination:

1. **Pre-coverage**: heardFrom's neighbors with ETX < 7.0 marked as already covered; ETX ≥ 7.0 (near noise floor) excluded
2. **Candidate Building**: Only SR-active neighbors and stock ROUTER/REPEATER/ROUTER_CLIENT nodes (excluding heardFrom, source, and non-SR roles like CLIENT/CLIENT_MUTE) plus SR-active co-listeners that can hear the same transmitter
3. **Stock Router Slots**: Legacy routers/repeaters assigned first slots only if they can hear the transmitter (verified via topology graph edges)
4. **SR Candidate Ranking**: Iteratively pick best candidate (most unique coverage, lowest ETX, packet-ID-parity node ID tiebreak), assign next slot, remove from candidates
4. **Self-Assignment**: When we're picked as best candidate, schedule TX at that slot's delay
5. **Already-Transmitted Absorption**: Candidates that already transmitted have their coverage absorbed without consuming a slot
6. **Forced Relay**: If not assigned a slot, downstream override or stock coverage checks may still force a relay
7. **Dupe Suppression**: If a relay arrives before our slot fires, existing deduplication cancels our queued TX

Slot spacing is half the packet airtime, ensuring the next-slot node detects ongoing reception (`busyRx`) and holds its transmission.

### Unicast Relay Logic

`shouldRelayUnicastForCoordination` mirrors the broadcast slot scheduler:

1. **Quick suppression**: src+dst same downstream relay, heardFrom can reach dst, SR neighbor covering heardFrom can reach dst
2. **No route → suppress**: `getNextHop(destination)` returns 0
3. **Phase 1 — designated next_hop**: if `p->next_hop` is set, slot 0 belongs to it; if we ARE that node, relay at slot 0 immediately; otherwise advance slotDelay to slot 1
4. **Phase 2 — SR candidates**: self + SR-active neighbors sorted ascending by cost-to-destination; first unassigned candidate gets the next slot
5. **Slot assignment**: if it's our slot, `pendingRelayDelayMs` is set and we return true; otherwise suppress
6. **Dupe cancellation**: if any dupe arrives before our TX fires, `isDupeRelayRedundant` unconditionally cancels our queued relay

```
Slot timing example (150ms half-airtime, next_hop set, 2 SR nodes with direct edge to FCM6):

  Slot 0 (0ms):    Designated next_hop node (e.g. MB9c, set in p->next_hop)
  Slot 1 (150ms):  SR node with lower ETX to destination (e.g. MBe4, ETX=1.2)
  Slot 2 (300ms):  SR node with higher ETX to destination (e.g. MBf1, ETX=1.8)

MB9c transmits at slot 0. MBe4 hears it → isDupeRelayRedundant: any dupe → redundant → cancelSending. MBf1 also cancels.

Slot timing example (150ms half-airtime, next_hop NOT set):

  Slot 0 (0ms):    SR node with lower ETX to destination (e.g. MB9c, ETX=1.2)
  Slot 1 (150ms):  SR node with higher ETX to destination (e.g. MBe4, ETX=1.8)

MB9c transmits at slot 0. MBe4 hears it → cancels.
```

### Performance Characteristics

**Broadcast Coordination:**
- Deterministic slot-based scheduling with half-airtime spacing eliminates most SR-to-SR collisions
- Stock routers get earliest slots for compatibility; SR nodes fill subsequent slots
- Dupe suppression via existing packet deduplication cancels redundant relays
- Hash-based delay for unique coverage prevents collision between independently-deciding nodes
- Effectiveness depends on topology knowledge accuracy (consistent candidate ordering requires consistent topology views)

**Unicast Coordination:**
- Uses the same slot-based scheduling as broadcasts, with ETX-to-destination as the ranking metric
- Designated next_hop (from `p->next_hop`) gets slot 0; SR candidates sorted by cost start from slot 1 (or slot 0 if no next_hop)
- Any dupe unconditionally cancels queued unicast relays — the slot ordering guarantees earlier transmitters are better positioned
- Falls back to broadcast-style relay for all destinations not reachable via SR topology

**Network Adaptation:**
- Assesses topology health but may not detect sudden changes immediately
- Degrades to contention-based approaches when coordination isn't possible
- Works with mixed legacy/SignalRouting networks but coordination is limited by legacy node behavior

### Fallback Mechanisms

SignalRouting gracefully degrades when coordination isn't possible:

1. **Unknown Destinations**: Unicasts to nodes not reachable via SR topology fall back to broadcast-style relay, regardless of whether the destination is known in NodeDB, the downstream table, or neither
2. **Topology Incomplete**: Uses traditional unicast routing for known but poorly connected destinations
3. **Legacy Node Priority**: Gives priority to legacy routers/repeaters for compatibility
4. **Memory/CPU Constraints**: Automatic feature disabling for constrained devices

### Compatibility

- **Backward Compatible**: Works with all existing Meshtastic nodes
- **Progressive Enhancement**: Benefits increase with more SR-capable nodes
- **Mixed Networks**: Automatically detects and adapts to legacy nodes including CLIENT_MUTE topology sharing
- **Gateway Integration**: Special handling for nodes bridging network segments
- **Mute Node Intelligence**: CLIENT_MUTE nodes contribute topology information without relay participation

## Troubleshooting

### Common Issues

**"SR disabled - insufficient RAM"**
- Device has <30KB free RAM (RP2040 guard)
- SR is disabled entirely on STM32WL (64KB RAM limit)

**"Topology unhealthy - flooding only"**
- Too few SR-capable nodes or incomplete topology information
- Add more SignalRouting-capable devices or wait for topology convergence

**"No route found for unicast"**
- Destination not in topology graph
- SR falls back to broadcast-style relay for all unroutable destinations
- Wait for topology convergence or use opportunistic forwarding

**"Packet not relayed despite good coverage"**
- Iterative algorithm may have excluded the node or found better candidates
- Legacy nodes may have priority and relayed instead
- Relay byte matching may have detected the designated relay already transmitted
- Unique coverage requirements may not be met

**"Unexpected relays from legacy nodes"**
- Legacy routers/repeaters are prioritized and may relay independently
- This is expected behavior for compatibility with existing infrastructure

**"Unresolved placeholder nodes (ff0000XX)"**
- Placeholder created for unknown relay byte, not yet resolved via direct contact
- Placeholders age out with `cfgNodeTtlSecs` if never resolved
- Only created for directly-heard relay nodes, not nodes reported by others

**"Nodes disappearing from topology"**
- Check if nodes are being evicted from NodeDB (100 node limit)
- Verify `last_heard` is being updated (uses `rx_time` or `getTime()` when no NTP/GPS)
- Check edge aging timeout: all nodes expire after `cfgNodeTtlSecs` (configurable via `node_ttl_secs`)

SignalRouting provides an alternative to traditional flooding-based mesh networking, offering basic coordination for packet relay decisions. It works alongside existing routing approaches and provides benefits in networks with sufficient SignalRouting-capable nodes, while maintaining compatibility with legacy devices through prioritized relay handling.
