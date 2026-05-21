# gb_server
c++ 网络游戏服务器框架

# 编译
## windows
    1. 设置profile 
    2. 执行 install_deps.bat
    3.  三选1 
        conan install . --build=missing 
        conan install . --build=missing -s build_type=Debug
        conan install . -of build -pr=profiles/win_debug_profile --build=missin  （路径可换）
    4. echo   cmake --preset conan-debug
    5. cmake --build --preset conan-debug --parallel


## linux
    1. 设置profile,添加下面两行 conan有个bug Conan + Boost 1.90 的 recipe 在初始化阶段就不兼容 cobalt 模块, 取消boost charconv模块的float128支持
        [options]
        boost/*:without_cobalt=True
        [conf]
        tools.build:cxxflags+=["-DBOOST_CHARCONV_DISABLE_FLOAT128"]
    2. 执行 install_deps.sh
    3. conan install . -pr=profiles/linux_debug_profile --build=missing（路径可换）
    4. cmake --preset conan-debug
    5. cmake --build --preset conan-debug --parallel
