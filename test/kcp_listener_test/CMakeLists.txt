# ============================================================
# kcp_listener_test 子工程
# 验证:KcpListener 多连接建立/回显/keep-alive 重建(ServerImpl KCP 分支)
# ============================================================

set(TARGET_NAME kcp_listener_test)

cmake_policy(SET CMP0144 NEW)

add_executable(${TARGET_NAME})

if(MSVC)
    target_compile_options(${TARGET_NAME} PRIVATE /bigobj)
endif()

add_definitions(-DPROTOBUF_USE_DLLS)
add_compile_definitions(BOOST_ASIO_HAS_STD_THREAD)

target_compile_definitions(${TARGET_NAME} PRIVATE
    BOOST_CHARCONV_DISABLE_FLOAT128
)

if(NOT COMMON_CPP_DIR)
    set(COMMON_CPP_DIR ${CMAKE_SOURCE_DIR}/src)
endif()
set(ALL_FILE ${COMMON_CPP_DIR})
collectHeadFiles(ALL_INC ALL_FILE)
collectSrcFiles(ALL_SRC ALL_FILE)
collectAllSrcFiles(ALL_INC_SRC ALL_FILE)

set(PROTOBUF_DIR ${CMAKE_SOURCE_DIR}/protobuf)
set(PROT_FILE ${PROTOBUF_DIR})
collectHeadFiles(PROT_INC PROT_FILE)
collectSrcFiles(PROT_SRC PROT_FILE)
collectAllSrcFiles(PROT_INC_SRC PROT_FILE)

if(NOT MAIN_CPP_DIR)
    set(MAIN_CPP_DIR ${CMAKE_CURRENT_LIST_DIR})
endif()
set(ALL_MY_FILE_MAIN ${MAIN_CPP_DIR})

collectHeadFiles(MY_INC_MAIN ALL_MY_FILE_MAIN)
collectSrcFiles(MY_INC_SRC_MAIN ALL_MY_FILE_MAIN)
collectAllSrcFiles(MY_INC_SRC_MAIN ALL_MY_FILE_MAIN)

target_sources(${TARGET_NAME} PRIVATE
    ${ALL_INC_SRC}
    ${PROT_INC_SRC}
    ${MY_INC_SRC_MAIN}
)

target_include_directories(${TARGET_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_CURRENT_LIST_DIR}
)

conan_link_libraries(${TARGET_NAME} PRIVATE)
