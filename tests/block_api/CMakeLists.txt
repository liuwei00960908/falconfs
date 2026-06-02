add_executable(BlockApiE2E block_api_e2e.cpp)
add_executable(BlockApiBench block_api_bench.cpp)

target_link_libraries(BlockApiE2E
    FalconClient
    FalconStore
    ${BRPC_LIBRARIES}
    ${DYNAMIC_LIB}
    pthread
    pq
    fuse
    zookeeper_mt
)

target_include_directories(BlockApiE2E PRIVATE
    ${PROJECT_SOURCE_DIR}/falcon_client/src/include
    ${PROJECT_SOURCE_DIR}/falcon_store/src/include
    ${PROJECT_SOURCE_DIR}/common/src/include
)

target_link_libraries(BlockApiBench
    FalconClient
    FalconStore
    ${BRPC_LIBRARIES}
    ${DYNAMIC_LIB}
    pthread
    pq
    fuse
    zookeeper_mt
)

target_include_directories(BlockApiBench PRIVATE
    ${PROJECT_SOURCE_DIR}/falcon_client/src/include
    ${PROJECT_SOURCE_DIR}/falcon_store/src/include
    ${PROJECT_SOURCE_DIR}/common/src/include
)
