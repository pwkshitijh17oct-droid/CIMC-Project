# High-Performance Concurrent In-Memory Cache Engine

A custom-built, multi-threaded, sharded in-memory key-value cache engine written in C++17. Designed to handle intense concurrent workloads using Win32 network threading sockets, data durability via Write-Ahead Logging (WAL), and optimized memory-efficient eviction using a Redis-style Approximate LRU algorithm.

Includes a custom high-concurrency automated benchmark suite to track performance under stress.

## 🚀 Key Architectural Features

       ┌────────────────────────────────────────────────────────┐
       │             TIER 1: CLIENT / BENCHMARK SUITE           │
       │  (10+ Parallel Worker Threads firing GET/SET Streams)  │
       └───────────────────────────┬────────────────────────────┘
                                   │ (TCP Traffic / Winsock Sockets)
                                   ▼
       ┌────────────────────────────────────────────────────────┐
       │             TIER 2: WIN32 SOCKET NETWORK SERVER        │
       │  [TCP Listener] ──► [Thread Dispatcher]                │
       │                           │                            │
       │      ┌────────────────────┼────────────────────┐       │
       │      ▼                    ▼                    ▼       │
       │  [Worker Thread]      [Worker Thread]   [Worker Thread]|
       └──────┬────────────────────┬────────────────────┬───────┘
              │                    │                    │
              ▼                    ▼                    ▼
       ┌────────────────────────────────────────────────────────┐
       │             TIER 3: SHARDED STORAGE ENGINE             │
       │        (Segmented Locking prevents global blocking)    │
       │                                                        │
       │  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  │
       │  │   SHARD 0    │  │   SHARD 1    │  │   SHARD 2    │  │
       │  │ [LOCK_0]     │  │ [LOCK_1]     │  │ [LOCK_2]     │  │
       │  │ unordered_map│  │ unordered_map│  │ unordered_map│  │
       │  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘  │
       │         │                 │                 │          │
       │         │   ┌─────────────┴─────────────┐   │          │
       │         ▼   ▼                           ▼   ▼          │
       │    [Approx LRU Sampler]        [Passive TTL Sweeper]   │
       └─────────┬───────────────────────────────────┬──────────┘
                 │                                   ▲
                 │ (Append-Only Mutex Write)         │ (Replay Log on Boot)
                 ▼                                   │
       ┌─────────────────────────────────────────────┴──────────┐
       │             TIER 4: DURABILITY LAYER (WAL)             │
       │  [Sequential Disk I/O] ──► cache.log (Crash Recovery)  │
       └────────────────────────────────────────────────────────┘

### 1. Concurrency & Segmented Locking (Sharding)
To maximize throughput across multi-core processors, the storage space is split into multiple independent memory sectors called **Shards**. Instead of a single global lock over the entire database, each shard contains its own localized `CRITICAL_SECTION`. 
* **Impact:** Parallel client worker threads can read/write to different shards at the exact same time without blocking each other, drastically minimizing lock contention.

### 2. Redis-Style Approximate LRU Eviction
The storage engine abandons the resource-heavy textbook Perfect LRU layout (which utilizes a doubly linked list + hash map setup) to reduce memory overhead and pointer updates. 
* **Mechanism:** Every node stores a flat integer timestamp tracking its `last_accessed` state. When a shard crosses its maximum allocation limit, it samples a random subset ($N=5$) of keys and immediately evicts the oldest candidate.
* **Impact:** Eliminates pointer re-linking operations (`.splice()`) on every single query, allowing threads to execute operations rapidly and exit the lock phase.

### 3. Crash-Resilient Write-Ahead Logging (WAL)
To prevent total data loss upon server reboots, the cache implements an automated append-only `cache.log` disk tracker. Before operations alter the memory map, they are flushed securely onto the disk. On system boot, the recovery manager automatically reconstructs the memory state up to the point of failure.

### 4. Custom Automated Benchmark Suite
Includes a high-precision `benchmark.cpp` harness capable of spawning configurable, parallel client workers making thousands of concurrent TCP network requests to establish real-world analytics.

---

## 📊 Performance Benchmarks & Evolution

The system was evaluated under an intense multi-threaded testing workload simulating a heavy production environment (50% Read / 50% Write ratio via 10 parallel background thread workers):

| Eviction Strategy Implementation | Throughput (Ops/Sec) | Avg Latency Per Request | Memory Allocation Node Overhead |
| :--- | :--- | :--- | :--- |
| **Traditional Perfect LRU** *(Doubly Linked List + Map)* | ~965 ops/sec | 1.035 ms | High (Contains 16-bytes pointer metadata) |
| **Redis-Style Approximate LRU** *(Random Sampler)* | **2,017 ops/sec** | **0.495 ms** | **Low (0 pointer tracking bytes)** |

**Key Takeaway:** Swapping from structural pointer operations to localized timestamp updating **more than doubled concurrent system throughput (+109% increase)** and dropped round-trip latency below half a millisecond.

---

## 🛠️ Compilation & Getting Started

### Prerequisites
* Windows OS
* G++ compiler supporting C++17 or higher
* Winsock2 support system paths (`ws2_32`)

### 1. Build and Run the Server
Compile the main cache engine architecture:
```bash
g++ -std=c++17 cache.cpp -o cache_engine -lws2_32

Fire up the background caching engine listening live on port 8888:
Bash
.\cache_engine.exe

2. Launch the High-Performance Benchmark Suite
Open a separate terminal window and compile the benchmarking script:

Bash
g++ -std=c++17 benchmark.cpp -o cache_benchmark -lws2_32

Run the stress suite by passing the number of parallel thread clients you wish to simulate (e.g., 10 concurrent connections):

Bash
.\cache_benchmark.exe 10

---

🔧 Core Code Layout Summary

cache.cpp: Houses the primary ShardedCache class, data schema parsing rules, network worker loops, and active/passive TTL expiration routines.

benchmark.cpp: Multi-threaded client network driver that generates high-resolution operational performance metrics using standard Windows threading API structures.
