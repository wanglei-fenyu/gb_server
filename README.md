# gb_server
c++ 网络游戏服务器框架

# 编译
## windows
    1. 设置profile 
    2. 执行 install_deps.bat
    3. conan install . --build=missing  或者 conan install . --build=missing -s build_type=Debug
    4. cmake -B build 
    5. cmake --build build --config Debug  或者 打开build下的sln


## linux
    1. 设置profile,添加下面两行 conan有个bug Conan + Boost 1.90 的 recipe 在初始化阶段就不兼容 cobalt 模块
        [options]
        boost/*:without_cobalt=True
    2. 执行 install_deps.sh
    3. conan install . --build=missing  或者 conan install . --build=missing -s build_type=Debug
    4. cmake -B build 
    5. cmake --build build --config Debug  或者 打开build下的sln