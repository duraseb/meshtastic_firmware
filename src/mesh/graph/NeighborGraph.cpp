#include "NeighborGraph.h"
#include "configuration.h"

#if !MESHTASTIC_EXCLUDE_SIGNALROUTING

#include <algorithm>
#include <cmath>

NeighborGraph::NeighborGraph() : neighborCount(0), downstreamCount(0), relayStateCount(0), routeCacheCount(0) {}

// --- Private helpers ---

NodeEdges *NeighborGraph::findNeighbor(NodeNum nodeId)
{
    for (uint8_t i = 0; i < neighborCount; i++) {
        if (neighbors[i].nodeId == nodeId) {
            return &neighbors[i];
        }
    }
    return nullptr;
}

const NodeEdges *NeighborGraph::findNeighbor(NodeNum nodeId) const
{
    for (uint8_t i = 0; i < neighborCount; i++) {
        if (neighbors[i].nodeId == nodeId) {
            return &neighbors[i];
        }
    }
    return nullptr;
}

NodeEdges *NeighborGraph::findOrCreateNeighbor(NodeNum nodeId)
{
    NodeEdges *node = findNeighbor(nodeId);
    if (node) {
        return node;
    }

    if (neighborCount < NEIGHBOR_GRAPH_MAX_NEIGHBORS) {
        node = &neighbors[neighborCount++];
        node->nodeId = nodeId;
        node->edgeCount = 0;
        node->lastFullUpdate = 0;
        return node;
    }

    // Full - evict oldest non-self neighbor
    uint32_t currentTime = millis() / 1000;
    NodeNum myNode = nodeDB ? nodeDB->getNodeNum() : 0;
    uint32_t oldestTime = UINT32_MAX;
    uint8_t evictIdx = 0;
    uint8_t minEdges = 255;

    for (uint8_t i = 0; i < neighborCount; i++) {
        if (neighbors[i].nodeId == myNode)
            continue;
        if (currentTime - neighbors[i].lastFullUpdate < 120)
            continue;

        if (neighbors[i].lastFullUpdate < oldestTime ||
            (neighbors[i].lastFullUpdate == oldestTime && neighbors[i].edgeCount < minEdges)) {
            oldestTime = neighbors[i].lastFullUpdate;
            minEdges = neighbors[i].edgeCount;
            evictIdx = i;
        }
    }

    if (oldestTime == UINT32_MAX) {
        return nullptr;
    }

    // Also remove downstream entries that reference the evicted node as relay
    NodeNum evictedNode = neighbors[evictIdx].nodeId;
    for (uint16_t i = 0; i < downstreamCount;) {
        if (downstream[i].relay == evictedNode) {
            if (i < downstreamCount - 1) {
                downstream[i] = downstream[downstreamCount - 1];
            }
            downstreamCount--;
        } else {
            i++;
        }
    }

    node = &neighbors[evictIdx];
    node->nodeId = nodeId;
    node->edgeCount = 0;
    node->lastFullUpdate = 0;
    return node;
}

Edge *NeighborGraph::findEdge(NodeEdges *node, NodeNum to)
{
    if (!node)
        return nullptr;
    for (uint8_t i = 0; i < node->edgeCount; i++) {
        if (node->edges[i].to == to) {
            return &node->edges[i];
        }
    }
    return nullptr;
}

const Edge *NeighborGraph::findEdge(const NodeEdges *node, NodeNum to) const
{
    if (!node)
        return nullptr;
    for (uint8_t i = 0; i < node->edgeCount; i++) {
        if (node->edges[i].to == to) {
            return &node->edges[i];
        }
    }
    return nullptr;
}

bool NeighborGraph::isOurDirectNeighbor(NodeNum nodeId) const
{
    NodeNum myNode = nodeDB ? nodeDB->getNodeNum() : 0;
    const NodeEdges *myEdges = findNeighbor(myNode);
    if (!myEdges)
        return false;
    for (uint8_t i = 0; i < myEdges->edgeCount; i++) {
        if (myEdges->edges[i].to == nodeId) {
            return true;
        }
    }
    return false;
}

// --- Core methods ---

int NeighborGraph::updateEdge(NodeNum from, NodeNum to, float etx, uint32_t timestamp,
                              Edge::Source source, bool updateTimestamp)
{
    NodeNum myNode = nodeDB ? nodeDB->getNodeNum() : 0;

    // Only store edges for our node and our direct neighbors
    // If 'from' is our node, always accept (slot 0 effectively)
    // If 'from' is already in neighbors[], update that neighbor's edge list
    // If 'from' is NOT in neighbors[]: only create a slot if 'from' is our direct neighbor
    //   or if we're adding an edge FROM our node
    bool isOurNode = (from == myNode);
    NodeEdges *existing = findNeighbor(from);

    if (!existing && !isOurNode) {
        // Check if 'from' is one of our direct neighbors (has an edge from us)
        if (!isOurDirectNeighbor(from)) {
            // Also allow if 'from' appears as a destination in an existing neighbor's edge list
            // (i.e., reachable one hop through a known direct neighbor — an SR gateway node).
            // This lets us store topology data from SR nodes behind our direct neighbors so
            // Dijkstra can route through them rather than falling back to broadcast-style relay.
            bool reachableViaNeighbor = false;
            for (uint8_t i = 0; i < neighborCount && !reachableViaNeighbor; i++) {
                for (uint8_t e = 0; e < neighbors[i].edgeCount; e++) {
                    if (neighbors[i].edges[e].to == from) {
                        reachableViaNeighbor = true;
                        break;
                    }
                }
            }
            if (!reachableViaNeighbor) {
                return EDGE_NO_CHANGE; // Remote node, not reachable via our graph - ignore
            }
        }
    }

    NodeEdges *node = findOrCreateNeighbor(from);
    if (!node) {
        return EDGE_NO_CHANGE;
    }

    if (updateTimestamp) {
        node->lastFullUpdate = timestamp;
    }

    // If from == myNode and we're adding an edge to 'to', ensure 'to' has a neighbor slot
    if (isOurNode) {
        findOrCreateNeighbor(to);
    }

    Edge *edge = findEdge(node, to);
    if (edge) {
        // A weaker class never overwrites a stronger one, whichever arrived last: that is what
        // makes two nodes holding the same reports agree on the same number.
        if (Edge::sourceRank(source) < Edge::sourceRank(edge->source)) {
            return EDGE_NO_CHANGE;
        }

        float oldEtx = edge->getEtx();
        float absChange = fabs(etx - oldEtx);
        float relChange = (oldEtx > 0.0f) ? absChange / oldEtx : 1.0f;

        edge->setEtx(etx);
        if (updateTimestamp) {
            edge->lastUpdate = timestamp;
        }
        // Update EWMA variance only on direct observations (Reported edges)
        if (source == Edge::Source::Reported) {
            edge->updateEtxVariance(absChange);
        }
        edge->source = source;

        // Per-edge dirty threshold: noisy links need bigger jumps to trigger dirty
        float dynamicThreshold = etxChangeThreshold + edge->getEtxVariance();
        return (relChange > dynamicThreshold) ? EDGE_SIGNIFICANT_CHANGE : EDGE_NO_CHANGE;
    }

    // Add new edge
    if (node->edgeCount < NEIGHBOR_GRAPH_MAX_EDGES_PER_NODE) {
        edge = &node->edges[node->edgeCount++];
        edge->to = to;
        edge->setEtx(etx);
        edge->lastUpdate = timestamp;
        edge->etxVariance = 0;
        edge->source = source;

        // Remove redundant downstream entry now that we have a proper edge
        for (uint16_t i = 0; i < downstreamCount; i++) {
            if (downstream[i].destination == to && downstream[i].relay == from) {
                if (i < downstreamCount - 1)
                    downstream[i] = downstream[downstreamCount - 1];
                downstreamCount--;
                break;
            }
        }

        return EDGE_NEW;
    }

    // Edge list full - replace worst edge
    uint8_t worstIdx = 0xFF;
    float worstScore = 0;
    for (uint8_t i = 0; i < node->edgeCount; i++) {
        // Only edges this write is at least as well evidenced as may be displaced: a guess at
        // the nominal price outscores every real link and would otherwise evict the
        // measurements the ranking depends on.
        if (Edge::sourceRank(source) < Edge::sourceRank(node->edges[i].source)) {
            continue;
        }
        float edgeEtx = node->edges[i].getEtx();
        float ageSeconds = static_cast<float>(timestamp - node->edges[i].lastUpdate);
        float score = edgeEtx + ageSeconds / 300.0f;
        if (score > worstScore) {
            worstScore = score;
            worstIdx = i;
        }
    }

    float newScore = etx;
    if (worstIdx != 0xFF && newScore < worstScore) {
        edge = &node->edges[worstIdx];
        edge->to = to;
        edge->setEtx(etx);
        edge->lastUpdate = timestamp;
        edge->etxVariance = 0;
        edge->source = source;
        edge->hearsUs = false; // Reset on edge replacement — must be re-confirmed
        return EDGE_SIGNIFICANT_CHANGE;
    }

    return EDGE_NO_CHANGE;
}

