cmake_minimum_required(VERSION 3.12)

file(GLOB_RECURSE LDPCBENCH_CPPS *.cpp)

add_executable(ldpc_bench ${LDPCBENCH_CPPS})
target_include_directories(ldpc_bench PUBLIC src-testing src-core)

# Core provides CCSDSLDPC, Sparse_matrix, SNR estimators, logger, plugin plumbing.
target_link_libraries(ldpc_bench PUBLIC gobdump_core)

# 16-lane AVX2 decoder lives in the simd_avx2 plugin; expose its include dir and
# link it (guarded by LDPC_BENCH_HAVE_AVX2). main.cpp self-registers the decoder.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../../plugins/simd_extensions/simd_avx2)
    target_include_directories(ldpc_bench PUBLIC ../../plugins/simd_extensions/simd_avx2)
    target_link_libraries(ldpc_bench PUBLIC simd_avx2)
    target_compile_definitions(ldpc_bench PUBLIC LDPC_BENCH_HAVE_AVX2)
endif()