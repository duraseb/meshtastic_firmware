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
| **REPEATER** | Rebroadcasts, can cancel duplicates | **Priority relays** - SR gives them highest priority as they always rebroadcast |
| **CLIENT_BASE** | Special: acts as ROUTER for favorited nodes | Uses SR coordination for broadcasts |
| **TRACKER/SENSOR/TAK** | Rebroadcasts, can cancel duplicates | Treated as legacy, no SR participation |

**Legacy Node Priority:** SignalRouting prioritizes ROUTER and REPEATER roles because these nodes are configured to always rebroadcast packets. CLIENT_MUTE nodes participate minimally by broadcasting their direct neighbor topology to assist active SignalRouting nodes in making informed unicast routing decisions.

### Retransmission Behavior

| Router Type | Unicast Retransmissions | Notes |
|-------------|------------------------|-------|
| **ReliableRouter** | Up to 3 retransmissions | For want_ack packets only |
| **NextHopRouter** | 2 for intermediate hops, 3 for origin | Route reset on final failure |
| **SignalRouting** | Iterative unicast route selection with fallback strategies | For SR-selected unicast routes |

### Passive Node Behavior

Passive SR nodes (TRACKER, SENSOR, TAK, or non-active-routing configured nodes) participate minimally in SR:

1. **Topology Maintenance**: Only track directly-heard neighbors (hopStart == hopLimit)
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
| **SignalRouting** | Slot-based: half-airtime per slot + hash-based unique coverage delay | ETX-based route selection + speculative retransmit |

## Direct Neighbor Detection

### Reliable Detection Method: hopStart/hopLimit

SignalRouting uses the hop counter to definitively determine if a packet was received directly from the sender:

```cpp
bool isDirectFromSender = (mp.hop_start == mp.hop_limit);
```

**How it works:**
- When a sender transmits a packet, `hop_start` is set to the initial hop limit
- Each relay decrements `hop_limit`
- If `hop_start == hop_limit`, the packet has NOT been decremented, meaning it reached us directly from the sender without any relays

**Advantages over relay_node matching:**
- Immune to node ID collisions (multiple nodes sharing the same last byte)
- Works correctly even when same packet received via different relay paths
- More reliable than checking if relay_node matches sender's last byte
- Handles SR broadcasts correctly since they maintain minimum hopLimit=1

**Impact on Topology:**
- Passive nodes only add neighbors from direct packets to their graph
- Direct packets are identified by `hopStart == hopLimit` condition
- Passive nodes skip all relayed packets (hopStart > hopLimit)
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

2. **Topology Broadcasts**: Periodically broadcast their complete neighbor list for comprehensive topology learning from other nodes' perspectives

3. **Relayed Packet Inference**: When receiving a relayed packet, gateway relationships are inferred between the original sender and the relay node. If the relay node is a stock (Legacy) firmware node, a directed edge is also recorded from the relay to the sender — observing a successful relay proves the relay can hear the sender, regardless of the sender's firmware type. The reverse edge is not assumed.

4. **Placeholder System**: Unknown relay nodes are tracked as placeholders until their real identities are discovered through direct contact

5. **Gateway Relationship Tracking**: Downstream relationships learned when packets flow through relay nodes, enabling multi-hop route discovery

**Passive Routing Nodes (TRACKER, SENSOR, TAK, and nodes not configured for active routing):**
Passive nodes maintain a simplified Level 1 topology containing ONLY directly-heard neighbors:

1. **Direct Neighbor Detection**: Only neighbors heard directly (hopStart == hopLimit with signal data) are tracked with measured ETX values

2. **SR Broadcast Reception**: Only process topology broadcasts from direct senders (hopStart == hopLimit), preventing ingestion of remote network topology

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
When any dupe arrives for a committed unicast relay, `isDupeRelayRedundant` unconditionally cancels our queued TX. The slot-based ordering guarantees that earlier transmitters are better positioned, so once they relay there is no need for later slots to retransmit.

## Broadcast Routing

### Slot-Based Relay Coordination

SignalRouting uses a deterministic slot-based algorithm to coordinate broadcast relays. Each node independently computes the same candidate ordering and assigns time slots spaced by half the packet airtime. Nodes schedule their TX at their assigned slot and rely on existing dupe suppression: if a node hears the packet relayed before its slot fires, it cancels its queued transmission.

**Algorithm phases:**

1. **Stock routers first**: Legacy ROUTER/REPEATER neighbors get the earliest slots since they transmit regardless of SR decisions. If they already transmitted, their coverage is absorbed.