void NeighborGraph::setEdgeHearsUs(NodeNum from, NodeNum to, bool hearsUs)
{
    NodeEdges *node = findNeighbor(from);
    if (!node) return;
    Edge *edge = findEdge(node, to);
    if (edge) edge->hearsUs = hearsUs;
}

const NodeEdges *NeighborGraph::getEdgesFrom(NodeNum node) const
{
    return findNeighbor(node);
}

void NeighborGraph::updateNodeActivity(NodeNum nodeId, uint32_t timestamp)
{
    // Refresh only. Creating an edgeless node here made every relayed source a graph entry that
    // ageEdges() removed on the next pass, and that removal deletes every edge in the graph
    // pointing at the node — including a gateway's own published measurement of it, which then
    // only returns on that gateway's next list.
    NodeEdges *node = findNeighbor(nodeId);
    if (node) {
        node->lastFullUpdate = timestamp;
    }
}

bool NeighborGraph::retainListedEdges(NodeNum sender, const NodeNum *listedIds, size_t listedCount)
{
    NodeEdges *node = findNeighbor(sender);
    if (!node || !listedIds) return false;

    uint8_t write = 0;
    bool removed = false;
    for (uint8_t e = 0; e < node->edgeCount; e++) {
        const Edge &edge = node->edges[e];
        bool listed = false;
        for (size_t i = 0; i < listedCount; i++) {
            if (listedIds[i] == edge.to) {
                listed = true;
                break;
            }
        }
        bool keep = edge.to == 0 || edge.source == Edge::Source::Reported ||
                    Edge::isPlaceholderId(edge.to) || listed;
        if (!keep) {
            removed = true;
            continue;
        }
        if (write != e) node->edges[write] = edge;
        write++;
    }
    node->edgeCount = write;
    return removed;
}

void NeighborGraph::ageEdges(uint32_t currentTimeSecs, uint32_t ttlSecs)
{
    nodeTtlSecs = ttlSecs;
    NodeNum myNode = nodeDB ? nodeDB->getNodeNum() : 0;
    uint16_t currentLo = static_cast<uint16_t>(currentTimeSecs & 0xFFFF);
    bool edgesRemoved = false;

    for (uint8_t n = 0; n < neighborCount;) {
        NodeEdges *node = &neighbors[n];

        if (node->nodeId == myNode) {
            n++;
            continue;
        }

        // Age individual edges
        uint8_t writeIdx = 0;
        for (uint8_t i = 0; i < node->edgeCount; i++) {
            if ((currentTimeSecs - node->edges[i].lastUpdate) <= ttlSecs) {
                if (writeIdx != i) {
                    node->edges[writeIdx] = node->edges[i];
                }
                writeIdx++;
            }
        }
        if (writeIdx < node->edgeCount) {
            edgesRemoved = true;
            node->edgeCount = writeIdx;
        }

        if (currentTimeSecs - node->lastFullUpdate > ttlSecs || node->edgeCount == 0) {
            // Remove downstream entries that reference this neighbor as relay
            NodeNum removedNode = node->nodeId;
            for (uint16_t i = 0; i < downstreamCount;) {
                if (downstream[i].relay == removedNode) {
                    if (i < downstreamCount - 1) {
                        downstream[i] = downstream[downstreamCount - 1];
                    }
                    downstreamCount--;
                } else {
                    i++;
                }
            }

            if (n < neighborCount - 1) {
                neighbors[n] = neighbors[neighborCount - 1];
            }
            neighborCount--;

            // Clear edges pointing TO the removed node from all remaining nodes
            for (uint8_t m = 0; m < neighborCount; m++) {
                for (uint8_t e = 0; e < neighbors[m].edgeCount;) {
                    if (neighbors[m].edges[e].to == removedNode) {
                        if (e < neighbors[m].edgeCount - 1) {
                            neighbors[m].edges[e] = neighbors[m].edges[neighbors[m].edgeCount - 1];
                        }
                        neighbors[m].edgeCount--;
                    } else {
                        e++;
                    }
                }
            }

            edgesRemoved = true;
            continue;
        }
        n++;
    }

    // Age downstream entries
    for (uint16_t i = 0; i < downstreamCount;) {
        if ((currentTimeSecs - downstream[i].lastUpdate) > ttlSecs) {
            if (i < downstreamCount - 1) {
                downstream[i] = downstream[downstreamCount - 1];
            }
            downstreamCount--;
            edgesRemoved = true;
        } else {
            // Also remove if the relay is no longer our neighbor
            if (!findNeighbor(downstream[i].relay)) {
                if (i < downstreamCount - 1) {
                    downstream[i] = downstream[downstreamCount - 1];
                }
                downstreamCount--;
                edgesRemoved = true;
            } else {
                i++;
            }
        }
    }

    // Age relay states
    for (uint8_t i = 0; i < relayStateCount;) {
        uint16_t age = currentLo - relayStates[i].timestampLo;
        if (age > 2) {
            if (i < relayStateCount - 1) {
                relayStates[i] = relayStates[relayStateCount - 1];
            }
            relayStateCount--;
            continue;
        }
        i++;
    }

    if (edgesRemoved) {
        routeCacheCount = 0;
    }
}

uint8_t NeighborGraph::countDirectNeighbors() const
{
    // Nodes we measured a direct link to — the set we publish. Counting peers that hold a
    // Reported edge back to us instead made this depend on our own symmetry assumption.
    NodeNum myNode = nodeDB ? nodeDB->getNodeNum() : 0;
    uint8_t count = 0;
    const NodeEdges *self = findNeighbor(myNode);
    if (self) {
        for (uint8_t e = 0; e < self->edgeCount; e++) {
            const Edge &edge = self->edges[e];
            if (edge.to != 0 && edge.to != myNode && edge.source == Edge::Source::Reported) {
                count++;
            }
        }
    }
    return count;
}

