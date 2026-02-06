# HAL Architecture

HAL (Hybrid Adaptive Learning Scheduler) is not a monolithic scheduler, but rather a **composable platform** built from multiple specialized components that work together to provide comprehensive workload orchestration for HPC and AI environments.

## Component Architecture

```
┌─────────────────────────────────────────────────────────────────────────────────────┐
│                                   HAL Platform                                       │
├─────────────────────────────────────────────────────────────────────────────────────┤
│                                                                                      │
│   ┌─────────────────────────────────────────────────────────────────────────────┐   │
│   │                         Task Management Layer                                │   │
│   │  ┌─────────────────┐  ┌─────────────────┐  ┌─────────────────────────────┐  │   │
│   │  │  Job Submission │  │ Task Scheduling │  │  ML-Powered Optimization    │  │   │
│   │  │    & Queuing    │  │    Engine       │  │  (Runtime Prediction, etc)  │  │   │
│   │  └─────────────────┘  └─────────────────┘  └─────────────────────────────┘  │   │
│   └─────────────────────────────────────────────────────────────────────────────┘   │
│                                        │                                             │
│   ┌────────────────────────────────────┼────────────────────────────────────────┐   │
│   │                        Resource Management Layer                             │   │
│   │  ┌─────────────────┐  ┌────────────┴────────────┐  ┌─────────────────────┐  │   │
│   │  │  Resource Pool  │  │  Topology-Aware         │  │  Reservation &      │  │   │
│   │  │   Abstraction   │  │  Placement Engine       │  │  Quota Management   │  │   │
│   │  └─────────────────┘  └─────────────────────────┘  └─────────────────────┘  │   │
│   └─────────────────────────────────────────────────────────────────────────────┘   │
│                                        │                                             │
│   ┌────────────────────────────────────┼────────────────────────────────────────┐   │
│   │                                    ▼                                         │   │
│   │  ┌─────────────────┐  ┌─────────────────┐  ┌─────────────────┐              │   │
│   │  │   Data Plane    │  │ Compute Control │  │  Observability  │              │   │
│   │  │                 │  │     Plane       │  │      Plane      │              │   │
│   │  │  ┌───────────┐  │  │  ┌───────────┐  │  │  ┌───────────┐  │              │   │
│   │  │  │  Caching  │  │  │  │   Cloud   │  │  │  │  Metrics  │  │              │   │
│   │  │  │  Layer    │  │  │  │  Access   │  │  │  │           │  │              │   │
│   │  │  ├───────────┤  │  │  ├───────────┤  │  │  ├───────────┤  │              │   │
│   │  │  │   Data    │  │  │  │  On-Prem  │  │  │  │  Logging  │  │              │   │
│   │  │  │  Movement │  │  │  │   Infra   │  │  │  │           │  │              │   │
│   │  │  ├───────────┤  │  │  ├───────────┤  │  │  ├───────────┤  │              │   │
│   │  │  │  Affinity │  │  │  │  Neo Cloud│  │  │  │  Tracing  │  │              │   │
│   │  │  │  Tracking │  │  │  │  (GPU/AI) │  │  │  │           │  │              │   │
│   │  │  └───────────┘  │  │  └───────────┘  │  │  └───────────┘  │              │   │
│   │  └─────────────────┘  └─────────────────┘  └─────────────────┘              │   │
│   │                                                                              │   │
│   │  ┌─────────────────────────────────────────────────────────────────────┐    │   │
│   │  │                    Binary Management Layer                           │    │   │
│   │  │  ┌───────────────────┐  ┌─────────────────┐  ┌───────────────────┐  │    │   │
│   │  │  │ Package Registry  │  │   Deployment    │  │  Version Control  │  │    │   │
│   │  │  │  & Distribution   │  │     Agent       │  │   & Rollback      │  │    │   │
│   │  │  └───────────────────┘  └─────────────────┘  └───────────────────┘  │    │   │
│   │  └─────────────────────────────────────────────────────────────────────┘    │   │
│   │                            Infrastructure Layer                              │   │
│   └─────────────────────────────────────────────────────────────────────────────┘   │
│                                                                                      │
└──────────────────────────────────────────────────────────────────────────────────────┘
                                         │
                                         ▼
┌─────────────────────────────────────────────────────────────────────────────────────┐
│                              Compute Resources                                       │
├───────────────────────┬─────────────────────────────┬───────────────────────────────┤
│      On-Premises      │      Traditional Cloud      │          Neo Clouds           │
│                       │                             │                               │
│  ┌─────────────────┐  │  ┌───────────────────────┐  │  ┌─────────────────────────┐  │
│  │  HPC Clusters   │  │  │  AWS / Azure / GCP    │  │  │  CoreWeave / Lambda     │  │
│  │  Bare Metal     │  │  │  Standard VMs         │  │  │  RunPod / Nebius        │  │
│  │  Private Cloud  │  │  │  Spot Instances       │  │  │  GPU-Specialized        │  │
│  └─────────────────┘  │  └───────────────────────┘  │  └─────────────────────────┘  │
└───────────────────────┴─────────────────────────────┴───────────────────────────────┘
```

