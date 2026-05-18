cmake_policy(SET CMP0144 NEW)
set(TARGET_NAME client_test)
add_executable(${TARGET_NAME})
add_definitions(-DPROTOBUF_USE_DLLS)  # protobuf使用动态库
add_compile_definitions(BOOST_ASIO_HAS_STD_THREAD) # asio使用std::thread

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

if(NOT MAIN_CPP_DIR)
    set(MAIN_CPP_DIR ${CMAKE_CURRENT_LIST_DIR})
endif()
set(ALL_MY_FILE_MAIN ${MAIN_CPP_DIR})

collectHeadFiles(MY_INC_MAIN ALL_MY_FILE_MAIN)
collectSrcFiles(MY_INC_SRC_MAIN ALL_MY_FILE_MAIN)
collectAllSrcFiles(MY_INC_SRC_MAIN ALL_MY_FILE_MAIN)

if(LINUX)
else()
    #windown下分组
    source_group(${TARGET_NAME} TREE ${CMAKE_CURRENT_LIST_DIR} FILES ${MY_INC_SRC_MAIN})
    source_group(${TARGET_NAME} TREE ${COMMON_CPP_DIR} FILES ${ALL_INC_SRC})
endif()

target_sources(${TARGET_NAME} PRIVATE
    ${ALL_INC_SRC}
    ${MY_INC_SRC_MAIN}
)

target_include_directories(${TARGET_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/src           # 项目公共源码
    ${CMAKE_CURRENT_LIST_DIR}         # 当前子工程目录
)

conan_link_libraries(${TARGET_NAME} PRIVATE)
