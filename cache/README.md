# HAL Cache Layer

The distributed caching layer for the HAL HPC Scheduler's Data Plane.

## Overview

The HAL Cache provides a high-performance, hierarchical caching system designed specifically for HPC and AI workloads. It enables data-aware scheduling by ensuring that compute nodes have rapid access to the data they need, minimizing data movement overhead across the cluster.

Built as a fork of [ValKey](https://github.com/valkey-io/valkey) (the open-source Redis successor), HAL Cache extends the standard key-value store with additional capabilities for distributed cache coordination and telemetry.

## Key Features

### Redis-Compatible API
Users interact with the cache using the standard Redis API, ensuring compatibility with existing tools, libraries, and workflows. Any Redis client can connect to and use a HAL Cache instance without modification.

### Hierarchical Cache Topology
The cache instances form a **tree topology** where:

- The **root** of the tree contains the complete dataset (source of truth)
- **Branch nodes** are intermediate caches that serve multiple downstream caches
- **Leaf nodes** are the most local caches, closest to compute resources

```
                          ┌─────────────────────┐
                          │   Root Cache        │
                          │  (Complete Dataset) │
                          └──────────┬──────────┘
                                     │
              ┌──────────────────────┼──────────────────────┐
              │                      │                      │
              ▼                      ▼                      ▼
     ┌─────────────────┐    ┌─────────────────┐    ┌─────────────────┐
     │ Regional Cache  │    │ Regional Cache  │    │ Regional Cache  │
     │   (Site A)      │    │   (Site B)      │    │   (Cloud)       │
     └────────┬────────┘    └────────┬────────┘    └────────┬────────┘
              │                      │                      │
       ┌──────┴──────┐        ┌──────┴──────┐        ┌──────┴──────┐
       │             │        │             │        │             │
       ▼             ▼        ▼             ▼        ▼             ▼
  ┌─────────┐   ┌─────────┐ ┌─────────┐ ┌─────────┐ ┌─────────┐ ┌─────────┐
  │ Rack 1  │   │ Rack 2  │ │ Rack 1  │ │ Rack 2  │ │ Zone A  │ │ Zone B  │
  │ Cache   │   │ Cache   │ │ Cache   │ │ Cache   │ │ Cache   │ │ Cache   │
  └────┬────┘   └────┬────┘ └────┬────┘ └────┬────┘ └────┬────┘ └────┬────┘
       │             │           │           │           │           │
       ▼             ▼           ▼           ▼           ▼           ▼
   ┌───────┐     ┌───────┐   ┌───────┐   ┌───────┐   ┌───────┐   ┌───────┐
   │Compute│     │Compute│   │Compute│   │Compute│   │Compute│   │Compute│
   │ Nodes │     │ Nodes │   │ Nodes │   │ Nodes │   │ Nodes │   │ Nodes │
   └───────┘     └───────┘   └───────┘   └───────┘   └───────┘   └───────┘
```

### Locality-First Data Access

Clients always interact with their **most local cache instance**. This ensures:

- Minimal network latency for cache hits
- Reduced network congestion
- Optimal performance for frequently accessed data

### Intelligent Data Retrieval

When a local cache does not contain the requested data, it automatically:

1. **Locates** the most proximate cache instance that holds the data (optimized by network latency, bandwidth, and cost)
2. **Retrieves** the data from that source
3. **Stores** a local copy for future requests
4. **Returns** the data to the client

This pull-through caching strategy ensures data naturally propagates to where it is needed.

## Cache Control Plane Integration

### Content Inventory API

Each cache instance exposes an additional API that provides:

- A complete list of all keys held in the cache
- The size (in bytes) of the data associated with each key

This allows the control plane and other system components to understand exactly what data is available at each cache location.

### Event Notifications

Every time a cache instance's contents change, it sends a notification to the **Cache Control Plane** containing:

| Field | Description |
|-------|-------------|
| `keys_added` | List of keys that were added to the cache |
| `keys_evicted` | List of keys that were evicted from the cache |
| `key_sizes` | Size of each key's data in bytes |
| `source_instance` | The cache instance from which the data was sourced |
| `retrieval_time_ms` | Time taken to retrieve the data (in milliseconds) |
| `timestamp` | When the change occurred |

### Control Plane Capabilities

The Cache Control Plane maintains a complete view of the distributed cache system:

- **Global Content Map**: Knows exactly which data is available in each cache instance
- **Topology Awareness**: Understands the proximity relationships between all cache instances (latency, bandwidth, cost)
- **Optimal Routing**: Can direct cache instances to the best source for any given piece of data
- **Analytics**: Tracks data access patterns, cache hit rates, and retrieval performance

```
┌─────────────────────────────────────────────────────────────────────────┐
│                         Cache Control Plane                              │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│  ┌──────────────────┐  ┌──────────────────┐  ┌──────────────────┐       │
│  │  Global Content  │  │    Topology      │  │    Analytics     │       │
│  │       Map        │  │     Graph        │  │     Engine       │       │
│  └────────┬─────────┘  └────────┬─────────┘  └────────┬─────────┘       │
│           │                     │                     │                  │
│           └─────────────────────┼─────────────────────┘                  │
│                                 │                                        │
│                    ┌────────────┴────────────┐                           │
│                    │    Decision Engine      │                           │
│                    │  (Routing & Placement)  │                           │
│                    └────────────┬────────────┘                           │
│                                 │                                        │
└─────────────────────────────────┼────────────────────────────────────────┘
                                  │
                                  ▼
            ┌─────────────────────────────────────────┐
            │         Event Stream (Pub/Sub)          │
            └──────┬──────────────┬──────────────┬────┘
                   │              │              │
                   ▼              ▼              ▼
              ┌─────────┐    ┌─────────┐    ┌─────────┐
              │ Cache   │    │ Cache   │    │ Cache   │
              │Instance │    │Instance │    │Instance │
              │    A    │    │    B    │    │    C    │
              └─────────┘    └─────────┘    └─────────┘
```

## Implementation

### ValKey Fork

The HAL Cache is based on a fork of ValKey with the following modifications:

1. **Key Change Hooks**: Instrumentation added to capture all key additions, modifications, and evictions
2. **Control Plane Client**: Built-in client for sending events to the Cache Control Plane
3. **Content Inventory Endpoint**: Additional API endpoint for querying cache contents and sizes
4. **Hierarchy Awareness**: Logic for locating and retrieving data from parent/sibling cache instances
5. **Proximity-Based Routing**: Integration with the control plane to determine optimal data sources

### Data Flow Example

```
┌──────────┐         ┌─────────────┐         ┌─────────────┐         ┌──────────┐
│  Client  │         │ Local Cache │         │Regional Cache│        │Root Cache│
│          │         │  (Leaf)     │         │  (Branch)    │        │  (Root)  │
└────┬─────┘         └──────┬──────┘         └──────┬───────┘        └────┬─────┘
     │                      │                       │                     │
     │  GET key:dataset_1   │                       │                     │
     │─────────────────────►│                       │                     │
     │                      │                       │                     │
     │                      │  Cache Miss           │                     │
     │                      │  Query Control Plane  │                     │
     │                      │  for nearest source   │                     │
     │                      │──────────────────────►│                     │
     │                      │                       │                     │
     │                      │  Regional has data    │                     │
     │                      │◄──────────────────────│                     │
     │                      │                       │                     │
     │                      │  GET key:dataset_1    │                     │
     │                      │──────────────────────►│                     │
     │                      │                       │                     │
     │                      │       Data            │                     │
     │                      │◄──────────────────────│                     │
     │                      │                       │                     │
     │                      │  Store locally        │                     │
     │                      │  Notify Control Plane │                     │
     │                      │                       │                     │
     │        Data          │                       │                     │
     │◄─────────────────────│                       │                     │
     │                      │                       │                     │
```

## Integration with HAL Scheduler

The Cache Layer is a core component of the HAL Data Plane, enabling:

- **Data-Aware Scheduling**: The scheduler can query the control plane to understand data locality and make placement decisions that minimize data movement
- **Predictive Caching**: Based on job submissions and historical patterns, data can be pre-staged to appropriate cache locations
- **Cost Optimization**: In hybrid environments, the cache layer helps minimize expensive cross-network data transfers

## License

See [LICENSE](../LICENSE) for details.