Route NeighborGraph::calculateRoute(NodeNum destination, uint32_t currentTime, const RoutePolicy &policy)
{
    // Check cache first
    Route cached = getCachedRoute(destination, currentTime);
    if (cached.nextHop != 0) {
        return cached;
    }

    Route result;
    result.destination = destination;
    result.nextHop = 0;
    result.costFixed = 0xFFFF;
    result.timestamp = currentTime;

    NodeNum myNode = nodeDB ? nodeDB->getNodeNum() : 0;
    if (myNode == 0) {
        return result;
    }

    // Dijkstra run backwards from the destination over "who hears whom" (see the declaration).
    // `cost` is the cost of the path from a node to the destination, `prev` the node after it.
    // Returns false when nothing confirmed (or, with allowUnverified, nothing at all) reaches us.
    static constexpr uint8_t MAX_DIJKSTRA = NEIGHBOR_GRAPH_MAX_NEIGHBORS;

    struct DNode {
        NodeNum id;
        uint16_t cost;
        NodeNum prev; // next node toward the destination
        bool visited;
    };
    DNode nodes[MAX_DIJKSTRA];
    uint8_t nodeCount = 0;

    auto search = [&](bool allowUnverified, uint16_t &costOut, NodeNum &nextHopOut, uint8_t &hopsOut) -> bool {
        nodeCount = 0;
        auto findOrAdd = [&](NodeNum id) -> int8_t {
            for (uint8_t i = 0; i < nodeCount; i++) {
                if (nodes[i].id == id) return i;
            }
            if (nodeCount >= MAX_DIJKSTRA) return -1;
            DNode &n = nodes[nodeCount];
            n.id = id;
            n.cost = 0xFFFF;
            n.prev = 0;
            n.visited = false;
            return nodeCount++;
        };
        // Lower cost[m] to cost + edgeCost with `via` as the next hop toward the destination.
        auto relax = [&](NodeNum m, NodeNum via, uint16_t cost, uint32_t edgeCost) {
            int8_t mIdx = findOrAdd(m);
            if (mIdx < 0 || nodes[mIdx].visited) return;
            uint32_t newCost = (uint32_t)cost + edgeCost;
            if (newCost > 0xFFFE) newCost = 0xFFFE;
            if ((uint16_t)newCost < nodes[mIdx].cost) {
                nodes[mIdx].cost = (uint16_t)newCost;
                nodes[mIdx].prev = via;
            }
        };

        int8_t dstIdx = findOrAdd(destination);
        if (dstIdx < 0) return false;
        nodes[dstIdx].cost = 0;

        for (;;) {
            int8_t uIdx = -1;
            uint16_t uCost = 0xFFFF;
            for (uint8_t i = 0; i < nodeCount; i++) {
                if (!nodes[i].visited && nodes[i].cost < uCost) {
                    uCost = nodes[i].cost;
                    uIdx = i;
                }
            }
            if (uIdx < 0 || uCost == 0xFFFF) break; // nothing else can reach the destination

            NodeNum n = nodes[uIdx].id;
            nodes[uIdx].visited = true;
            if (n == myNode) break;

            // Every settled node other than the destination would relay on this path.
            if (n != destination && policy.routable && !policy.routable(policy.ctx, n)) continue;

            // The nodes N hears, at the cost N measured on their signal: the true cost of M -> N.
            const NodeEdges *nEdges = findNeighbor(n);
            if (nEdges) {
                for (uint8_t e = 0; e < nEdges->edgeCount; e++) {
                    relax(nEdges->edges[e].to, n, uCost, nEdges->edges[e].etxFixed);
                }
            }
            // Nodes N confirmed hearing (hearsUs on their edge to N) and, for a node without lists,
            // anyone hearing N. Priced at the sender's measurement of N, the best available. In the
            // fallback pass an unconfirmed hop into a publishing node counts too, penalised.
            bool nPublishes = policy.publishes && policy.publishes(policy.ctx, n);
            for (uint8_t i = 0; i < neighborCount; i++) {
                NodeNum m = neighbors[i].nodeId;
                if (m == 0 || m == n) continue;
                if (nEdges && findEdge(nEdges, m)) continue; // already priced from N's own list
                const Edge *toN = findEdge(&neighbors[i], n);
                if (!toN) continue;
                if (toN->hearsUs || !nPublishes) {
                    relax(m, n, uCost, toN->etxFixed);
                } else if (allowUnverified) {
                    relax(m, n, uCost, (uint32_t)toN->etxFixed * UNVERIFIED_HOP_COST_FACTOR);
                }
            }
        }

        for (uint8_t i = 0; i < nodeCount; i++) {
            if (nodes[i].id == myNode && nodes[i].cost < 0xFFFF && nodes[i].prev != 0) {
                costOut = nodes[i].cost;
                nextHopOut = nodes[i].prev;
                NodeNum cur = nodes[i].prev;
                uint8_t hops = 1;
                while (cur != destination && cur != 0 && hops < MAX_DIJKSTRA) {
                    NodeNum next = 0;
                    for (uint8_t j = 0; j < nodeCount; j++) {
                        if (nodes[j].id == cur) {
                            next = nodes[j].prev;
                            break;
                        }
                    }
                    cur = next;
                    hops++;
                }
                hopsOut = hops;
                return true;
            }
        }
        return false;
    };

    {
        uint16_t cost;
        NodeNum nextHop;
        uint8_t hops;
        if (search(false, cost, nextHop, hops)) {
            result.costFixed = cost;
            result.nextHop = nextHop;
            result.hops = hops;
        }
    }

    // Fallback: downstream table only when Dijkstra found no route at all.
    // Never let the downstream estimate compete with a Dijkstra-computed cost,
    // since the edge graph gives us verified per-link costs.
    if (result.nextHop == 0) {
        const NodeEdges *myEdges = findNeighbor(myNode);
        for (uint16_t i = 0; i < downstreamCount; i++) {
            if (downstream[i].destination == destination) {
                if (policy.routable && !policy.routable(policy.ctx, downstream[i].relay)) continue;
                if (!findNeighbor(downstream[i].relay)) continue;

                uint16_t costToRelay = 0xFFFF;
                if (myEdges) {
                    for (uint8_t j = 0; j < myEdges->edgeCount; j++) {
                        if (myEdges->edges[j].to == downstream[i].relay) {
                            costToRelay = myEdges->edges[j].etxFixed;
                            break;
                        }
                    }
                }
                uint16_t totalCost = (costToRelay < 0xFFF0 && downstream[i].costFixed < 0xFFF0)
                                         ? costToRelay + downstream[i].costFixed
                                         : 0xFFFF;
                if (totalCost < result.costFixed) {
                    result.nextHop = downstream[i].relay;
                    result.costFixed = totalCost;
                    result.verified = false;
                }
            }
        }
    }

    // Inbound-gateway fallback: nothing confirmed reaches the destination, so let the node that
    // hears the far side try, at a penalty. A one-way edge is usually a marginal link or a
    // truncated list, not silence.
    if (result.nextHop == 0) {
        uint16_t cost;
        NodeNum nextHop;
        uint8_t hops;
        if (search(true, cost, nextHop, hops)) {
            result.costFixed = cost;
            result.nextHop = nextHop;
            result.hops = hops;
            result.verified = false;
        }
    }

    if (result.nextHop != 0) {
        if (routeCacheCount < NEIGHBOR_GRAPH_MAX_CACHED_ROUTES) {
            routeCache[routeCacheCount++] = result;
        } else {
            routeCache[0] = result;
        }
    }

    return result;
}

Route NeighborGraph::getCachedRoute(NodeNum destination, uint32_t currentTime)
{
    for (uint8_t i = 0; i < routeCacheCount; i++) {
        if (routeCache[i].destination == destination && (currentTime - routeCache[i].timestamp) < ROUTE_CACHE_TIMEOUT_SECS) {
            return routeCache[i];
        }
    }
    return Route();
}

void NeighborGraph::clearCache()
{
    routeCacheCount = 0;
}

// --- Downstream methods ---

void NeighborGraph::updateDownstream(NodeNum destination, NodeNum relay, float totalCost, uint32_t timestamp)
{
    if (destination == 0 || relay == 0 || destination == relay)
        return;

    // Don't add downstream entries for ourselves
    NodeNum myNode = nodeDB ? nodeDB->getNodeNum() : 0;
    if (destination == myNode)
        return;

    // Skip if the relay already has this destination as a direct edge — it's a neighbor, not downstream
    const NodeEdges *relayNode = findNeighbor(relay);
    if (relayNode && findEdge(relayNode, destination))
        return;

    uint16_t costFixed = static_cast<uint16_t>(std::min(totalCost * 100.0f, 65535.0f));

    // Update existing entry for the same (destination, relay) pair
    for (uint16_t i = 0; i < downstreamCount; i++) {
        if (downstream[i].destination == destination && downstream[i].relay == relay) {
            downstream[i].costFixed = costFixed;
            downstream[i].lastUpdate = timestamp;
            return;
        }
    }

    // Add new entry
    if (downstreamCount < NEIGHBOR_GRAPH_MAX_DOWNSTREAM) {
        DownstreamEntry &entry = downstream[downstreamCount++];
        entry.destination = destination;
        entry.relay = relay;
        entry.costFixed = costFixed;
        entry.lastUpdate = timestamp;
    } else {
        // Replace oldest entry
        uint16_t oldestIdx = 0;
        uint32_t oldestTime = downstream[0].lastUpdate;
        for (uint16_t i = 1; i < downstreamCount; i++) {
            if (downstream[i].lastUpdate < oldestTime) {
                oldestTime = downstream[i].lastUpdate;
                oldestIdx = i;
            }
        }
        downstream[oldestIdx].destination = destination;
        downstream[oldestIdx].relay = relay;
        downstream[oldestIdx].costFixed = costFixed;
        downstream[oldestIdx].lastUpdate = timestamp;
    }
}

void NeighborGraph::updateDownstreamExclusive(NodeNum destination, NodeNum relay, float totalCost, uint32_t timestamp)
{
    if (destination == 0 || relay == 0 || destination == relay)
        return;

    NodeNum myNode = nodeDB ? nodeDB->getNodeNum() : 0;
    if (destination == myNode)
        return;

    // Skip if the relay already has this destination as a direct edge — it's a neighbor, not downstream
    const NodeEdges *relayNode = findNeighbor(relay);
    if (relayNode && findEdge(relayNode, destination))
        return;

    uint16_t costFixed = static_cast<uint16_t>(std::min(totalCost * 100.0f, 65535.0f));

    // Remove all existing entries for this destination (regardless of relay) to avoid
    // duplicates when mixing with updateDownstream() calls on the same destination.
    for (uint16_t i = 0; i < downstreamCount; ) {
        if (downstream[i].destination == destination) {
            downstream[i] = downstream[--downstreamCount];
        } else {
            i++;
        }
    }

    // No existing entry — add new
    if (downstreamCount < NEIGHBOR_GRAPH_MAX_DOWNSTREAM) {
        DownstreamEntry &entry = downstream[downstreamCount++];
        entry.destination = destination;
        entry.relay = relay;
        entry.costFixed = costFixed;
        entry.lastUpdate = timestamp;
    } else {
        // Replace oldest entry
        uint16_t oldestIdx = 0;
        uint32_t oldestTime = downstream[0].lastUpdate;
        for (uint16_t i = 1; i < downstreamCount; i++) {
            if (downstream[i].lastUpdate < oldestTime) {
                oldestTime = downstream[i].lastUpdate;
                oldestIdx = i;
            }
        }
        downstream[oldestIdx].destination = destination;
        downstream[oldestIdx].relay = relay;
        downstream[oldestIdx].costFixed = costFixed;
        downstream[oldestIdx].lastUpdate = timestamp;
    }
}

