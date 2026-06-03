cmake_policy(SET CMP0144 NEW)
set(TARGET_NAME client_test)
add_executable(${TARGET_NAME})

if(MSVC)
    target_compile_options(${TARGET_NAME} PRIVATE /bigobj)
endif()

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

# ========== 收集 protobuf 生成代码 ==========
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

if(NOT LINUX)
    #windown下分组
    source_group(${TARGET_NAME} TREE ${CMAKE_CURRENT_LIST_DIR} FILES ${MY_INC_SRC_MAIN})
    source_group(${TARGET_NAME} TREE ${COMMON_CPP_DIR} FILES ${ALL_INC_SRC})
    source_group("protobuf" TREE ${CMAKE_SOURCE_DIR} FILES ${PROT_INC_SRC})
endif()

target_sources(${TARGET_NAME} PRIVATE
    ${ALL_INC_SRC}
    ${PROT_INC_SRC}
    ${MY_INC_SRC_MAIN}
)

target_include_directories(${TARGET_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/src           # 项目公共源码
    ${CMAKE_SOURCE_DIR}               # protobuf 生成代码
    ${CMAKE_CURRENT_LIST_DIR}         # 当前子工程目录
)

add_game_cxx_modules(${TARGET_NAME})

conan_link_libraries(${TARGET_NAME} PRIVATE)
