# SR Routing Debug Guide

## Log locations
- `../logs/<node>_YYYYMMDD_HHMM.txt` — per-node logs
- Always use the most recent file per node.
- Logs are binary-safe only via `strings`: `strings <log> | grep ...`

## Key log patterns

### Relay decisions
```
[SR-DECISION] BROADCAST RELAY: from <src> via <heardFrom> (<reason>, delay=<N>ms)
[SR-DECISION] BROADCAST SUPPRESS: from <src> via <heardFrom> (<reason>, delay=0ms)
```

### Slot scheduling (per packet)
```
[SR] Slot scheduling for pkt 0x<id>: halfAirtime=<N>ms, <K> candidates
[SR] Slot 0ms: stock router <nodeId> (already transmitted)
[SR] Slot 0ms: SR node <nodeId> (coverage=<N>, cost=<F>)
[SR] Slot 0ms: SR node <nodeId> (coverage=<N>, cost=<F>, bidi)
[SR] Slot <N>ms: US (<myNode>) — assigned
[SR] Slot <N>ms: US (<myNode>) — assigned (bidi)
```
Only the node assigned "US" will relay. All slots are computed independently by each node. The `bidi` tag indicates the candidate has a confirmed bidirectional link (`hearsUs=true`, ETX < 20.0) to the packet source, giving it tier 1 priority over non-bidi candidates regardless of coverage.

### Channel QoS relay gating
```
[QoS] Drop relay 0x<id> from 0x<from>: tier <LOW|MEDIUM|HIGH>, chutil <N>% >= <threshold>%
```
Fires when channel utilization exceeds the tier threshold. Tiers: LOW (telemetry/position, >25%), MEDIUM (non-primary text, >30%), HIGH (routing/traceroute, >38%). CRITICAL (primary text, admin) is never dropped. If no QoS messages appear, channel utilization is below all thresholds.

### Committed relay & TX
```
[SR] Committed relay for packet 0x<id> (heardFrom 0x<id>, delay <N>ms)
Started Tx (id=0x<id> ... relay=0x<byte> ...)
Completed sending (id=0x<id> ...)
```

### T1 retransmit insurance
```
[SR] T1 scheduled for 0x<id> (<portnum>) — fires in <N>ms (ROUTER_LATE window <W>ms + airtime <A>ms)
[SR] T1 firing for 0x<id> — no relay heard in window; retransmitting now
[SR] T1 canceled for 0x<id> — relay confirmed heard
[SR] T1 canceled for 0x<id> — all hearsUs neighbors already heard packet
[SR] No confirmed relay neighbors — skipping T1 for 0x<id>   ← DEBUG, hearsUs=false on all edges
[SR] Packet pool exhausted — skipping T1 for 0x<id>           ← WARN
[SR] All retransmit slots occupied — dropping T1 for 0x<id>   ← WARN, MAX_PENDING_RETRANSMITS full
```
T1 fires only if no dupe relay is heard before the timer expires. If a relay is heard (any path through `perhapsCancelDupe`), `T1 canceled — relay confirmed heard` appears instead. At fire time, `allHearsUsNeighborsHeardPacket()` checks whether every `hearsUs` neighbor already received the packet — via direct observation (they transmitted it) or graph inference (a known transmitter has a link to them). If so, `T1 canceled — all hearsUs neighbors already heard packet` appears. If `T1 firing` appears without either cancel, the original transmission was likely lost to interference.

### Dupe suppression (post-TX cancellation)
```
[SR] Dupe relayer <name> covers all our neighbors — relay is redundant
[SR] Canceling committed relay for 0x<id> - dupe relayer covers our nodes
cancelSending id=0x<id>, removed=<0|1>      ← 1=cancelled before TX, 0=already sent
[SR] Not canceling committed relay for 0x<id> - we have unique coverage
[SR] Unique coverage: neighbor <nodeId> not covered by any coveredBy node
```

### Topology propagation
```
[SR] Processing topology from <name>: <N> neighbors (version <V>, new version, relay=0x<byte>)
[SR] Empty broadcast from direct SR neighbor <name> — marking topology dirty
[SR] Topology dirty — sending early broadcast
[SR] Network Topology: <N> nodes, <K> direct neighbors
```

