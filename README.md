# gb_server
c++ 网络游戏服务器框架

# 编译
## windows
    1. 设置profile
    2. 执行 install_deps.bat
    3. conan install . -pr=profiles/msvc_debug_pr --build="protobuf*" --build="boost*"
       （首次安装需重建 boost（~5min）；之后可去掉 --build="boost*" 加速）
    4. cmake --preset conan-debug
    5. cmake --build --preset conan-debug --parallel 或者可以用vs打开cmake文件
    注意*: vs2026环境不会默认安装到系统环境，执行以上命令需要到  Command Prompt for VS下运行


## linux
    1. 设置profile,添加下面两行 conan有个bug Conan + Boost 1.90 的 recipe 在初始化阶段就不兼容 cobalt 模块, 取消boost charconv模块的float128支持
        [options]
        boost/*:without_cobalt=True
        [conf]
        tools.build:cxxflags+=["-DBOOST_CHARCONV_DISABLE_FLOAT128"]
    2. 执行 install_deps.sh
    3. conan install . -pr=profiles/clang_debug_pr --build="protobuf*" --build="boost*"（路径可换）
    4. cmake --preset conan-debug
    5. cmake --build --preset conan-debug --parallel
