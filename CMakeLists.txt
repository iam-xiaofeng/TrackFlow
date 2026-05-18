cmake_minimum_required(VERSION 3.20)
project(yolo_edge_server VERSION 1.0 LANGUAGES CXX)

# ============================================================================
# 编译器设置
# ============================================================================
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 编译选项
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-g -O0 -Wall -Wextra)
else()
    add_compile_options(-O3 -DNDEBUG)
endif()

# ============================================================================
# 依赖查找
# ============================================================================

# OpenCV
find_package(OpenCV REQUIRED)
message(STATUS "OpenCV version: ${OpenCV_VERSION}")

# spdlog 已被自定义日志库替代，不再需要
# find_package(spdlog REQUIRED)

# nlohmann_json
find_package(nlohmann_json REQUIRED)

# PROJ (使用pkg-config)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PROJ REQUIRED proj)

# MySQL client (libmysqlclient-dev)
pkg_check_modules(MYSQL REQUIRED mysqlclient)
message(STATUS "MySQL client version: ${MYSQL_VERSION}")

# Threads
find_package(Threads REQUIRED)

# ============================================================================
# ONNX Runtime (手动配置)
# ============================================================================
set(ONNXRUNTIME_ROOT "${CMAKE_SOURCE_DIR}/third_party/onnxruntime" CACHE PATH "ONNX Runtime安装路径")

if(EXISTS "${ONNXRUNTIME_ROOT}/include")
    set(ONNXRUNTIME_INCLUDE_DIRS "${ONNXRUNTIME_ROOT}/include")
    set(ONNXRUNTIME_LIBRARIES 
        "${ONNXRUNTIME_ROOT}/lib/libonnxruntime.so"
    )
    # GPU 支持（如果存在）
    if(EXISTS "${ONNXRUNTIME_ROOT}/lib/libonnxruntime_providers_cuda.so")
        list(APPEND ONNXRUNTIME_LIBRARIES 
            "${ONNXRUNTIME_ROOT}/lib/libonnxruntime_providers_cuda.so"
            "${ONNXRUNTIME_ROOT}/lib/libonnxruntime_providers_shared.so"
        )
        message(STATUS "ONNX Runtime CUDA provider found")
    endif()
    message(STATUS "ONNX Runtime found: ${ONNXRUNTIME_ROOT}")
else()
    message(WARNING "ONNX Runtime not found at ${ONNXRUNTIME_ROOT}, YOLO detector will be disabled")
    set(ONNXRUNTIME_INCLUDE_DIRS "")
    set(ONNXRUNTIME_LIBRARIES "")
endif()

# ============================================================================
# uWebSockets (Header-only + uSockets)
# ============================================================================
set(UWEBSOCKETS_ROOT "${CMAKE_SOURCE_DIR}/third_party/uWebSockets" CACHE PATH "uWebSockets路径")

if(EXISTS "${UWEBSOCKETS_ROOT}/src")
    set(UWEBSOCKETS_INCLUDE_DIRS 
        "${UWEBSOCKETS_ROOT}/src"
        "${UWEBSOCKETS_ROOT}/uSockets/src"
    )
    set(USOCKETS_LIBRARY "${UWEBSOCKETS_ROOT}/uSockets/uSockets.a")
    message(STATUS "uWebSockets found: ${UWEBSOCKETS_ROOT}")
else()
    message(FATAL_ERROR "uWebSockets not found at ${UWEBSOCKETS_ROOT}")
endif()

# ============================================================================
# 头文件目录
# ============================================================================
include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${OpenCV_INCLUDE_DIRS}
    ${ONNXRUNTIME_INCLUDE_DIRS}
    ${UWEBSOCKETS_INCLUDE_DIRS}
)

# ============================================================================
# 源文件
# ============================================================================
set(CORE_SOURCES
    src/core/processor_factory.cpp
    src/core/event_writer.cpp
    src/core/roi_registry.cpp
)

set(PROCESSOR_SOURCES
    src/processors/image_decoder.cpp
    src/processors/yolo_detector.cpp
    src/processors/byte_tracker.cpp
    src/processors/geo_transformer.cpp
    src/processors/undistort_processor.cpp
    src/processors/batch_inference_engine.cpp
    src/processors/traffic_analyzer.cpp
)

set(NETWORK_SOURCES
    src/network/ws_server.cpp
)

set(ALL_SOURCES
    ${CORE_SOURCES}
    ${PROCESSOR_SOURCES}
    ${NETWORK_SOURCES}
    src/main.cpp
)

# ============================================================================
# 可执行文件
# ============================================================================
add_executable(${PROJECT_NAME} ${ALL_SOURCES})

# GCC 11 -O3 reassoc pass ICE 在 src/network/ws_server.cpp 触发 (Ubuntu 22.04
# gcc-11.4.0). 单文件关掉 tree-reassoc 通行, 性能影响微乎其微.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
   AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 12)
  set_source_files_properties(src/network/ws_server.cpp
    PROPERTIES COMPILE_OPTIONS "-fno-tree-reassoc")
endif()

target_link_libraries(${PROJECT_NAME}
    ${OpenCV_LIBS}
    nlohmann_json::nlohmann_json
    ${PROJ_LIBRARIES}
    ${MYSQL_LIBRARIES}
    ${ONNXRUNTIME_LIBRARIES}
    ${USOCKETS_LIBRARY}
    Threads::Threads
    ssl
    crypto
    z
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${MYSQL_INCLUDE_DIRS}
)
target_link_directories(${PROJECT_NAME} PRIVATE
    ${MYSQL_LIBRARY_DIRS}
)

# RPATH设置 (用于找到ONNX Runtime动态库)
set_target_properties(${PROJECT_NAME} PROPERTIES
    BUILD_RPATH "${ONNXRUNTIME_ROOT}/lib"
    INSTALL_RPATH "${ONNXRUNTIME_ROOT}/lib"
)

# ============================================================================
# 安装
# ============================================================================
install(TARGETS ${PROJECT_NAME} DESTINATION bin)
install(DIRECTORY config/ DESTINATION etc/yolo_edge)
install(DIRECTORY models/ DESTINATION share/yolo_edge/models)
