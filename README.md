# gb_server
c++ 网络游戏服务器框架

# 编译
## windows
    1. 执行 install_deps.bat
    2. conan install . --build=missing  或者 conan install . --build=missing -s build_type=Debug
    3. cmake -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE="./build/generators/conan_toolchain.cmake"
    4. cmake --build build --config Debug  或者 打开build下的sln
