cmake_minimum_required(VERSION 3.15)
project(gbluasocket VERSION 0.1.0)

# 查找 Lua
find_package(lua REQUIRED)

# 添加头文件路径
include_directories(${lua_INCLUDE_DIRS})
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src)

if(UNIX)
    add_compile_options(-fPIC)
endif()
add_compile_definitions(GB_LUASOCKET)

# 平台特定文件
IF(WIN32)
    SET(PLATFORM_SPECIFIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src/wsocket.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/wsocket.c
    )
ELSEIF(UNIX)
    SET(PLATFORM_SPECIFIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src/usocket.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/usocket.c
    )
ENDIF()

# 源文件列表
SET(SOCKET_SOURCES
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
ADD_LIBRARY(gbluasocket SHARED ${SOCKET_SOURCES})

# 设置输出名称
SET_TARGET_PROPERTIES(gbluasocket PROPERTIES
    PREFIX ""
    OUTPUT_NAME "core"
)

# 链接 Lua
TARGET_LINK_LIBRARIES(gbluasocket lua::lua)

IF(WIN32)
    TARGET_LINK_LIBRARIES(gbluasocket ws2_32)
ENDIF()

# 覆盖输出目录到 socket 子目录
set_target_properties(gbluasocket PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>/bin/socket
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>/lib
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>/lib
)

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

# 将 core.so 复制到 socket 目录（Linux/Unix）
if(UNIX)
    add_custom_command(
        TARGET gbluasocket POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            ${CMAKE_BINARY_DIR}/$<CONFIG>/bin/socket/
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:gbluasocket>
            ${CMAKE_BINARY_DIR}/$<CONFIG>/bin/socket/
        COMMENT "Copying core.so to socket directory"
    )
endif()

# 安装规则
install(TARGETS gbluasocket
    RUNTIME DESTINATION bin/socket
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/src/luasocket.h DESTINATION include/gbluasocket)
install(FILES ${LUA_SCRIPTS} DESTINATION bin/socket)