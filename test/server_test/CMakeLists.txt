# ============================================================
# server_test 子工程
# ============================================================

# 定义目标名称
set(TARGET_NAME server_test)

# 设置 CMake 策略
cmake_policy(SET CMP0144 NEW)

# 创建可执行文件
add_executable(${TARGET_NAME})

# ========== 编译宏定义 ==========
# protobuf 使用动态库
add_definitions(-DPROTOBUF_USE_DLLS)
# Asio 使用 std::thread
add_compile_definitions(BOOST_ASIO_HAS_STD_THREAD)

# ========== 收集公共源码（src 目录）==========
if(NOT COMMON_CPP_DIR)
    set(COMMON_CPP_DIR ${CMAKE_SOURCE_DIR}/src)
endif()
set(ALL_FILE ${COMMON_CPP_DIR})
collectHeadFiles(ALL_INC ALL_FILE)
collectSrcFiles(ALL_SRC ALL_FILE)
collectAllSrcFiles(ALL_INC_SRC ALL_FILE)

# ========== 收集当前子工程源码 ==========
if(NOT MAIN_CPP_DIR)
    set(MAIN_CPP_DIR ${CMAKE_CURRENT_LIST_DIR})
endif()
set(ALL_MY_FILE_MAIN ${MAIN_CPP_DIR})

collectHeadFiles(MY_INC_MAIN ALL_MY_FILE_MAIN)
collectSrcFiles(MY_INC_SRC_MAIN ALL_MY_FILE_MAIN)
collectAllSrcFiles(MY_INC_SRC_MAIN ALL_MY_FILE_MAIN)

# ========== Visual Studio 分组 ==========
if(LINUX)
    # Linux 下不需要分组
else()
    source_group(${TARGET_NAME} TREE ${CMAKE_CURRENT_LIST_DIR} FILES ${MY_INC_SRC_MAIN})
    source_group(${TARGET_NAME} TREE ${COMMON_CPP_DIR} FILES ${ALL_INC_SRC})
endif()

# ========== 添加源文件 ==========
target_sources(${TARGET_NAME} PRIVATE
    ${ALL_INC_SRC}
    ${MY_INC_SRC_MAIN}
)

# ========== 包含目录（仅项目自己的头文件）==========
# Conan 管理的库会自动处理头文件路径
target_include_directories(${TARGET_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/src           # 项目公共源码
    ${CMAKE_CURRENT_LIST_DIR}         # 当前子工程目录
)

conan_link_libraries(${TARGET_NAME} PRIVATE)