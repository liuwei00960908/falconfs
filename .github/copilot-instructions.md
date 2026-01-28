## FalconFS — 快速上手指引（供 AI 编码助手）

以下说明面向希望在本仓库快速定位、修改与构建代码的自动化/AI 编码代理；聚焦「能直接上手」的可执行信息与可验证模式。

- **项目大体**: FalconFS 是一个面向 AI 工作负载的分布式文件系统。
  - `falcon_store/`：核心存储与元数据实现（静态库 `FalconStore`）。
  - `falcon_client/`：用户态 FUSE 客户端入口与前端逻辑（`fuse_main.cpp`）。
  - `falcon/`：PostgreSQL `contrib/falcon` 插件代码（与 `third_party/postgres` 集成）。
  - `remote_connection_def/`：RPC/Proto 定义（生成 protobuf/BRPC 代码）。
  - `cloud_native/`, `deploy/`: 容器与部署脚本（k8s/ansible），用于生产/测试部署。

- **构建与常用命令**（来自顶层 `README.md` 与 `build.sh`）:
  - 全量构建（容器内常用）: `./build.sh` 或 `./build.sh build`。
  - 仅构建 PostgreSQL 插件: `./build.sh build pg [--debug|--deploy]`。
  - 仅构建 FalconFS: `./build.sh build falcon [--debug|--release|--relwithdebinfo] [--with-fuse-opt] [--with-zk-init] [--with-rdma] [--with-prometheus]`。
  - 运行测试（会在 `build/tests/falcon_store/` 下执行可执行文件）: `./build.sh test`。
  - 清理: `./build.sh clean [pg|falcon|test|dist]`。
  - 开发镜像: 使用 `ghcr.io/falcon-infra/falconfs-dev:ubuntu24.04`（见 README 中的 docker run 示例）。
  - 为 clangd 提供编译数据库: 在顶层执行 `ln -s build/compile_commands.json .`。

- **关键构建细节与陷阱（不要猜）**:
  - proto 生成：`build.sh` 在构建前调用 `protoc` 生成 `falcon_meta_rpc.pb.cc`/`.h`（proto 在 `remote_connection_def/proto`）。生成文件位于 `build/` 并通过 CMake 引入。
  - FlatBuffers 生成：项目包含 `generated/`（示例 `generated/falcon_meta_param_generated.h`）和 `cmake/Flatbuffers.cmake`。修改 schema 后需触发相关生成步骤。
  - OBS SDK：代码假定系统路径 `/usr/local/obs/include` 和 `/usr/local/obs/lib` 下有依赖（见 `falcon_store/CMakeLists.txt`），构建环境需提供这些库。

- **外部依赖（在 CMake/脚本中显式引用）**:
  - BRPC, Protobuf, GFlags, LevelDB, OpenSSL, glog, jsoncpp, zookeeper_mt
  - 系统库：`fuse`、`libpq` (Postgres client)、pthread、dl、z 等

- **代码约定与常见模式（以实例为准）**:
  - 错误/返回：内部使用 error-code→errno 映射（参见 `fuse_main.cpp` 中 `return ret > 0 ? -ErrorCodeToErrno(ret) : ret;`）。不要直接把内部 error-code 当作 POSIX errno。
  - 统计与延迟：使用单例 `FalconStats::GetInstance()` 计数、使用 `StatFuseTimer`/`META_LAT` 等宏测量延迟 — 在插桩或调试时优先使用现有统计点。
  - FUSE API：`falcon_client` 实现了典型的 FUSE 操作映射（`DoRead`, `DoWrite`, `DoOpen` 等），这些入口函数调用 `Falcon*` 前端接口（位于 `falcon_store` 或公共库）。修改行为时注意保持 FUSE 语义一致。
  - PostgreSQL 集成：`falcon/` 作为 `contrib` 模块被复制到 Postgres 源树并与 `make` 一起编译，调试 Postgres 插件请从 `third_party/postgres` 源树着手。

- **测试与验证位置**:
  - 单元/集成测试源码在 `tests/`，真正的可执行测试会被放到 `build/tests/`（`./build.sh test` 会运行 `build/tests/falcon_store/` 下的可执行文件）。

- **编辑/重构建议（AI 代理专用）**:
  - 变更 C/C++ 接口时，同时更新 `remote_connection_def/proto`（如影响 RPC）和任何 FlatBuffers schema；重新运行构建以生成 `.pb.cc/.h` 与生成头文件。
  - 若要修改 Postgres 插件，优先在 `falcon/` 编辑；构建步骤会把其复制进 `third_party/postgres/contrib/falcon`，但不要手动编辑 `third_party/postgres` 下的副本（会被覆盖）。
  - 在引入新外部库前，先查看 `cmake/` 中已有的 Find* 脚本（如 `FindBRPC.cmake`, `Protobuf.cmake`），遵循现有查找/链接约定。

- **快速参考（常用命令）**:
  - 全量构建：`./build.sh` 或 `docker run ... /root/code/falconfs /bin/zsh` 后 `./build.sh`
  - 仅 build falcon: `./build.sh build falcon --debug --with-fuse-opt`
  - 仅 build pg: `./build.sh build pg --debug`
  - 运行测试：`./build.sh test`

如需我把某部分扩展成更详细的“文件到函数”的索引（比如把 `falcon_store/src` 的重要类列出来并说明职责），或者在 `AGENTS.md` 中加入可执行的修复/PR 模板，请告诉我要优先覆盖的目录或任务。 