NodeNum NeighborGraph::getDownstreamRelay(NodeNum destination) const
{
    uint32_t now = millis() / 1000;
    NodeNum bestRelay = 0;
    uint16_t bestCost = UINT16_MAX;
    for (uint16_t i = 0; i < downstreamCount; i++) {
        if (downstream[i].destination == destination && (now - downstream[i].lastUpdate) < nodeTtlSecs) {
            if (downstream[i].costFixed < bestCost) {
                bestCost = downstream[i].costFixed;
                bestRelay = downstream[i].relay;
            }
        }
    }
    return bestRelay;
}

bool NeighborGraph::isDownstream(NodeNum destination) const
{
    return getDownstreamRelay(destination) != 0;
}

size_t NeighborGraph::getDownstreamCountForRelay(NodeNum relay) const
{
    size_t count = 0;
    uint32_t now = millis() / 1000;
    for (uint16_t i = 0; i < downstreamCount; i++) {
        if (downstream[i].relay == relay && (now - downstream[i].lastUpdate) < nodeTtlSecs) {
            count++;
        }
    }
    return count;
}

size_t NeighborGraph::getDownstreamNodesForRelay(NodeNum relay, NodeNum *outArray, uint16_t *outCosts, size_t maxCount,
                                                  size_t skipCount) const
{
    size_t count = 0;
    size_t skipped = 0;
    uint32_t now = millis() / 1000;
    for (uint16_t i = 0; i < downstreamCount && count < maxCount; i++) {
        if (downstream[i].relay == relay && (now - downstream[i].lastUpdate) < nodeTtlSecs) {
            if (skipped < skipCount) {
                skipped++;
                continue;
            }
            outArray[count] = downstream[i].destination;
            if (outCosts) outCosts[count] = downstream[i].costFixed;
            count++;
        }
    }
    return count;
}

bool NeighborGraph::isRelayFor(NodeNum myNode, NodeNum destination) const
{
    for (uint16_t i = 0; i < downstreamCount; i++) {
        if (downstream[i].destination == destination && downstream[i].relay == myNode) {
            uint32_t now = millis() / 1000;
            if ((now - downstream[i].lastUpdate) < nodeTtlSecs) {
                return true;
            }
        }
    }
    return false;
}

void NeighborGraph::clearDownstreamForRelay(NodeNum relay)
{
    for (uint16_t i = 0; i < downstreamCount;) {
        if (downstream[i].relay == relay) {
            if (i < downstreamCount - 1) {
                downstream[i] = downstream[downstreamCount - 1];
            }
            downstreamCount--;
        } else {
            i++;
        }
    }
}

size_t NeighborGraph::transferDownstream(NodeNum oldRelay, NodeNum newRelay)
{
    uint32_t now = millis() / 1000;
    size_t count = 0;
    // First pass: add entries under newRelay
    for (uint16_t i = 0; i < downstreamCount; i++) {
        if (downstream[i].relay == oldRelay) {
            updateDownstream(downstream[i].destination, newRelay, downstream[i].costFixed / 100.0f, now);
            count++;
        }
    }
    // Second pass: remove old entries
    clearDownstreamForRelay(oldRelay);
    return count;
}

void NeighborGraph::clearDownstreamForDestination(NodeNum destination)
{
    for (uint16_t i = 0; i < downstreamCount;) {
        if (downstream[i].destination == destination) {
            if (i < downstreamCount - 1) {
                downstream[i] = downstream[downstreamCount - 1];
            }
            downstreamCount--;
        } else {
            i++;
        }
    }
}

// --- Static methods ---

float NeighborGraph::calculateETX(int32_t rssi, float snr)
{
    static constexpr int32_t rssiBreak[] = {-110, -100, -90, -80, -70, -60};
    static constexpr float probBreak[] = {0.05f, 0.15f, 0.40f, 0.65f, 0.85f, 0.95f};
    static constexpr int N = 6;

    float deliveryProb;
    if (rssi <= rssiBreak[0]) {
        deliveryProb = probBreak[0];
    } else if (rssi >= rssiBreak[N - 1]) {
        deliveryProb = probBreak[N - 1];
    } else {
        int seg = 0;
        for (int i = 1; i < N; i++) {
            if (rssi < rssiBreak[i]) {
                seg = i - 1;
                break;
            }
        }
        float t = static_cast<float>(rssi - rssiBreak[seg]) / static_cast<float>(rssiBreak[seg + 1] - rssiBreak[seg]);
        deliveryProb = probBreak[seg] + t * (probBreak[seg + 1] - probBreak[seg]);
    }

    float snrFactor;
    if (snr <= 0.0f) {
        snrFactor = 0.5f;
    } else if (snr >= 10.0f) {
        snrFactor = 1.0f;
    } else {
        snrFactor = 0.5f + snr * 0.05f;
    }
    deliveryProb *= snrFactor;

    return (deliveryProb > 0.0f) ? (1.0f / deliveryProb) : 100.0f;
}

void NeighborGraph::etxToSignal(float etx, int32_t &rssi, int32_t &snr)
{
    static constexpr int32_t rssiBreak[] = {-110, -100, -90, -80, -70, -60};
    static constexpr float probBreak[] = {0.05f, 0.15f, 0.40f, 0.65f, 0.85f, 0.95f};
    static constexpr int N = 6;

    float prob = 1.0f / std::max(etx, 1.0f);

    if (prob <= probBreak[0]) {
        rssi = rssiBreak[0];
    } else if (prob >= probBreak[N - 1]) {
        rssi = rssiBreak[N - 1];
    } else {
        int seg = 0;
        for (int i = 1; i < N; i++) {
            if (prob < probBreak[i]) {
                seg = i - 1;
                break;
            }
        }
        float t = (prob - probBreak[seg]) / (probBreak[seg + 1] - probBreak[seg]);
        rssi = rssiBreak[seg] + static_cast<int32_t>(t * (rssiBreak[seg + 1] - rssiBreak[seg]));
    }

    float etxAtSnr10 = calculateETX(rssi, 10.0f);
    if (etx <= etxAtSnr10 * 1.05f) {
        snr = 10;
    } else {
        float snrFactor = etxAtSnr10 / etx;
        if (snrFactor < 0.5f)
            snrFactor = 0.5f;
        float snrFloat = (snrFactor - 0.5f) / 0.05f;
        snr = static_cast<int32_t>(snrFloat);
        if (snr < -5)
            snr = -5;
        if (snr > 10)
            snr = 10;
    }
}

uint32_t NeighborGraph::getContentionWindowMs()
{
    // Dynamic contention window based on LoRa settings
    // This matches the behavior from Graph::getContentionWindowMs()
    uint32_t baseWindow = 2000; // 2 seconds base

    // Scale based on air time (LoRa modem config)
    // Faster presets need shorter windows, slower ones need longer
    if (config.lora.modem_preset == meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST ||
        config.lora.modem_preset == meshtastic_Config_LoRaConfig_ModemPreset_LONG_MODERATE) {
        baseWindow = 3000;
    } else if (config.lora.modem_preset == meshtastic_Config_LoRaConfig_ModemPreset_VERY_LONG_SLOW ||
               config.lora.modem_preset == meshtastic_Config_LoRaConfig_ModemPreset_LONG_SLOW) {
        baseWindow = 5000;
    } else if (config.lora.modem_preset == meshtastic_Config_LoRaConfig_ModemPreset_SHORT_TURBO ||
               config.lora.modem_preset == meshtastic_Config_LoRaConfig_ModemPreset_SHORT_FAST) {
        baseWindow = 1500;
    }

    return baseWindow;
}

// --- Node management ---

uint8_t NeighborGraph::getNeighborCount(NodeNum node) const
{
    const NodeEdges *n = findNeighbor(node);
    return n ? n->edgeCount : 0;
}

size_t NeighborGraph::getAllNodeIds(NodeNum *outArray, size_t maxCount) const
{
    size_t count = 0;
    for (uint8_t i = 0; i < neighborCount && count < maxCount; i++) {
        outArray[count++] = neighbors[i].nodeId;
    }
    return count;
}

void NeighborGraph::removeNode(NodeNum nodeId)
{
    for (uint8_t n = 0; n < neighborCount; n++) {
        if (neighbors[n].nodeId == nodeId) {
            if (n < neighborCount - 1) {
                neighbors[n] = neighbors[neighborCount - 1];
            }
            neighborCount--;

            // Clear edges pointing TO the removed node from all other nodes
            for (uint8_t m = 0; m < neighborCount; m++) {
                for (uint8_t e = 0; e < neighbors[m].edgeCount;) {
                    if (neighbors[m].edges[e].to == nodeId) {
                        if (e < neighbors[m].edgeCount - 1) {
                            neighbors[m].edges[e] = neighbors[m].edges[neighbors[m].edgeCount - 1];
                        }
                        neighbors[m].edgeCount--;
                    } else {
                        e++;
                    }
                }
            }

            // Clear downstream entries referencing this node as relay or destination.
            // With multi-relay support, other relay entries for the same destination survive.
            for (uint16_t i = 0; i < downstreamCount;) {
                if (downstream[i].relay == nodeId || downstream[i].destination == nodeId) {
                    if (i < downstreamCount - 1) {
                        downstream[i] = downstream[downstreamCount - 1];
                    }
                    downstreamCount--;
                } else {
                    i++;
                }
            }

            // Clear route cache entries
            for (uint8_t i = 0; i < routeCacheCount;) {
                if (routeCache[i].destination == nodeId || routeCache[i].nextHop == nodeId) {
                    for (uint8_t j = i; j < routeCacheCount - 1; j++) {
                        routeCache[j] = routeCache[j + 1];
                    }
                    routeCacheCount--;
                } else {
                    i++;
                }
            }
            return;
        }
    }
}