## Core Components

### 1. Observability Plane

The Observability Plane provides comprehensive visibility into the entire HAL ecosystem:

| Capability | Description |
|------------|-------------|
| **Metrics Collection** | Gathers performance metrics from all components—compute nodes, cache instances, schedulers |
| **Distributed Logging** | Centralized logging with correlation across distributed job executions |
| **Distributed Tracing** | End-to-end tracing of job lifecycles from submission to completion |
| **Alerting** | Proactive alerting on anomalies, failures, and threshold breaches |
| **Dashboards** | Real-time visualization of cluster health, utilization, and performance |

### 2. Compute Control Plane

The Compute Control Plane provides unified access to heterogeneous compute resources:

| Provider Type | Examples | Capabilities |
|---------------|----------|--------------|
| **Traditional Clouds** | AWS, Azure, GCP | Auto-scaling, spot instance management, cross-region deployment |
| **Neo Clouds** | CoreWeave, Lambda Labs, RunPod, Nebius | GPU-optimized provisioning, AI/ML workload acceleration |
| **On-Premises** | HPC clusters, bare metal, private cloud | Direct integration, topology awareness, RDMA/InfiniBand support |

Key functions:
- **Unified Resource Abstraction**: Present all compute as a single pool regardless of source
- **Cost Optimization**: Intelligent placement based on cost, performance, and availability
- **Auto-Scaling**: Dynamic provisioning and deprovisioning based on workload demand
- **Multi-Cloud Orchestration**: Seamless workload distribution across providers

### 3. Resource Management

The Resource Management layer handles the allocation and tracking of compute resources:

| Function | Description |
|----------|-------------|
| **Resource Pooling** | Logical grouping of resources with specific characteristics (GPU type, memory, network) |
| **Quota Management** | Fair-share allocation across users, projects, and organizations |
| **Reservations** | Advanced booking of resources for scheduled workloads |
| **Topology Mapping** | Understanding physical/network topology for optimal placement |
| **Affinity Rules** | Co-location and anti-affinity constraints for jobs |

### 4. Data Plane (Caching, Movement & Affinity)

The Data Plane ensures data is where it needs to be for optimal job performance:

| Capability | Description |
|------------|-------------|
| **Hierarchical Caching** | Multi-tier cache topology from node-local to global |
| **Data Movement** | Intelligent pre-staging and migration of data to compute locations |
| **Affinity Tracking** | Knows where data resides and schedules jobs accordingly |
| **Pull-Through Caching** | Automatic propagation of data through the cache hierarchy |
| **Content Inventory** | Real-time knowledge of what data exists where |

### 5. Binary Management Layer

The Binary Management layer handles software deployment across the compute fleet:

| Function | Description |
|----------|-------------|
| **Package Registry** | Centralized storage of application binaries, containers, and environments |
| **Deployment Agent** | Lightweight agent on each node for receiving and installing software |
| **Version Control** | Track deployed versions, support for pinned versions per job |
| **Rollback** | Quick rollback to previous versions on failure |
| **Environment Management** | Conda, virtual environments, container runtimes |

### 6. Task Management

The Task Management layer is the core scheduling intelligence:

| Function | Description |
|----------|-------------|
| **Job Submission** | API and CLI for submitting jobs with resource requirements |
| **Queue Management** | Multiple queues with priority, fair-share, and preemption policies |
| **Scheduling Engine** | Core algorithm for matching jobs to resources |
| **ML Optimization** | Machine learning for runtime prediction, workload compaction, and anomaly detection |
| **Dependency Resolution** | DAG-based workflows with complex dependencies |

---

## Data Plane Deep Dive: The Cache Directory

The `cache/` directory in this repository contains the specification and documentation for HAL's Data Plane—specifically the distributed caching layer that enables data-aware scheduling.

### What the Cache Provides

```
cache/
├── openapi.yaml      # Cache Control Plane API specification
├── README.md         # Cache architecture and design documentation
└── prompt-history.md # Development history
```

#### Hierarchical Cache Architecture

The cache forms a **tree topology** that mirrors the physical infrastructure:

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
              ▼                      ▼                      ▼
         Local Caches           Local Caches           Local Caches
              │                      │                      │
              ▼                      ▼                      ▼
         Compute Nodes          Compute Nodes          Compute Nodes