### Broadcast timing
```
[SR] Topology dirty — sending early broadcast     ← dirty-triggered; only fires after cfgDirtyBroadcastSecs min interval
[SR] Sending empty boot broadcast to bootstrap topology
[SR] Direct neighbor lost during aging — marking topology dirty
```
The dirty broadcast respects a minimum inter-broadcast interval of `cfgDirtyBroadcastSecs`. When `markTopologyDirty()` is called it schedules `runOnce()` to wake at `max(0, cfgDirtyBroadcastSecs − elapsed_since_last_broadcast)`. If no topology changes occur the periodic broadcast fires every `cfgBroadcastSecs`.

`markTopologyDirty()` is intentionally scoped to direct-edge events only:
- A new direct neighbor is heard, or significant ETX change on an existing one (`updateNeighborInfo`)
- A direct neighbor is aged out (`ageEdges` + `countDirectNeighbors` check)
- An empty SR broadcast (SR-zero) is received from a direct SR neighbor (bootstrap)

It does **not** fire on remote node count changes during graph aging — those are transient nodes irrelevant to our own topology broadcast.

### Downstream node assignment
```
[SR]   -> <name>: NO direct connection, marking as downstream of topology source <relay>
[SR]   -> <name>: NO direct connection, but asymmetric link (hearsUs=false) — skipping downstream of <relay>
[SR]   -> <name>: HAS direct connection, sender confirms reachability
```
When topology arrives from node X, any of X's listed neighbors that *we* cannot hear directly are recorded in the downstream table as `(destination, relay=X)` — but only if X reports `hearsUs=true` for that neighbor (the neighbor can hear X). If `hearsUs=false`, the link is asymmetric and X cannot deliver to that neighbor, so the downstream entry is skipped.

### Authoritative hearsUs override
```
[SR] Clearing hearsUs on <name> -> <source>: source topology doesn't list <name> as neighbor
```
After processing a topology broadcast from node X, SR checks all graph nodes that claim `hearsUs=true` on their edge to X. If X didn't list that node as a neighbor, the flag is cleared — X is authoritative about who it can hear. This corrects stale bidi claims from nodes that X can no longer receive.

**Stale downstream entries at boot**: a topology from a gateway node may arrive before the gateway's first direct packet. The gateway would be incorrectly marked as downstream of the topology sender. This self-corrects: when the first direct packet from the gateway is received, `clearDownstreamForDestination` removes all downstream entries where that node is the destination.

In the topology dump, each direct neighbor has two sub-sections:
- **listener lines** (no prefix, just `ETX=…`) — nodes in that neighbor's own edge list (what it can directly hear)
- **`[downstream]` lines** — nodes in our downstream table whose relay is that neighbor; these are nodes we can only reach *through* that neighbor

`relay for N downstream` reflects `getDownstreamCountForRelay()`. If this shows 0 for a node that is clearly a gateway, the downstream entries were not written — check that the topology broadcast from that node was received and processed (see "Topology propagation" above).

### Last-hop unicast hop limiting
```
[SR] Limiting hop_limit=0 for unicast relay 0x<id>: dest is direct hearsUs neighbor, stock neighbors present
[SR] Limiting hop_limit=1 for unicast relay 0x<id>: dest is direct hearsUs neighbor, stock neighbors present
[SR] Limiting hop_limit=0 for originated unicast 0x<id>: dest is direct hearsUs neighbor, stock neighbors present
[SR] Limiting hop_limit=1 for originated unicast 0x<id>: dest is direct hearsUs neighbor, stock neighbors present
```
These fire when the destination is a confirmed direct neighbor (`hearsUs=true`) and at least one other direct neighbor is a stock node. Good links (ETX < 3.0) get `hop_limit=0` (direct delivery, no further relay). Marginal links (ETX ≥ 3.0) get `hop_limit=1`, allowing one retry relay if our transmission is lost. If `hop_limit=1` appears frequently for a link that should be reliable, check the ETX on that edge.

## Debugging redundant relays

### Step 1 — extract all relay events for a packet
```sh
strings <log> | grep "<packetId>"
```
Look for multiple `Started Tx` lines with the same packet ID across logs — that's the redundancy.

### Step 2 — cross-reference timing
For a broadcast from node X, check each other node's log for:
1. `Slot scheduling` — what slots were assigned
2. `Not canceling … unique coverage` — why dupe suppression failed
3. `Unique coverage: neighbor <id> not covered` — which neighbor was missing from the relayer's edge set

### Step 3 — check topology at decision time
Find the last topology processing line **before** the packet's `Slot scheduling` line:
```sh
strings <log> | grep -n "Processing topology\|Slot scheduling for pkt 0x<id>"
```
If the relayer's topology hasn't arrived yet (its line number is **after** the scheduling line), dupe suppression will fail because the relayer has 0 edges in the graph.

