# HAL - Hybrid Adaptive Learning Scheduler

An AI-first approach to building a next-generation HPC and AI workload scheduler.

## Overview

HAL is a modern scheduler designed to combine the best aspects of existing scheduling solutions:

- **High Throughput Computing (HTC)** capabilities inspired by IBM Symphony for maximum throughput
- **Topological workload placement** inspired by Slurm for intelligent resource allocation

## Key Features

### Intelligent Scheduling
- **Data-aware scheduling** - Schedule workloads based on data locality to minimize data movement
- **Topology-aware placement** - Understands the topological and geographical placement of nodes for smarter scheduling decisions
- **ML-powered scheduling** - Uses machine learning to optimize workload placement and predict job runtimes for better hardware utilization and compaction

### Cloud-Native Architecture
- **Cloud-enabled from day one** - Built with cloud infrastructure as a first-class citizen
- **Hybrid deployment** - Supports on-premises, cloud, and hybrid environments

### Modular Design

HAL is built with a modular architecture where each component is accessed through well-defined interfaces, allowing any part to be replaced with an alternative implementation:

| Component | Description |
|-----------|-------------|
| **Data Plane** | Manages data movement and locality |
| **Observability Plane** | Provides monitoring, logging, and metrics |
| **Cloud Control Plane** | Handles cloud resource provisioning and management |
| **Scheduling Engine** | Core scheduling logic and ML-based optimization |

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                      HAL Scheduler                          │
├─────────────┬─────────────┬─────────────┬─────────────────┤
│  Scheduling │    Data     │ Observability│  Cloud Control  │
│   Engine    │   Plane     │    Plane     │     Plane       │
├─────────────┴─────────────┴─────────────┴─────────────────┤
│                  Interface Layer                            │
├─────────────────────────────────────────────────────────────┤
│              Compute Resources (HPC / Cloud / Hybrid)       │
└─────────────────────────────────────────────────────────────┘
```

## Goals

- Maximize hardware utilization through intelligent scheduling
- Minimize job wait times with accurate runtime predictions
- Reduce data movement overhead with data-aware placement
- Provide seamless scaling across on-premises and cloud resources
- Enable easy customization through pluggable components

## License

See [LICENSE](LICENSE) for details.