void NeighborGraph::clearEdgesForNode(NodeNum nodeId)
{
    NodeEdges *node = findNeighbor(nodeId);
    if (node) {
        uint8_t writeIdx = 0;
        for (uint8_t i = 0; i < node->edgeCount; i++) {
            if (node->edges[i].source == Edge::Source::Reported) {
                if (writeIdx != i) {
                    node->edges[writeIdx] = node->edges[i];
                }
                writeIdx++;
            }
        }
        node->edgeCount = writeIdx;
    }

    for (uint8_t i = 0; i < routeCacheCount;) {
        if (routeCache[i].destination == nodeId || routeCache[i].nextHop == nodeId) {
            for (uint8_t j = i; j < routeCacheCount - 1; j++) {
                routeCache[j] = routeCache[j + 1];
            }
            routeCacheCount--;
        } else {
            i++;
        }
    }
}

bool NeighborGraph::removeEdge(NodeNum from, NodeNum to)
{
    NodeEdges *node = findNeighbor(from);
    if (!node) return false;
    for (uint8_t e = 0; e < node->edgeCount; e++) {
        if (node->edges[e].to != to) continue;
        if (e < node->edgeCount - 1) {
            node->edges[e] = node->edges[node->edgeCount - 1];
        }
        node->edgeCount--;
        return true;
    }
    return false;
}

uint8_t NeighborGraph::pruneSilentPublishers(NodeNum myNode, uint32_t currentTimeSecs, uint32_t silenceSecs,
                                             const CoveragePolicy *policy)
{
    if (!policy) return 0;
    NodeEdges *self = findNeighbor(myNode);
    if (!self) return 0;

    NodeNum gone[NEIGHBOR_GRAPH_MAX_NEIGHBORS];
    uint8_t count = 0;
    for (uint8_t i = 0; i < self->edgeCount && count < NEIGHBOR_GRAPH_MAX_NEIGHBORS; i++) {
        NodeNum target = self->edges[i].to;
        if (target == 0 || target == myNode) continue;
        if (!policy->reports(target)) continue;
        // Guarded against a backwards clock: a wrap must not retract every link at once.
        uint32_t silent = currentTimeSecs - self->edges[i].lastUpdate;
        if (silent > silenceSecs && silent < 0x80000000u) {
            gone[count++] = target;
        }
    }

    for (uint8_t i = 0; i < count; i++) {
        removeEdge(myNode, gone[i]);
        removeEdge(gone[i], myNode);
        LOG_INFO("[SR] %08x silent for %us — direct link retracted", gone[i], silenceSecs);
    }
    return count;
}

void NeighborGraph::removeEdgesTo(NodeNum nodeId)
{
    for (uint8_t m = 0; m < neighborCount; m++) {
        for (uint8_t e = 0; e < neighbors[m].edgeCount;) {
            if (neighbors[m].edges[e].to == nodeId) {
                if (e < neighbors[m].edgeCount - 1) {
                    neighbors[m].edges[e] = neighbors[m].edges[neighbors[m].edgeCount - 1];
                }
                neighbors[m].edgeCount--;
            } else {
                e++;
            }
        }
    }
}

void NeighborGraph::clearInferredEdgesToNode(NodeNum nodeId)
{
    for (uint8_t nodeIdx = 0; nodeIdx < neighborCount; nodeIdx++) {
        NodeEdges *node = &neighbors[nodeIdx];
        if (node->nodeId == 0)
            continue;

        uint8_t writeIdx = 0;
        for (uint8_t i = 0; i < node->edgeCount; i++) {
            if (!(node->edges[i].to == nodeId && node->edges[i].source == Edge::Source::Mirrored)) {
                if (writeIdx != i) {
                    node->edges[writeIdx] = node->edges[i];
                }
                writeIdx++;
            }
        }
        node->edgeCount = writeIdx;
    }

    routeCacheCount = 0;
}

// --- Relay decisions (ported from GraphLite) ---

bool NeighborGraph::knownToHear(NodeNum from, NodeNum to) const
{
    const NodeEdges *fromEdges = findNeighbor(from);
    const Edge *forward = fromEdges ? findEdge(fromEdges, to) : nullptr;
    if (forward && forward->hearsUs) return true;
    const NodeEdges *toEdges = findNeighbor(to);
    return toEdges && findEdge(toEdges, from);
}

float NeighborGraph::hopCost(NodeNum from, NodeNum to) const
{
    // An Inferred edge is skipped: it was minted at a nominal price because a frame once
    // crossed the link, which says a path exists and nothing about what it costs. Everything
    // that decides whether a transmission may be skipped reads this price — covers(),
    // coverageOwner(), witnessOwner() and the slot rankings — so a guess must not produce one.
    // The route search prices its own hops from the edges directly and keeps using inferred
    // edges for reachability, which is what they exist for.
    const NodeEdges *toEdges = findNeighbor(to);
    if (const Edge *received = toEdges ? findEdge(toEdges, from) : nullptr) {
        if (Edge::isMeasured(received->source)) return received->getEtx();
    }
    const NodeEdges *fromEdges = findNeighbor(from);
    if (const Edge *sent = fromEdges ? findEdge(fromEdges, to) : nullptr) {
        if (Edge::isMeasured(sent->source)) return sent->getEtx();
    }
    return 0.0f;
}

bool NeighborGraph::canDeliver(NodeNum from, NodeNum to, const RoutePolicy &policy) const
{
    if (knownToHear(from, to)) return true;
    return !(policy.publishes && policy.publishes(policy.ctx, to));
}

bool NeighborGraph::isSilentPublisher(NodeNum node, const CoveragePolicy &policy) const
{
    if (policy.publisherSilenceSecs == 0 || !policy.reports(node)) return false;
    const NodeEdges *entry = findNeighbor(node);
    if (!entry) return false;
    uint32_t silent = policy.nowSecs - entry->lastFullUpdate;
    return silent > policy.publisherSilenceSecs && silent < 0x80000000u;
}

bool NeighborGraph::admitsCoverage(NodeNum relay, NodeNum target, float poorLinkEtx,
                                   const CoveragePolicy *policy) const
{
    // A publisher that has gone quiet is nobody's target, however good the link a peer published
    // to it: our own link to it is already retracted, and crediting peers with reaching it hands
    // them a slot they will decline.
    if (policy && isSilentPublisher(target, *policy)) return false;
    if (covers(relay, target, poorLinkEtx, policy)) return true;
    // A neighbour nobody can be shown to reach is still worth one relay, but only from its owner.
    return policy && coverageOwner(target, *policy) == relay;
}

bool NeighborGraph::covers(NodeNum from, NodeNum to, float poorLinkEtx, const CoveragePolicy *policy) const
{
    bool reports = policy && policy->reports(to);
    bool evidenced = knownToHear(from, to);
    if (!evidenced && !reports) {
        // All anyone can ever measure about a silent node is that someone hears it.
        const NodeEdges *fromEdges = findNeighbor(from);
        evidenced = fromEdges && findEdge(fromEdges, to);
    }
    if (!evidenced) return false;
    // A zero threshold asks this primitive for the evidence question alone. The firmware cannot
    // reach it: the policy carries the configured ceiling and a non-positive configuration is
    // rejected, so only direct graph-level callers see this.
    if (poorLinkEtx <= 0.0f) return true;
    float cost = hopCost(from, to);
    return cost > 0.0f && cost <= poorLinkEtx;
}