2. **SR candidate ranking**: Remaining candidates are iteratively ranked by `findBestRelayCandidate` (most unique coverage, lowest ETX, deterministic node ID tiebreak based on packet ID parity). Each candidate gets the next slot. If it's us, we schedule TX and stop. If a candidate already transmitted, we absorb its coverage without consuming a slot.

3. **Overrides**: Downstream relay obligations and stock coverage needs can force a relay.

**Post-cancellation — unique coverage re-evaluation:**
When a dupe arrives and `isDupeRelayRedundant` is called, it checks whether we still have neighbors not reachable by any transmitter heard so far (original `heardFrom` + all subsequent dupe relayers). Transmitters are accumulated across all dupes for the same packet. If we have unique coverage, `isDupeRelayRedundant` returns false (keep our scheduled relay). If all our neighbors are already covered, it returns true (cancel). Because Phase 2 assigns each SR candidate a unique sequential slot, two nodes with unique coverage will never collide when both keep their relays.

```
Slot timing example (150ms half-airtime):

  Slot 0 (0ms):    Stock router R1
  Slot 1 (150ms):  Stock router R2
  Slot 2 (300ms):  Best SR candidate (most unique coverage)
  Slot 3 (450ms):  Next SR candidate
  ...
  Unique (600ms+): Hash-based delay for nodes with uncovered neighbors
```

**Deterministic tiebreak**: When two candidates have identical coverage and cost, the winner is determined by node ID direction based on packet ID parity (even → lowest ID, odd → highest ID). This distributes relay duty evenly across nodes.

### How Slot Spacing Works

Slots are spaced by half the packet airtime. When slot 0 starts transmitting, slot 1's radio detects the ongoing reception (`busyRx`) and holds. After slot 0's packet is received, dupe detection cancels slot 1's queued relay if the coverage is redundant. Half-airtime spacing provides enough margin for preamble detection while keeping propagation fast.

### Topology Edge Persistence

Mirrored edges (learned from topology broadcasts) are not cleared when a new topology version arrives. They age out naturally. This prevents valid neighbor knowledge from being lost between broadcasts — for example, a stock node that both SR neighbors can hear would otherwise be forgotten each time they exchange topology, causing false "unique coverage" decisions.

### Relay Decision Factors

1. **Stock Router Priority**: Legacy routers/repeaters get earliest slots as they always rebroadcast
2. **Deterministic Ordering**: All nodes compute the same candidate ranking for consistent slot assignment
3. **Coverage-Based Selection**: Candidates ranked by unique coverage count, then ETX quality
4. **Dupe Suppression**: Existing packet deduplication cancels queued relays when earlier slots transmit
5. **Unique Coverage Fallback**: Nodes covering areas no candidate reaches relay with hash-based delay
6. **Downstream Override**: Nodes recorded as relay for source/destination are forced to relay

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
- Unique coverage relays use hash-based delays (0-2000ms) to spread out remaining transmissions

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
| `NodeEdges` | A neighbor slot: nodeId + up to 16 edges + last full update time |
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
4. **TTL**: Placeholders age out with normal node TTL (90 min) if never resolved

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
- **Route caching**: Cache validity and expiration (5 min)
- **Transmission tracking**: Contention window timing
- **Edge timestamps**: Connection update times

This prevents issues with RTC updates that could cause time jumps, ensuring reliable graph maintenance and routing decisions.

**Hardware Adaptation:**
- **STM32WL**: SR disabled entirely (64KB RAM limit)
- **RP2040**: RAM guard (< 30KB free disables SR)
- **ESP32-C3/ESP32/all others**: NeighborGraph (~24 KB)

## Configuration and Tuning

### Key Parameters

```cpp
// Broadcast interval for topology updates
#define SIGNAL_ROUTING_BROADCAST_SECS 300     // 5 minutes

// Maximum neighbors tracked per broadcast payload
#define MAX_SIGNAL_ROUTING_NEIGHBORS 11       // Fits 11 in 233 byte payload
```

### Timeout Values

| Parameter | Value | Purpose |
|-----------|-------|---------|
| SR broadcast interval | 300s (5 min) | Topology update frequency (event-driven on neighbor add/remove) |
| Node TTL | 5400s (90 min) | How long all nodes stay in graph |
| Edge aging timeout | 5400s (90 min) | Unified edge expiration for all node types |
| Placeholder TTL | 5400s (90 min) | How long unresolved placeholders survive (same as node TTL) |
| Relay ID cache TTL | 600s (10 min) | Relay byte → NodeNum mapping cache |
| Capability cache TTL | 910s (~15 min) | Node capability status cache (3× broadcast interval + 10s) |
| Route cache timeout | 300s (5 min) | Dijkstra result cache validity |
| ETX change threshold | 0.50 | Minimum ETX change to trigger update |

