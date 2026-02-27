cmake_minimum_required(VERSION 3.15)
project(rtsp_grpc_server CXX)

set(CMAKE_CXX_STANDARD 14)

# 1. 查找 OpenCV
find_package(OpenCV REQUIRED)

# 2. 查找 Protobuf (系统自带通常支持)
find_package(Protobuf REQUIRED)

# 3. 查找 gRPC (关键修改：使用 PkgConfig 替代 CONFIG 模式)
find_package(PkgConfig REQUIRED)
# 查找 grpc++ 和 grpc 库
pkg_check_modules(GRPC REQUIRED grpc++ grpc)

# 查找 gRPC C++ 插件 (用于生成代码)
find_program(GRPC_CPP_PLUGIN_EXECUTABLE grpc_cpp_plugin)
if(NOT GRPC_CPP_PLUGIN_EXECUTABLE)
    message(FATAL_ERROR "grpc_cpp_plugin not found! Please install protobuf-compiler-grpc")
endif()

# 4. 编译 Proto 文件
set(PROTO_SRC "${CMAKE_CURRENT_SOURCE_DIR}/stream_service.proto")
set(PROTO_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}")

# 确保生成目录存在
file(MAKE_DIRECTORY ${PROTO_GEN_DIR})

add_custom_command(
    OUTPUT "${PROTO_GEN_DIR}/stream_service.pb.cc"
           "${PROTO_GEN_DIR}/stream_service.pb.h"
           "${PROTO_GEN_DIR}/stream_service.grpc.pb.cc"
           "${PROTO_GEN_DIR}/stream_service.grpc.pb.h"
    COMMAND protoc
    ARGS --grpc_out="${PROTO_GEN_DIR}"
         --cpp_out="${PROTO_GEN_DIR}"
         -I "${CMAKE_CURRENT_SOURCE_DIR}"
         --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN_EXECUTABLE}
         "${PROTO_SRC}"
    DEPENDS "${PROTO_SRC}"
)

# 5. 包含头文件目录
include_directories(
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${PROTO_GEN_DIR}"
    ${OpenCV_INCLUDE_DIRS}
    ${GRPC_INCLUDE_DIRS}  # 添加 gRPC 的头文件路径
)

# 6. 收集源码
file(GLOB SRC_FILES "src/*.cpp")

# 7. 编译可执行文件
add_executable(rtsp_server 
    ${SRC_FILES}
    "${PROTO_GEN_DIR}/stream_service.pb.cc" 
    "${PROTO_GEN_DIR}/stream_service.grpc.pb.cc"
)

# 8. 链接库 (关键修改：链接 PkgConfig 找到的库)
target_link_libraries(rtsp_server 
    ${OpenCV_LIBS}
    ${GRPC_LIBRARIES}      # 使用 PkgConfig 返回的库列表
    protobuf::libprotobuf 
    # 有时系统安装的 grpc++ 需要显式链接下面这几个，视具体情况而定，通常上面的 GRPC_LIBRARIES 够了
    # 如果运行报错 undefined reference，可以解开下面注释
    # grpc++_reflection 
    # dl
)