NodeNum NeighborGraph::witnessOwner(NodeNum source, const CoveragePolicy &policy) const
{
    NodeNum me = policy.me;
    if (source == 0 || (source & 0xFF000000) == 0xFF000000) return 0;
    NodeNum owner = 0;
    uint8_t bestTier = 0xFF;
    uint16_t bestBucket = 0xFFFF;
    for (uint8_t i = 0; i < neighborCount; i++) {
        NodeNum candidate = neighbors[i].nodeId;
        if (candidate == 0 || candidate == source || (candidate & 0xFF000000) == 0xFF000000) continue;
        uint8_t tier;
        if (candidate == me) {
            if (!policy.meRelays) continue;
            tier = 1;
        } else if (policy.isStockRelayRouter && policy.isStockRelayRouter(policy.ctx, candidate)) {
            tier = 0;
        } else if (policy.isSrActive && policy.isSrActive(policy.ctx, candidate)) {
            tier = 1;
        } else {
            continue;
        }
        // Positive evidence that the source hears this candidate: the source's own list named
        // it, or we watched the source carry its frame. A direct observation writes both edge
        // directions from one measurement, so edge existence alone is our own assumption of
        // symmetry — the one thing a witness may not assume, since a copy the originator cannot
        // hear acknowledges nothing.
        const Edge *edge = findEdge(&neighbors[i], source);
        if (!edge || !edge->hearsUs) continue;
        // A guessed link carries no acknowledgement: it was never measured in either direction.
        if (!Edge::isMeasured(edge->source)) continue;
        // Priced in the delivery direction: the source's own measurement of the candidate when
        // it published one, our own edge otherwise. A link past the coverage ceiling carries no
        // acknowledgement either.
        uint16_t costFixed = edge->etxFixed;
        const NodeEdges *sourceEdges = getEdgesFrom(source);
        if (sourceEdges) {
            const Edge *back = findEdge(sourceEdges, candidate);
            if (back) costFixed = back->etxFixed;
        }
        if (policy.poorLinkEtx > 0.0f && (float)costFixed / 100.0f > policy.poorLinkEtx) continue;
        uint16_t bucket = (uint16_t)(costFixed / SR_OWNER_COST_BUCKET);
        if (tier < bestTier || (tier == bestTier && bucket < bestBucket) ||
            (tier == bestTier && bucket == bestBucket && candidate < owner)) {
            bestTier = tier;
            bestBucket = bucket;
            owner = candidate;
        }
    }
    return owner;
}

NodeNum NeighborGraph::coverageOwner(NodeNum target, const CoveragePolicy &policy) const
{
    NodeNum me = policy.me;
    if (target == 0 || (target & 0xFF000000) == 0xFF000000) return 0;
    // Only a silent neighbour has an owner. One that publishes topology and does not list a
    // candidate has reported that the candidate cannot reach it, and that silence is evidence:
    // nobody owns it, and a relay spent on it would be spent against its own report.
    if (policy.reports(target)) return 0;
    NodeNum owner = 0;
    uint8_t bestTier = 0xFF;
    uint16_t bestBucket = 0xFFFF;
    for (uint8_t i = 0; i < neighborCount; i++) {
        NodeNum candidate = neighbors[i].nodeId;
        if (candidate == 0 || candidate == target || (candidate & 0xFF000000) == 0xFF000000) continue;
        // Only its own measurement counts: for a stock node the edge exists only because we
        // watched it carry the target's traffic.
        const Edge *edge = findEdge(&neighbors[i], target);
        if (!edge) continue;
        // A guessed link owns nothing: it was never measured, so its holder cannot be shown to
        // deliver to the target at all.
        if (!Edge::isMeasured(edge->source)) continue;
        // Ownership decides *who* carries a neighbour nobody can be shown to reach; it must not
        // decide *whether* the neighbour is reachable at all. A link past the coverage ceiling
        // (the "heard once" ETX 40 sentinel included) delivers nothing, so its holder owns
        // nothing: the ranking would otherwise credit it with unique coverage and hand it the
        // first slot, and the packet would wait a full defer window for a relay that cannot
        // come. Measured 2026-09-08: 74 of 183 slots went out over links worse than the
        // ceiling, and the branch's insurance fired to cover them.
        if (policy.poorLinkEtx > 0.0f && edge->getEtx() > policy.poorLinkEtx) continue;
        uint8_t tier;
        if (candidate == me) {
            if (!policy.meRelays) continue;
            tier = 1;
        } else if (policy.isStockRelayRouter && policy.isStockRelayRouter(policy.ctx, candidate)) {
            tier = 0;
        } else if (policy.isSrActive && policy.isSrActive(policy.ctx, candidate)) {
            tier = 1;
        } else {
            continue;
        }
        uint16_t bucket = (uint16_t)(edge->etxFixed / SR_OWNER_COST_BUCKET);
        if (tier < bestTier || (tier == bestTier && bucket < bestBucket) ||
            (tier == bestTier && bucket == bestBucket && candidate < owner)) {
            bestTier = tier;
            bestBucket = bucket;
            owner = candidate;
        }
    }
    return owner;
}

size_t NeighborGraph::getCoverageIfRelays(NodeNum relay, NodeNum *coveredNodes, size_t maxNodes,
                                           const NodeNum *alreadyCovered, size_t alreadyCoveredCount,
                                           NodeNum selfNode, const CoveragePolicy *policy) const
{
    float poorLinkEtx = policy ? policy->poorLinkEtx : 0.0f;
    if (!coveredNodes || maxNodes == 0)
        return 0;

    size_t coveredCount = 0;
    const NodeEdges *relayEdges = findNeighbor(relay);
    if (!relayEdges)
        return 0;

    for (uint8_t i = 0; i < relayEdges->edgeCount && coveredCount < maxNodes; i++) {
        // Our own Mirrored edges (nodes we only know relayed us, or that listed us) are invisible to
        // our peers: they rank us on what we report. Counting them here made every node see more
        // coverage for itself than its neighbours saw for it, and colocated nodes both took slot 0.
        // A candidate's coverage set is what it published, and for ourselves what we publish. An
        // edge we invented from a relayed frame is invisible to everyone, the candidate it is
        // attributed to included, so it belongs in nobody's set.
        if (!Edge::isMeasured(relayEdges->edges[i].source)) continue;
        if (relay == selfNode && selfNode != 0 && relayEdges->edges[i].source != Edge::Source::Reported)
            continue;
        NodeNum target = relayEdges->edges[i].to;
        // Not coverage unless the relay is shown to reach it — or, when nobody can be shown to
        // reach it at all, unless this relay is its owner. Counting it for everyone made the whole
        // branch relay every frame for the same unconfirmed node.
        if (!admitsCoverage(relay, target, poorLinkEtx, policy)) {
            continue;
        }

        bool isAlreadyCovered = false;
        for (size_t j = 0; j < alreadyCoveredCount; j++) {
            if (alreadyCovered[j] == target) {
                isAlreadyCovered = true;
                break;
            }
        }

        if (!isAlreadyCovered) {
            coveredNodes[coveredCount++] = target;
        }
    }

    return coveredCount;
}

RelayCandidate NeighborGraph::findBestRelayCandidate(const NodeSet &candidates, const NodeSet &alreadyCovered,
                                                          uint32_t currentTime, uint32_t packetId,
                                                          bool preferHighNodeId, NodeNum sourceNode,
                                                          NodeNum selfNode, const CoveragePolicy *policy) const
{
    RelayCandidate bestCandidate(0, 0, 0, 0);

    for (uint16_t ci = 0; ci < candidates.count; ci++) {
        NodeNum candidate = candidates.nodes[ci];
        if (hasNodeTransmitted(candidate, packetId, currentTime)) {
            continue;
        }

        NodeNum newCoverage[NODE_SET_MAX];
        size_t coverageCount =
            getCoverageIfRelays(candidate, newCoverage, NODE_SET_MAX, nullptr, 0, selfNode, policy);

        size_t uniqueCoverageCount = 0;
        for (size_t i = 0; i < coverageCount; i++) {
            if (!alreadyCovered.contains(newCoverage[i])) {
                newCoverage[uniqueCoverageCount++] = newCoverage[i];
            }
        }

        if (uniqueCoverageCount == 0) {
            continue;
        }

        // Price each covered hop at its receiver: the candidate's own edge measures the other
        // direction (what it hears), which is not the cost of delivering to that node.
        float totalCost = 0;
        size_t validCosts = 0;
        const NodeEdges *candidateEdges = findNeighbor(candidate);
        for (size_t j = 0; j < uniqueCoverageCount; j++) {
            float cost = hopCost(candidate, newCoverage[j]);
            if (cost > 0.0f) {
                totalCost += cost;
                validCosts++;
            }
        }

        if (validCosts == 0)
            continue;

        float avgCost = totalCost / validCosts;
        uint16_t avgCostFixed = static_cast<uint16_t>(avgCost * 100);
        avgCostFixed = (uint16_t)(avgCostFixed / SR_COST_BUCKET_FIXED * SR_COST_BUCKET_FIXED); // half-ETX buckets

        // Bidirectional link priority: candidates that can deliver back to the source
        // (hearsUs=true on their edge to sourceNode) get a higher tier. This ensures
        // nodes with confirmed round-trip connectivity relay first, so both SR and
        // stock nodes on the branch discover the correct gateway.
        // ETX above this threshold is too poor to be considered a usable bidirectional link.
        // ETX=40 is the worst possible calculated value (RSSI=-110, SNR<=0) and is essentially
        // a placeholder for "heard once, barely" — not a deliverable link.
        static constexpr float BIDI_ETX_CEILING = 20.0f;

        uint8_t candidateTier = 0;
        if (sourceNode != 0 && candidateEdges) {
            const Edge *srcEdge = findEdge(candidateEdges, sourceNode);
            if (srcEdge && srcEdge->hearsUs && srcEdge->getEtx() < BIDI_ETX_CEILING) {
                candidateTier = 1;
            }
        }

        bool isBetter = candidateTier > bestCandidate.tier ||
                        (candidateTier == bestCandidate.tier && uniqueCoverageCount > bestCandidate.coverageCount) ||
                        (candidateTier == bestCandidate.tier && uniqueCoverageCount == bestCandidate.coverageCount &&
                         avgCostFixed < bestCandidate.avgCostFixed);
        // Deterministic tiebreak on node ID when tier, coverage and cost are equal
        if (!isBetter && candidateTier == bestCandidate.tier &&
            uniqueCoverageCount == bestCandidate.coverageCount &&
            avgCostFixed == bestCandidate.avgCostFixed && bestCandidate.nodeId != 0) {
            isBetter = preferHighNodeId ? (candidate > bestCandidate.nodeId)
                                        : (candidate < bestCandidate.nodeId);
        }
        if (isBetter) {
            bestCandidate = RelayCandidate(candidate, uniqueCoverageCount, avgCostFixed, candidateTier,
                                           static_cast<uint8_t>(coverageCount));
        }
    }

    return bestCandidate;
}

