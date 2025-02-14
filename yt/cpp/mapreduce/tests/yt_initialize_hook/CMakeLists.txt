# This file was generated by the build system used internally in Yandex and called "ya"
# (https://github.com/yandex/yatool).
#
# Configuration files of ya build system are usually named ya.make. If ya.make file is present
# at the root of the repository, then this repository supports ya build.
#
# If the repository supports both CMake and ya build configurations, please modify both of them.
#
# If only CMake build configuration is supported then modify only CMake files and note that only
# simple modifications are allowed like adding source-files to targets or adding simple properties
# like target_include_directories. These modifications will be ported to original ya.make files
# by maintainers. Any complex modifications which can't be easily ported back to the ya build
# system may be rejected.

find_package(linux-headers-generic REQUIRED)

add_library(mapreduce-tests-yt_initialize_hook INTERFACE)

target_link_libraries(mapreduce-tests-yt_initialize_hook INTERFACE
  linux-headers-generic::linux-headers-generic
  contrib-libs-cxxsupp
  yutil
  cpp-testing-hook
  cpp-mapreduce-common
  cpp-mapreduce-interface
  cpp-mapreduce-client
)


add_global_library_for(mapreduce-tests-yt_initialize_hook.global
  mapreduce-tests-yt_initialize_hook
)

target_compile_options(mapreduce-tests-yt_initialize_hook.global PRIVATE
  -Wdeprecated-this-capture
)

target_link_libraries(mapreduce-tests-yt_initialize_hook.global PUBLIC
  linux-headers-generic::linux-headers-generic
  contrib-libs-cxxsupp
  yutil
  cpp-testing-hook
  cpp-mapreduce-common
  cpp-mapreduce-interface
  cpp-mapreduce-client
)

target_sources(mapreduce-tests-yt_initialize_hook.global PRIVATE
  ${PROJECT_SOURCE_DIR}/yt/cpp/mapreduce/tests/yt_initialize_hook/yt_hook.cpp
)

