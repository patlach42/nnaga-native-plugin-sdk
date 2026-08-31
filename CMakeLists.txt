cmake_minimum_required(VERSION 3.22.1)
project(nnaga_native_plugin_sdk LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
option(NNAGA_BUILD_TESTS "Build SDK tests" ON)
option(NNAGA_ENABLE_THINLTO "Enable ThinLTO for release artifacts" ON)

add_library(nnaga_plugin_filter SHARED src/nnaga_filter.cpp)
target_include_directories(nnaga_plugin_filter PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(nnaga_plugin_filter PRIVATE
    -O3 -ffp-contract=fast -fno-exceptions -fno-rtti -fvisibility=hidden)
set_target_properties(nnaga_plugin_filter PROPERTIES
    OUTPUT_NAME nnaga_plugin_filter
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES)
if(ANDROID)
    target_link_options(nnaga_plugin_filter PRIVATE -nostdlib++)
endif()
if(NNAGA_ENABLE_THINLTO AND CMAKE_BUILD_TYPE STREQUAL "Release" AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(nnaga_plugin_filter PRIVATE -flto=thin)
    target_link_options(nnaga_plugin_filter PRIVATE -flto=thin)
endif()

add_library(nnaga_plugin_shuffle SHARED src/nnaga_shuffle.cpp)
target_include_directories(nnaga_plugin_shuffle PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(nnaga_plugin_shuffle PRIVATE
    -O3 -ffp-contract=fast -fno-exceptions -fno-rtti -fvisibility=hidden)
set_target_properties(nnaga_plugin_shuffle PROPERTIES
    OUTPUT_NAME nnaga_plugin_shuffle
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES)
if(ANDROID)
    target_link_options(nnaga_plugin_shuffle PRIVATE -nostdlib++)
endif()
if(NNAGA_ENABLE_THINLTO AND CMAKE_BUILD_TYPE STREQUAL "Release" AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(nnaga_plugin_shuffle PRIVATE -flto=thin)
    target_link_options(nnaga_plugin_shuffle PRIVATE -flto=thin)
endif()

if(NNAGA_BUILD_TESTS)
    enable_testing()
    add_executable(nnaga_filter_tests tests/FilterTests.cpp src/nnaga_filter.cpp)
    target_include_directories(nnaga_filter_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_options(nnaga_filter_tests PRIVATE -O3 -ffp-contract=fast -fno-exceptions -fno-rtti)
    add_test(NAME nnaga_filter_tests COMMAND nnaga_filter_tests)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME nnaga_filter_metadata COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_metadata.py
        ${CMAKE_CURRENT_SOURCE_DIR}/package/filter.json)
    add_executable(nnaga_shuffle_tests tests/ShuffleTests.cpp src/nnaga_shuffle.cpp)
    target_include_directories(nnaga_shuffle_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_options(nnaga_shuffle_tests PRIVATE -O3 -ffp-contract=fast -fno-exceptions -fno-rtti)
    add_test(NAME nnaga_shuffle_tests COMMAND nnaga_shuffle_tests)
endif()
