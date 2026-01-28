# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

FalconFS is a high-performance distributed file system (DFS) optimized for AI workloads. It addresses three main challenges:
- **Massive small files** - High-performance metadata engine for billions of files
- **High throughput** - TB/s aggregated throughput via tiered storage (DRAM, SSD, object store)
- **Large scale** - Scales to thousands of NPUs/GPUs through distributed metadata engine

## Architecture

FalconFS consists of four main components:

1. **Metadata Engine** (`falcon/`)
   - PostgreSQL extension (shared-nothing architecture)
   - Distributed metadata management with ACID properties
   - Replicated directory namespace + sharded file metadata
   - Concurrent request merging for high throughput

2. **File Data Store** (`falcon_store/`)
   - Stores file data across local DRAM, SSDs, and cloud object store
   - Static library `FalconStore`

3. **Client** (`falcon_client/`)
   - POSIX API through FUSE (`fuse_main.cpp`)
   - LibFS interface for direct integration

4. **Cluster Management** (`deploy/`, `cloud_native/`)
   - Zookeeper for membership management
   - Ansible deployment for bare metal, Kubernetes for cloud

## Communication Plugins

FalconFS uses a plugin architecture for communication. Two plugins are supported:

- **brpc** (default) - `falcon/brpc_comm_adapter/`
- **hcom** - `falcon/hcom_comm_adapter/`

The plugin is selected via `--comm-plugin=brpc|hcom` when building.

## Build Commands

```bash
# Full build (PostgreSQL + FalconFS)
./build.sh

# Incremental builds
./build.sh build pg          # Only build PostgreSQL
./build.sh build falcon      # Only build FalconFS
./build.sh build falcon --debug   # Debug build
./build.sh build falcon --comm-plugin=hcom  # Use hcom plugin

# Clean
./build.sh clean             # Clean all
./build.sh clean pg          # Clean PostgreSQL only
./build.sh clean falcon      # Clean FalconFS only

# Test
./build.sh test              # Run unit tests (executes binaries in build/tests/falcon_store/)

# Install
./build.sh install           # Install all components
```

## Development Environment

The project uses a specific development container:
```bash
docker run -it --privileged -d -v `pwd`/..:/root/code -w /root/code/falconfs ghcr.io/falcon-infra/falconfs-dev:ubuntu24.04 /bin/zsh
```

For clangd LSP support:
```bash
ln -s /root/code/falconfs/build/compile_commands.json .
```

## Build System Details

- **PostgreSQL extension** (`falcon/`) - Uses traditional Makefile with PGXS, built as `falcon.so`
- **Client/Store** - CMake-based build in `build/` directory (uses Ninja)
- **Protobuf generation** - Run automatically by `build.sh` before CMake, outputs to `build/`
- **FlatBuffers generation** - Generated headers in `falcon/connection_pool/fbs/`

## Key Dependencies

- PostgreSQL (metadata backend)
- BRPC or HCOM (communication)
- FUSE (POSIX API)
- Zookeeper (cluster management)
- Protocol Buffers, LevelDB, GFlags, OpenSSL, glog, jsoncpp
- OBS SDK (assumed at `/usr/local/obs/include` and `/usr/local/obs/lib`)

## Code Conventions

- **Error handling**: Internal error codes are mapped to errno (see `fuse_main.cpp`). Do not return internal error codes directly as POSIX errno.
- **Statistics**: Use `FalconStats::GetInstance()` for metrics, `StatFuseTimer`/`META_LAT` macros for latency.
- **FUSE operations**: Entry points in `falcon_client/fuse_main.cpp` (`DoRead`, `DoWrite`, `DoOpen`, etc.)

## Testing

Unit tests are in `tests/` directory. Built test executables are placed in `build/tests/falcon_store/` and `build/tests/falcon_plugin/`. The `./build.sh test` command finds and runs all executables matching `*UT` in these directories.

## Configuration

Main configuration is in `config/config.json`:
- Storage settings (block size, cache directory)
- Cluster configuration (node IDs, cluster view)
- Performance tuning (thread counts, async operations)
- Monitoring (Prometheus integration)

## Important Notes

- When modifying PostgreSQL plugin code, edit in `falcon/` - it gets copied to `third_party/postgres/contrib/falcon` during build
- Modifying RPC interfaces requires updating protobuf definitions in `remote_connection_def/proto/`
- The `common/` directory contains shared utilities and error code definitions