NodeNum NeighborGraph::uniqueCoverageNeighbor(NodeNum myNode, const NodeNum *coveredBy, size_t coveredByCount,
                                              float poorLinkEtx, const CoveragePolicy *policy) const
{
    const NodeEdges *myEdges = findNeighbor(myNode);
    if (!myEdges || myEdges->edgeCount == 0) {
        return 0;
    }

    for (uint8_t i = 0; i < myEdges->edgeCount; i++) {
        NodeNum neighbor = myEdges->edges[i].to;

        // Placeholder neighbors have unknown identity — they could be any of the
        // coveredBy nodes' neighbors, so don't count them as unique coverage
        if ((neighbor & 0xFF000000) == 0xFF000000) {
            continue;
        }

        // A publisher we have stopped hearing is nobody's coverage target, so it cannot be ours
        // either. admitsCoverage() has applied this since the silence rule was introduced and this
        // path did not, so a queued relay was kept alive for a node the ranking had already agreed
        // nobody could carry. One rule, read the same way on both sides.
        if (policy && isSilentPublisher(neighbor, *policy)) {
            continue;
        }

        // Skip nodes that are themselves in the coveredBy set
        bool isCoverer = false;
        for (size_t c = 0; c < coveredByCount; c++) {
            if (neighbor == coveredBy[c]) {
                isCoverer = true;
                break;
            }
        }
        if (isCoverer) continue;

        // Ours to cover: it proved it hears us, or nobody can prove anything about it and we are
        // its owner. Otherwise it is another node's responsibility, or nobody's.
        if (!myEdges->edges[i].hearsUs && policy && coverageOwner(neighbor, *policy) != myNode) {
            continue;
        }
        // Confirmation or ownership says whose it is; covers() says whether a copy from us would
        // arrive at all. hearsUs is sticky, so without this a neighbour behind a decayed link
        // stayed "ours to cover" and kept a queued relay the ranking refuses.
        if (!covers(myNode, neighbor, poorLinkEtx, policy)) {
            continue;
        }

        // A coverer counts only when it is shown to reach the neighbour over a link that is not
        // hopeless (the 40.0 "heard once" sentinel included).
        bool covered = false;
        for (size_t c = 0; c < coveredByCount && !covered; c++) {
            covered = covers(coveredBy[c], neighbor, poorLinkEtx, policy);
        }

        if (!covered) {
            LOG_INFO("[SR] Relaying for %08x (no transmitter reaches it)", neighbor);
            return neighbor;
        }
    }

    return 0;
}

bool NeighborGraph::isGatewayNode(NodeNum nodeId, NodeNum sourceNode) const
{
    const NodeEdges *nodeEdges = findNeighbor(nodeId);
    const NodeEdges *sourceEdges = findNeighbor(sourceNode);

    if (!nodeEdges || nodeEdges->edgeCount == 0) {
        return false;
    }

    for (uint8_t i = 0; i < nodeEdges->edgeCount; i++) {
        NodeNum neighbor = nodeEdges->edges[i].to;
        if (neighbor == sourceNode)
            continue;

        bool sourceHasNeighbor = false;
        if (sourceEdges) {
            for (uint8_t j = 0; j < sourceEdges->edgeCount; j++) {
                if (sourceEdges->edges[j].to == neighbor) {
                    sourceHasNeighbor = true;
                    break;
                }
            }
        }

        if (!sourceHasNeighbor) {
            const NodeEdges *neighborEdges = findNeighbor(neighbor);
            if (neighborEdges && neighborEdges->edgeCount > 1) {
                return true;
            }
        }
    }

    return false;
}

bool NeighborGraph::shouldRelayEnhanced(NodeNum myNode, NodeNum sourceNode, NodeNum heardFrom, uint32_t currentTime,
                                         uint32_t packetId, uint32_t packetRxTime,
                                         const NodeNum *coListeners, uint8_t coListenerCount) const
{
    NodeSet alreadyCovered;
    alreadyCovered.insert(sourceNode);
    alreadyCovered.insert(heardFrom);

    const NodeEdges *transmittingEdges = findNeighbor(heardFrom);
    if (transmittingEdges) {
        for (uint8_t i = 0; i < transmittingEdges->edgeCount; i++) {
            alreadyCovered.insert(transmittingEdges->edges[i].to);
        }
    }

    NodeSet candidates;
    if (transmittingEdges) {
        for (uint8_t i = 0; i < transmittingEdges->edgeCount; i++) {
            candidates.insert(transmittingEdges->edges[i].to);
        }
    }

    // When heardFrom has no edges in the graph (stock node), SR neighbors that
    // also heard the packet won't appear as candidates. Add them so the relay
    // selection can coordinate and pick a single relay.
    if (coListeners && coListenerCount > 0) {
        for (uint8_t i = 0; i < coListenerCount; i++) {
            if (coListeners[i] != myNode && coListeners[i] != heardFrom) {
                candidates.insert(coListeners[i]);
            }
        }
        // Also add ourselves so findBestRelayCandidate can pick us
        candidates.insert(myNode);
    }

    bool preferHighNodeId = (packetId & 1) != 0;

    while (!candidates.empty()) {
        RelayCandidate bestCandidate = findBestRelayCandidate(candidates, alreadyCovered, currentTime, packetId,
                                                               preferHighNodeId, sourceNode);

        if (bestCandidate.nodeId == 0) {
            break;
        }

        if (bestCandidate.nodeId == myNode) {
            return true;
        }

        if (isGatewayNode(myNode, sourceNode)) {
            return true;
        }

        bool bestHasTransmitted = hasNodeTransmitted(bestCandidate.nodeId, packetId, currentTime);

        if (!bestHasTransmitted) {
            if (packetRxTime > 0) {
                uint32_t timeSinceRx = currentTime - packetRxTime;
                uint32_t contentionWindowMs = getContentionWindowMs();
                if (timeSinceRx > (contentionWindowMs + 500)) {
                    candidates.erase(bestCandidate.nodeId);
                    continue;
                }
            }
            return false;
        }

        NodeSet relayCoverage;
        for (uint16_t ci = 0; ci < candidates.count; ci++) {
            NodeNum candidate = candidates.nodes[ci];
            if (hasNodeTransmitted(candidate, packetId, currentTime)) {
                const NodeEdges *candidateEdges = findNeighbor(candidate);
                if (candidateEdges) {
                    for (uint8_t i = 0; i < candidateEdges->edgeCount; i++) {
                        relayCoverage.insert(candidateEdges->edges[i].to);
                    }
                }
            }
        }

        const NodeEdges *myEdges = findNeighbor(myNode);
        if (myEdges) {
            bool haveUniqueCoverage = false;
            for (uint8_t i = 0; i < myEdges->edgeCount; i++) {
                NodeNum neighbor = myEdges->edges[i].to;
                if (!alreadyCovered.contains(neighbor) &&
                    !relayCoverage.contains(neighbor)) {
                    LOG_INFO("[SR] Unique coverage in relay decision: neighbor %08x uncovered", neighbor);
                    haveUniqueCoverage = true;
                    break;
                }
            }

            if (haveUniqueCoverage) {
                return true;
            }
        }

        return false;
    }

    // No relay candidates had unique coverage — check if WE have unique coverage
    const NodeEdges *myEdges = findNeighbor(myNode);
    if (myEdges) {
        for (uint8_t i = 0; i < myEdges->edgeCount; i++) {
            if (!alreadyCovered.contains(myEdges->edges[i].to)) {
                LOG_INFO("[SR] Unique coverage fallthrough: neighbor %08x uncovered", myEdges->edges[i].to);
                return true;
            }
        }
    }

    return false;
}