### Node Capability Tracking

SR tracks each node's capability status to make informed routing decisions:

| Status | Description | TTL |
|--------|-------------|-----|
| `SRactive` | Node runs SR and actively participates | ~15 min |
| `Passive` | Node runs SR but in passive mode (TRACKER, SENSOR, etc.) | ~15 min |
| `Legacy` | Stock firmware node, no SR participation | ~15 min |
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

When a unicast packet targets a node not reachable through the SR topology graph, SR checks whether the node exists in NodeDB. If it does (e.g., a legacy node not participating in SR), the packet falls back to broadcast-style relay to give it a chance to reach its destination. Truly unknown nodes (not in NodeDB at all) are dropped to prevent flooding.

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

1. **Candidate Building**: Direct neighbors (excluding heardFrom and source) plus SR-active co-listeners that can hear the same transmitter
2. **Stock Router Slots**: Legacy routers/repeaters assigned first slots (they transmit regardless)
3. **SR Candidate Ranking**: Iteratively pick best candidate (most unique coverage, lowest ETX, packet-ID-parity node ID tiebreak), assign next slot, remove from candidates
4. **Self-Assignment**: When we're picked as best candidate, schedule TX at that slot's delay
5. **Already-Transmitted Absorption**: Candidates that already transmitted have their coverage absorbed without consuming a slot
6. **Unique Coverage**: If not assigned a slot, check for uncovered neighbors and relay with hash-based delay
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
- Falls back to broadcast-style relay when destination is in NodeDB but not in SR topology

**Network Adaptation:**
- Assesses topology health but may not detect sudden changes immediately
- Degrades to contention-based approaches when coordination isn't possible
- Works with mixed legacy/SignalRouting networks but coordination is limited by legacy node behavior

### Fallback Mechanisms

SignalRouting gracefully degrades when coordination isn't possible:

1. **Unknown Destinations**: Unicasts to nodes in NodeDB but not in SR topology fall back to broadcast-style relay; truly unknown nodes are dropped
2. **Topology Incomplete**: Uses traditional unicast routing for known but poorly connected destinations
3. **Legacy Node Priority**: Gives priority to legacy routers/repeaters for compatibility
4. **Memory/CPU Constraints**: Automatic feature disabling for constrained devices

### Compatibility

- **Backward Compatible**: Works with all existing Meshtastic nodes
- **Progressive Enhancement**: Benefits increase with more SR-capable nodes
- **Mixed Networks**: Automatically detects and adapts to legacy nodes including CLIENT_MUTE topology sharing
- **Gateway Integration**: Special handling for nodes bridging network segments
- **Mute Node Intelligence**: CLIENT_MUTE nodes contribute topology information without relay participation

## Implementation Notes

### NodeDB Integration

SR relies on NodeDB for node existence checks (e.g., unicast fallback decisions). When NodeDB is full (100 nodes), the least-recently-heard node is evicted. The `last_heard` field is set from `rx_time` when available, or from `getTime()` when `rx_time` is 0 (no NTP/GPS), ensuring active nodes are not evicted as stale.

### Stack Safety

Large buffers are stored in the heap-allocated NeighborGraph class, not on the stack. Stack-allocated display buffers are capped to small sizes:
- `transferDownstream()` operates directly on the private downstream array
- `logNetworkTopology()` caps display buffers to 64 entries with "... and N more" truncation

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
- If node exists in NodeDB, SR will fall back to broadcast-style relay
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
- Placeholders age out with normal node TTL (90 min) if never resolved
- Only created for directly-heard relay nodes, not nodes reported by others

**"Nodes disappearing from topology"**
- Check if nodes are being evicted from NodeDB (100 node limit)
- Verify `last_heard` is being updated (uses `rx_time` or `getTime()` when no NTP/GPS)
- Check edge aging timeout: all nodes expire after 90 min (NODE_TTL_SECS=5400)

SignalRouting provides an alternative to traditional flooding-based mesh networking, offering basic coordination for packet relay decisions. It works alongside existing routing approaches and provides benefits in networks with sufficient SignalRouting-capable nodes, while maintaining compatibility with legacy devices through prioritized relay handling.
