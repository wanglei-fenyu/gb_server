# tools/luasocket/CMakeLists.txt
# Subdirectory CMakeLists — relies on parent project already calling find_package(lua)
# and providing the lua::lua target via Conan CMakeDeps.

# C standard for Lua C sources (luasocket is pure C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_definitions(GB_LUASOCKET)

# 平台特定文件
if(WIN32)
    set(PLATFORM_SPECIFIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src/wsocket.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/wsocket.c
    )
else()
    set(PLATFORM_SPECIFIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src/usocket.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/usocket.c
    )
endif()

# 源文件列表
set(SOCKET_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/luasocket.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/inet.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tcp.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/udp.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/except.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/select.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/timeout.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/buffer.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/io.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/auxiliar.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/options.c
    ${PLATFORM_SPECIFIC}
)

# 创建库
add_library(gbluasocket SHARED ${SOCKET_SOURCES})

# 设置输出名称
set_target_properties(gbluasocket PROPERTIES
    PREFIX ""
    OUTPUT_NAME "core"
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
)

# 链接 Lua（由父项目通过 Conan 提供 lua::lua target）
target_link_libraries(gbluasocket PRIVATE lua::lua)

# Windows 需要 Winsock2
if(WIN32)
    target_link_libraries(gbluasocket PRIVATE ws2_32)
endif()

# 输出目录：匹配运行时 require("socket.core") 的查找路径
#   Linux:   build/<config>/<config>/bin/socket/core.so
#   Windows: build/<config>/<config>/bin/socket/core.dll
set_target_properties(gbluasocket PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>/bin/socket
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>/bin/socket
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>/lib
)

# ALIAS target — 供 cmake/base.cmake 的 collect_dlls 查找
add_library(gbluasocket::gbluasocket ALIAS gbluasocket)

# 复制 Lua 脚本到 socket 目录
file(GLOB LUA_SCRIPTS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.lua
)

foreach(LUA_SCRIPT ${LUA_SCRIPTS})
    get_filename_component(SCRIPT_NAME ${LUA_SCRIPT} NAME)
    add_custom_command(
        TARGET gbluasocket POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${LUA_SCRIPT}
            ${CMAKE_BINARY_DIR}/$<CONFIG>/bin/socket/${SCRIPT_NAME}
        COMMENT "Copying ${SCRIPT_NAME} to socket directory"
    )
endforeach()