bool NeighborGraph::shouldRelayEnhancedConservative(NodeNum myNode, NodeNum sourceNode, NodeNum heardFrom,
                                                     uint32_t currentTime, uint32_t packetId,
                                                     uint32_t packetRxTime,
                                                     const NodeNum *coListeners, uint8_t coListenerCount) const
{
    const NodeEdges *myEdges = findNeighbor(myNode);
    if (!myEdges)
        return false;

    bool hasStockGateways = false;
    for (uint8_t i = 0; i < myEdges->edgeCount; i++) {
        const NodeEdges *neighborEdges = findNeighbor(myEdges->edges[i].to);
        if (neighborEdges && neighborEdges->edgeCount >= 8) {
            hasStockGateways = true;
            break;
        }
    }

    if (hasStockGateways) {
        return shouldRelaySimpleConservative(myNode, sourceNode, heardFrom, currentTime);
    }

    return shouldRelayEnhanced(myNode, sourceNode, heardFrom, currentTime, packetId, packetRxTime,
                               coListeners, coListenerCount);
}

bool NeighborGraph::shouldRelaySimple(NodeNum myNode, NodeNum sourceNode, NodeNum heardFrom, uint32_t currentTime) const
{
    const NodeEdges *myEdges = findNeighbor(myNode);
    const NodeEdges *transmittingEdges = findNeighbor(heardFrom);

    if (!myEdges || myEdges->edgeCount == 0) {
        return false;
    }

    if (!transmittingEdges) {
        return false;
    }

    NodeSet alreadyCovered;
    alreadyCovered.insert(sourceNode);
    alreadyCovered.insert(heardFrom);

    for (uint8_t i = 0; i < transmittingEdges->edgeCount; i++) {
        alreadyCovered.insert(transmittingEdges->edges[i].to);
    }

    uint8_t uniqueNeighbors = 0;
    for (uint8_t i = 0; i < myEdges->edgeCount; i++) {
        NodeNum neighbor = myEdges->edges[i].to;
        if (!alreadyCovered.contains(neighbor)) {
            uniqueNeighbors++;
        }
    }

    return uniqueNeighbors > 0;
}

bool NeighborGraph::shouldRelaySimpleConservative(NodeNum myNode, NodeNum sourceNode, NodeNum heardFrom,
                                                   uint32_t currentTime) const
{
    const NodeEdges *myEdges = findNeighbor(myNode);
    const NodeEdges *transmittingEdges = findNeighbor(heardFrom);

    if (!myEdges || myEdges->edgeCount == 0) {
        return false;
    }

    if (!transmittingEdges) {
        LOG_INFO("NeighborGraph: No topology for TX node %08x - fallback relay", heardFrom);
        return true;
    }

    uint8_t uniqueSrNeighbors = 0;
    uint8_t totalNeighborsNotCovered = 0;
    static constexpr uint8_t MAX_LOG_NODES = 10;
    NodeNum uncoveredNodes[MAX_LOG_NODES];
    for (uint8_t i = 0; i < myEdges->edgeCount; i++) {
        NodeNum neighbor = myEdges->edges[i].to;
        if (neighbor == sourceNode || neighbor == heardFrom) {
            continue;
        }

        bool transmittingHasIt = false;
        for (uint8_t j = 0; j < transmittingEdges->edgeCount; j++) {
            if (transmittingEdges->edges[j].to == neighbor) {
                transmittingHasIt = true;
                break;
            }
        }

        if (!transmittingHasIt) {
            if (totalNeighborsNotCovered < MAX_LOG_NODES) {
                uncoveredNodes[totalNeighborsNotCovered] = neighbor;
            }
            uniqueSrNeighbors++;
            totalNeighborsNotCovered++;
        }
    }

    if (uniqueSrNeighbors >= 2) {
        return true;
    }

    if (totalNeighborsNotCovered > 0) {
#ifndef DEBUG_MUTE
        char nodeList[128] = "";
        int offset = 0;
        uint8_t logCount = totalNeighborsNotCovered < MAX_LOG_NODES ? totalNeighborsNotCovered : MAX_LOG_NODES;
        for (uint8_t i = 0; i < logCount; i++) {
            offset += snprintf(nodeList + offset, sizeof(nodeList) - offset, "%s%08x", i > 0 ? ", " : "", uncoveredNodes[i]);
            if (offset >= (int)sizeof(nodeList) - 1)
                break;
        }
        LOG_INFO("NeighborGraph: fallback, %u uncovered neighbors [%s], relaying", totalNeighborsNotCovered, nodeList);
#endif
        return true;
    }

    return false;
}

bool NeighborGraph::shouldRelayWithContention(NodeNum myNode, NodeNum sourceNode, NodeNum heardFrom, uint32_t packetId,
                                               uint32_t currentTime) const
{
    const NodeEdges *myEdges = findNeighbor(myNode);
    const NodeEdges *sourceEdges = findNeighbor(sourceNode);
    const NodeEdges *relayEdges = (heardFrom == sourceNode) ? nullptr : findNeighbor(heardFrom);

    if (!myEdges || myEdges->edgeCount == 0) {
        return false;
    }

    uint8_t uniqueNeighbors = 0;
    for (uint8_t i = 0; i < myEdges->edgeCount; i++) {
        NodeNum neighbor = myEdges->edges[i].to;
        if (neighbor == sourceNode || neighbor == heardFrom) {
            continue;
        }

        bool sourceHasIt = false;
        if (sourceEdges) {
            for (uint8_t j = 0; j < sourceEdges->edgeCount; j++) {
                if (sourceEdges->edges[j].to == neighbor) {
                    sourceHasIt = true;
                    break;
                }
            }
        }

        if (relayEdges && !sourceHasIt) {
            for (uint8_t j = 0; j < relayEdges->edgeCount; j++) {
                if (relayEdges->edges[j].to == neighbor) {
                    sourceHasIt = true;
                    break;
                }
            }
        }

        if (!sourceHasIt) {
            uniqueNeighbors++;
        }
    }

    if (uniqueNeighbors == 0) {
        return false;
    }

    for (uint8_t i = 0; i < neighborCount; i++) {
        NodeNum otherNode = neighbors[i].nodeId;
        if (otherNode != myNode && otherNode != sourceNode && otherNode != heardFrom) {
            if (hasNodeTransmitted(otherNode, packetId, currentTime)) {
                return false;
            }
        }
    }

    return true;
}

void NeighborGraph::recordNodeTransmission(NodeNum nodeId, uint32_t packetId, uint32_t currentTime)
{
    // Keyed on the pair: a node transmits many packets, and "did X put *this* packet on the
    // air" is what the coverage tests ask. Keying on the node alone kept one packet per node,
    // so an answer about an older packet silently became "no".
    for (uint8_t i = 0; i < relayStateCount; i++) {
        if (relayStates[i].nodeId == nodeId && relayStates[i].packetId == packetId) {
            relayStates[i].timestampLo = static_cast<uint16_t>(currentTime & 0xFFFF);
            return;
        }
    }

    if (relayStateCount < NEIGHBOR_GRAPH_MAX_RELAY_STATES) {
        relayStates[relayStateCount].nodeId = nodeId;
        relayStates[relayStateCount].packetId = packetId;
        relayStates[relayStateCount].timestampLo = static_cast<uint16_t>(currentTime & 0xFFFF);
        relayStateCount++;
    } else {
        uint8_t oldestIdx = 0;
        uint16_t oldestTimestamp = relayStates[0].timestampLo;
        uint16_t currentLo = static_cast<uint16_t>(currentTime & 0xFFFF);

        for (uint8_t i = 1; i < NEIGHBOR_GRAPH_MAX_RELAY_STATES; i++) {
            uint16_t age = currentLo - relayStates[i].timestampLo;
            uint16_t oldestAge = currentLo - oldestTimestamp;
            if (age > oldestAge) {
                oldestIdx = i;
                oldestTimestamp = relayStates[i].timestampLo;
            }
        }

        relayStates[oldestIdx].nodeId = nodeId;
        relayStates[oldestIdx].packetId = packetId;
        relayStates[oldestIdx].timestampLo = static_cast<uint16_t>(currentTime & 0xFFFF);
    }
}

bool NeighborGraph::hasNodeTransmitted(NodeNum nodeId, uint32_t packetId, uint32_t currentTime) const
{
    uint16_t currentLo = static_cast<uint16_t>(currentTime & 0xFFFF);

    for (uint8_t i = 0; i < relayStateCount; i++) {
        if (relayStates[i].nodeId == nodeId && relayStates[i].packetId == packetId) {
            uint16_t age = currentLo - relayStates[i].timestampLo;
            return age <= (getContentionWindowMs() / 1000 + 1);
        }
    }
    return false;
}

#endif // !MESHTASTIC_EXCLUDE_SIGNALROUTING