```

#### Cache Control Plane API (openapi.yaml)

The [openapi.yaml](cache/openapi.yaml) defines the control plane API that enables:

| API Category | Purpose |
|--------------|---------|
| **Instance Management** | Registration, heartbeats, and health monitoring of cache instances |
| **Content Map** | Global view of which keys exist in which cache instances |
| **Topology Services** | Routing information and proximity relationships between caches |
| **Event Streaming** | Real-time notifications of cache changes (adds, evictions) |

#### How the Cache Enables Data-Aware Scheduling

1. **Content Visibility**: The control plane maintains a global map of what data exists in each cache instance
2. **Locality Information**: The scheduler knows where data is cached before making placement decisions
3. **Affinity Scoring**: Jobs are scored higher for nodes where their required data already exists locally
4. **Predictive Staging**: Based on job queue analysis, data can be pre-staged to expected execution locations
5. **Cost Optimization**: Data transfer costs (time, bandwidth, egress fees) factor into scheduling decisions

### Integration with Other Components

```
┌────────────────────────────────────────────────────────────────────────────────────┐
│                              HAL Scheduler Core                                     │
│                                                                                     │
│  ┌───────────────────────────────────────────────────────────────────────────────┐ │
│  │                           Scheduling Decision                                  │ │
│  │                                                                                │ │
│  │   Job Requirements ──┬──► Resource Availability                                │ │
│  │                      ├──► Data Locality (from Cache Control Plane) ◄───────┐  │ │
│  │                      ├──► Topology Constraints                              │  │ │
│  │                      ├──► Cost Optimization                                 │  │ │
│  │                      └──► ML Predictions                                    │  │ │
│  └───────────────────────────────────────────────────────────────────────────────┘ │
│                                       │                                             │
└───────────────────────────────────────┼─────────────────────────────────────────────┘
                                        │
              ┌─────────────────────────┼─────────────────────────┐
              │                         │                         │
              ▼                         ▼                         ▼
    ┌─────────────────┐       ┌─────────────────┐       ┌─────────────────┐
    │  Cache Control  │       │    Compute      │       │  Observability  │
    │     Plane       │       │ Control Plane   │       │     Plane       │
    │                 │       │                 │       │                 │
    │ • Content Map   │       │ • Provisioning  │       │ • Metrics       │
    │ • Topology      │       │ • Scaling       │       │ • Logs          │
    │ • Routing       │       │ • Health        │       │ • Traces        │
    └────────┬────────┘       └────────┬────────┘       └────────┬────────┘
             │                         │                         │
             ▼                         ▼                         ▼
    ┌─────────────────────────────────────────────────────────────────────┐
    │                    Distributed Cache Instances                       │
    │                    Compute Nodes (HPC/Cloud/Hybrid)                  │
    └─────────────────────────────────────────────────────────────────────┘
```

---

## Design Principles

### Modularity Through Interfaces

Every component in HAL is accessed through well-defined interfaces:

```
┌──────────────────────────────────────────────────────────────────┐
│                      Component Interface                          │
├──────────────────────────────────────────────────────────────────┤
│  • OpenAPI/gRPC specifications for all APIs                      │
│  • Pluggable implementations (swap any component)                │
│  • Contract testing ensures compatibility                         │
│  • Version negotiation for backward compatibility                │
└──────────────────────────────────────────────────────────────────┘
```

This allows:
- Replacing any component with an alternative implementation
- Testing components in isolation
- Gradual migration between implementations
- Third-party integrations

### Eventual Consistency with Strong Coordination

HAL embraces eventual consistency for scalability while providing strong coordination where needed:

| Pattern | Use Case |
|---------|----------|
| **Eventual Consistency** | Cache content maps, metrics aggregation, resource discovery |
| **Strong Consistency** | Job state transitions, resource reservations, quota enforcement |

### Failure Isolation

Each component is designed to fail independently:

- Cache instance failure → Jobs continue with degraded data locality
- Observability outage → Scheduling continues without metrics
- Single cloud failure → Workloads shift to available providers

---

## Summary

HAL is a **platform of specialized components**, not a singular scheduler:

| Component | Responsibility |
|-----------|----------------|
| **Observability Plane** | Visibility and monitoring across the entire system |
| **Compute Control Plane** | Unified access to clouds, neo clouds, and on-premises infrastructure |
| **Resource Management** | Allocation, quotas, topology, and affinity |
| **Data Plane** | Caching, data movement, and locality tracking |
| **Binary Management** | Software deployment and versioning on compute nodes |
| **Task Management** | Core scheduling, queuing, and ML optimization |

The `cache/` directory provides the foundational Data Plane specification—enabling the data-aware scheduling that differentiates HAL from traditional workload schedulers.
