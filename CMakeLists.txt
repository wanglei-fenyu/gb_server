cmake_minimum_required(VERSION 3.15)
add_definitions(-D_CRT_SECURE_NO_WARNINGS)
# 包含自定义函数和配置
include(cmake/base.cmake)
include(cmake/def.cmake)
list(APPEND CMAKE_PREFIX_PATH "${CMAKE_BINARY_DIR}/generators")
# 项目定义
project(GBServer)

# 引入 Conan toolchain
if(EXISTS ${CMAKE_BINARY_DIR}/generators/conan_toolchain.cmake)
    include(${CMAKE_BINARY_DIR}/generators/conan_toolchain.cmake)
    message(STATUS "Loaded Conan toolchain")
elseif(EXISTS conan_toolchain.cmake)
    include(conan_toolchain.cmake)
    message(STATUS "Loaded Conan toolchain from root")
endif()


# 查找所有 Conan 依赖包（全局，子目录共享）
find_package(gbnet REQUIRED)
find_package(Boost REQUIRED COMPONENTS charconv)
find_package(spdlog REQUIRED)
find_package(protobuf REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)
find_package(lua REQUIRED)
find_package(mimalloc REQUIRED)
find_package(async_simple REQUIRED)
find_package(concurrentqueue REQUIRED)
find_package(RapidJSON  REQUIRED)
find_package(rapidxml REQUIRED)
find_package(sol2 REQUIRED)
find_package(cxxopts REQUIRED)
find_package(PostgreSQL REQUIRED)

# 定义全局编译宏
add_compile_definitions(
    # PROTOBUF_USE_DLLS  # 静态链接 protobuf 时不需要
    BOOST_ASIO_HAS_STD_THREAD
    BOOST_CHARCONV_DISABLE_FLOAT128
    BOOST_BIND_GLOBAL_PLACEHOLDERS
)

# 设置静态库和动态库的输出目录
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>/bin)

# add_subdirectory(tools/luasocket)  # temporarily disabled for build verify
add_subdirectory(test/server_test)
add_subdirectory(test/client_test)

# Game server processes
add_subdirectory(server/login_server)
add_subdirectory(server/gateway_server)
add_subdirectory(server/scene_server)
add_subdirectory(test/db_test)
