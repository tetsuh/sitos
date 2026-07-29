add_executable(sitos_cache_get_bench cache_get_bench.cpp)
target_link_libraries(sitos_cache_get_bench PRIVATE sitos::sitos benchmark::benchmark)
sitos_enable_warnings(sitos_cache_get_bench)

if(SITOS_WITH_ROCKSDB)
  add_executable(sitos_rocksdb_snapshot_bench rocksdb_snapshot_bench.cpp)
  target_link_libraries(sitos_rocksdb_snapshot_bench PRIVATE sitos::sitos benchmark::benchmark)
  sitos_enable_warnings(sitos_rocksdb_snapshot_bench)
endif()
