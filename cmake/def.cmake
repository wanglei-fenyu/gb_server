include_guard()

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)  # 防止使用c++20之下的编译器
set(CMAKE_CXX_EXTENSIONS OFF)        # 禁止使用编译器的扩展

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
    
    # 直接使用 target 链接（头文件和库会自动传递）
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
endmacro()

# 环境信息
message("CMAKE_SYSTEM_NAME: ${CMAKE_SYSTEM_NAME} CMAKE_SYSTEM_VERSION: ${CMAKE_SYSTEM_VERSION}")
message("CMAKE_VERSION: ${CMAKE_VERSION} CMAKE_CXX_STANDARD: ${CMAKE_CXX_STANDARD}")
message("CMAKE_BUILD_TYPE: ${CMAKE_BUILD_TYPE} CMAKE_GENERATOR: ${CMAKE_GENERATOR}")
message("CMAKE_CXX_COMPILER: ${CMAKE_CXX_COMPILER}")
message("CMAKE_CXX_COMPILER_ID: ${CMAKE_CXX_COMPILER_ID}")
message("CMAKE_CXX_COMPILER_VERSION: ${CMAKE_CXX_COMPILER_VERSION}")
message("CMAKE_CONFIGURATION_TYPES: ${CMAKE_CONFIGURATION_TYPES}")

if(MSVC)
    # MSVC 编译选项，无论使用 Visual Studio 还是 Ninja 生成器都会生效
    add_compile_options(/utf-8)
	add_compile_options(/source-charset:utf-8 /execution-charset:utf-8)
    add_compile_options(/bigobj)
    add_compile_options(/EHa)
endif()

if(LINUX)
    add_compile_definitions(LINUX)
    
    # ========== 根据编译器类型设置不同的标志 ==========
    if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        # Clang 编译器标志
        set(CMAKE_CXX_FLAGS_DEBUG "-g -O0")
        set(CMAKE_CXX_FLAGS_RELEASE "-g -O2")
        set(CMAKE_CXX_FLAGS_ASAN "-g -Og -fsanitize=address -fsanitize-recover=address -fno-omit-frame-pointer -fsanitize=leak")
        string(APPEND CMAKE_CXX_FLAGS " -pthread -Wall -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-function -Wunused-result -Wno-gnu-folding-constant")
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        # GCC 编译器标志
        set(CMAKE_CXX_FLAGS_DEBUG "-g -O0")
        set(CMAKE_CXX_FLAGS_RELEASE "-g -O2")
        set(CMAKE_CXX_FLAGS_ASAN "-g -Og -fsanitize=address -fsanitize-recover=address -fno-omit-frame-pointer -fsanitize=leak")
        string(APPEND CMAKE_CXX_FLAGS " -pthread -fcoroutines -Wall -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-function -Wunused-result")
    else()
        message(WARNING "Unknown compiler: ${CMAKE_CXX_COMPILER_ID}")
    endif()
    
elseif(WIN32)
    # Windows 相关配置（暂未修改）
else()
    message(FATAL_ERROR "unsupportted OS")
endif()