### Step 4 — topology dump
```sh
strings <log> | grep -n "Network Topology:"
strings <log> | sed -n '<line>,<line+200>p'
```
Check that all expected branch nodes are in each other's direct neighbor lists. Missing edges indicate incomplete topology propagation.

## Common failure modes to investigate

- **Redundant relays**: multiple nodes transmit the same packet. Check if dupe suppression fired (`Not canceling … unique coverage`) and whether the named uncovered neighbor's topology was available at decision time.
- **Suppressed relays**: a packet doesn't propagate far enough. Check if slot scheduling assigned no slot (`no unique coverage`) and whether edge data is stale or missing.
- **Unicast to unknown destination suppressed**: if the destination is not in the SR graph, not in NodeDB, and not in the downstream table, SR suppresses the relay entirely (`UNICAST SUPPRESS ... unknown destination — not in SR graph or NodeDB`). This is expected — relaying for completely unknown destinations wastes airtime. If the destination should be known, check topology propagation and NodeDB state.
- **Incorrect slot ordering**: a node picks an early slot when a better-covered node should go first. Compare coverage counts and costs in the slot schedule across both logs.
- **Topology staleness**: relay decisions based on outdated edges. Compare topology processing timestamps against packet scheduling timestamps.
- **Stock nodes relaying last-hop unicasts**: if you see a stock node retransmitting a unicast that was already destined for a direct neighbor, check whether `getUnicastHopLimitForDirectNeighbor` fired (look for the `Limiting hop_limit` log lines). If it did not fire, verify that `hearsUs=true` is set on the destination's edge and that the sender had at least one stock direct neighbor.
- **T1 firing frequently**: indicates the original T0 transmission is regularly lost to interference or overlapping transmissions. If `T1 firing` appears often without a prior `T1 canceled` (either variant), investigate RF environment. If `T1 canceled — relay confirmed heard` consistently appears, a neighbor retransmitted our relay. If `T1 canceled — all hearsUs neighbors already heard packet` appears, all neighbors already had the packet from another path — graph-based inference prevented a redundant retransmit.
- **T1 never scheduled**: check that `hearsUs=true` is set on at least one direct neighbor edge (requires that neighbor to have relayed one of our packets), and that `t1_retransmit_enabled=true` in config.
- **Delayed topology dirty broadcast**: the early broadcast fires after at most `cfgDirtyBroadcastSecs` from the last broadcast. If it never fires, check whether `markTopologyDirty()` was actually called — it only triggers on direct-edge events (new/changed/lost direct neighbor, or SR-zero bootstrap). Remote topology changes do not trigger it by design.
- **SR broadcasts firing at `cfgDirtyBroadcastSecs` rate instead of `cfgBroadcastSecs`**: means topology is continuously dirty. The per-edge dirty threshold is `etxChangeThreshold + edge.etxVariance` — noisy links auto-dampen, but if many edges fluctuate simultaneously it can still trigger. Check `updateNeighborInfo` for repeated significant ETX changes on a direct neighbor caused by radio link instability, or a direct neighbor cycling in/out of range.
- **Placeholder created repeatedly for the same relay byte**: normal — placeholder IDs are deterministic (`0xFF000000 | relayByte`). The "Created placeholder" log fires only once per unique relay byte (on first encounter). Subsequent packets from the same unresolved relay silently reuse the same ID.
- **Edge timestamps not advancing for known neighbors**: by design — `updateGraphWithNeighbor` (topology processing) sets `updateTimestamp=false` on existing edges to prevent phantom reference cascades. Edge timestamps only advance from direct observations (`updateNeighborInfo`, relay events). A topology-learned edge ages out after `cfgNodeTtlSecs` from first observation unless reinforced by a direct packet.

## Relay algorithm summary

1. **Slot scheduling** (`shouldRelayBroadcast`): each node independently ranks all SR candidates by unique coverage (greedy). Stock routers take slot 0. SR nodes fill subsequent slots. "US" gets assigned a delay and breaks the loop.
2. **TX**: packet queued with `tx_after = now + slotDelay`.
3. **Dupe arrives**: `isDupeRelayRedundant` checks if the dupe relayer's known edges cover all our neighbors. If yes → cancel TX. If no (or edges unknown) → keep TX.
4. `cancelSending removed=1` = successfully cancelled; `removed=0` = already on air.

The `alreadyCovered` set in slot scheduling is intentionally NOT updated between iterations — each candidate's unique coverage is evaluated independently. Actual relay suppression happens via over-the-air dupe detection, not slot ordering.
