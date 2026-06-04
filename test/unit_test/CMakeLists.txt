# ============================================================
# unit_test 子工程 — Catch2 单元测试
# ============================================================

set(TARGET_NAME unit_test)

cmake_policy(SET CMP0144 NEW)

add_executable(${TARGET_NAME})

if(MSVC)
    target_compile_options(${TARGET_NAME} PRIVATE /bigobj)
endif()

# ========== 编译宏定义 ==========
add_definitions(-DPROTOBUF_USE_DLLS)
add_compile_definitions(BOOST_ASIO_HAS_STD_THREAD)

target_compile_definitions(${TARGET_NAME} PRIVATE
    BOOST_CHARCONV_DISABLE_FLOAT128
)

# ========== 收集公共源码（src 目录）==========
if(NOT COMMON_CPP_DIR)
    set(COMMON_CPP_DIR ${CMAKE_SOURCE_DIR}/src)
endif()
set(ALL_FILE ${COMMON_CPP_DIR})
collectHeadFiles(ALL_INC ALL_FILE)
collectSrcFiles(ALL_SRC ALL_FILE)
collectAllSrcFiles(ALL_INC_SRC ALL_FILE)

# ========== 收集 protobuf 生成代码 ==========
set(PROTOBUF_DIR ${CMAKE_SOURCE_DIR}/protobuf)
set(PROT_FILE ${PROTOBUF_DIR})
collectHeadFiles(PROT_INC PROT_FILE)
collectSrcFiles(PROT_SRC PROT_FILE)
collectAllSrcFiles(PROT_INC_SRC PROT_FILE)

# ========== 收集当前子工程源码 ==========
if(NOT MAIN_CPP_DIR)
    set(MAIN_CPP_DIR ${CMAKE_CURRENT_LIST_DIR})
endif()
set(ALL_MY_FILE_MAIN ${MAIN_CPP_DIR})

collectHeadFiles(MY_INC_MAIN ALL_MY_FILE_MAIN)
collectSrcFiles(MY_INC_SRC_MAIN ALL_MY_FILE_MAIN)
collectAllSrcFiles(MY_INC_SRC_MAIN ALL_MY_FILE_MAIN)


# ========== 添加源文件 ==========
target_sources(${TARGET_NAME} PRIVATE
    ${ALL_INC_SRC}
    ${PROT_INC_SRC}
    ${MY_INC_SRC_MAIN}
)

# ========== 包含目录 ==========
target_include_directories(${TARGET_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_CURRENT_LIST_DIR}
)

conan_link_libraries(${TARGET_NAME} PRIVATE)

# ========== Catch2 链接 ==========
target_link_libraries(${TARGET_NAME} PRIVATE Catch2::Catch2)
