include_guard()

set_property(GLOBAL PROPERTY USE_FOLDERS ON)


set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)  #防止使用c++20之下的编译器
set(CMAKE_CXX_EXTENSIONS OFF)        #禁止使用编译器的扩展

set(CMAKE_VERBOSE_MAKEFILE ON)
set(CMAKE_CONFIGURATION_TYPES "debug;release") # 限定构建模式 统一跨平台大小写
set(CMAKE_POSITION_INDEPENDENT_CODE OFF)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(LINUX true)
endif()





# 用法：conan_link_libraries(target_name [PRIVATE|PUBLIC|INTERFACE])
macro(conan_link_libraries target_name)
    set(_link_type PRIVATE)
    foreach(_arg ${ARGN})
        if(_arg STREQUAL "PUBLIC" OR _arg STREQUAL "INTERFACE" OR _arg STREQUAL "PRIVATE")
            set(_link_type ${_arg})
        endif()
    endforeach()
    
    # 原有的 Conan 依赖
    target_link_libraries(${target_name} ${_link_type}
        gbnet::gbnet
        Boost::boost
        spdlog::spdlog
        protobuf::libprotobuf
        OpenSSL::SSL
        OpenSSL::Crypto
        ZLIB::ZLIB
        lua::lua
        mimalloc-static
        async_simple::async_simple_header_only
        concurrentqueue::concurrentqueue
        rapidjson
        rapidxml::rapidxml
        sol2::sol2
        cxxopts::cxxopts
    )
    
    # Linux 下额外链接 
    if(LINUX)
        target_link_options(${target_name} ${_link_type}
       	 "LINKER:--push-state,--no-as-needed,-lquadmath,--pop-state"
        )
    endif()
endmacro()


# 环境信息
message("CMAKE_SYSTEM_NAME: ${CMAKE_SYSTEM_NAME} CMAKE_SYSTEM_VERSION: ${CMAKE_SYSTEM_VERSION}")
message("CMAKE_VERSION: ${CMAKE_VERSION} CMAKE_CXX_STANDARD: ${CMAKE_CXX_STANDARD}")
message("CMAKE_BUILD_TYPE: ${CMAKE_BUILD_TYPE} CMAKE_GENERATOR: ${CMAKE_GENERATOR}")
message("CMAKE_CXX_COMPILER: ${CMAKE_CXX_COMPILER}")
message("CMAKE_CXX_COMPILER_ID: ${CMAKE_CXX_COMPILER_ID}")
message("CMAKE_CXX_COMPILER_VERSION: ${CMAKE_CXX_COMPILER_VERSION}")
message("CMAKE_CONFIGURATION_TYPES: ${CMAKE_CONFIGURATION_TYPES}")



if("${CMAKE_GENERATOR}" MATCHES "Visual Studio")
    message("OpenType: generate VS Sln")
    add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")
    add_compile_options("$<$<C_COMPILER_ID:MSVC>:/utf-8>")
    add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/bigobj>")
else()
    if(NOT CMAKE_BUILD_TYPE)
        message(FATAL_ERROR "If you are using single-config generator, you should set CMAKE_BUILD_TYPE=<Debug|Release>")
    endif()
endif()

if(LINUX)
    add_compile_definitions(LINUX)
    set(CMAKE_CXX_FLAGS_DEBUG "-g -O0")
    set(CMAKE_CXX_FLAGS_RELEASE "-g -O2")
    set(CMAKE_CXX_FLAGS_ASAN "-g -Og -fsanitize=address -fsanitize-recover=address -fno-omit-frame-pointer -fsanitize=leak")
    string(APPEND CMAKE_CXX_FLAGS "  -pthread -fcoroutines -Wall -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-function -Wunused-result ")
    #set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -lquadmath")
elseif(WIN32)

else()
    message(FATAL_ERROR "unsupportted OS")
endif()
