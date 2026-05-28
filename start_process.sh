#!/bin/sh

# 项目根目录
PROJECT_ROOT=~/workspace/gb_server
RES_PATH=$PROJECT_ROOT/res

# 初始化变量
BUILD_TYPE=""
EXE_NAME=""

# 显示帮助信息
show_help() {
    echo "Usage: $0 [OPTIONS]"
    echo "Options:"
    echo "  -t, --type TYPE    Build type: Debug or Release (default: prompt if missing)"
    echo "  -n, --name NAME    Executable name, e.g., server_test (default: prompt if missing)"
    echo "  -h, --help         Show this help message"
    exit 0
}

# 解析命令行参数
while [ $# -gt 0 ]; do
    case "$1" in
        -t|--type)
            BUILD_TYPE="$2"
            shift 2
            ;;
        -n|--name)
            EXE_NAME="$2"
            shift 2
            ;;
        -h|--help)
            show_help
            ;;
        *)
            echo "Unknown option: $1"
            show_help
            ;;
    esac
done

# 交互式获取构建类型（如果未通过参数提供）
if [ -z "$BUILD_TYPE" ]; then
    echo "Build type not specified."
    while true; do
        printf "Enter build type (Debug/Release): "
        read BUILD_TYPE
        case "$BUILD_TYPE" in
            Debug|Release)
                break
                ;;
            *)
                echo "Invalid input. Please enter 'Debug' or 'Release'."
                ;;
        esac
    done
fi

# 交互式获取可执行文件名（如果未通过参数提供）
if [ -z "$EXE_NAME" ]; then
    echo "Executable name not specified."
    printf "Enter executable name (e.g., server_test): "
    read EXE_NAME
    if [ -z "$EXE_NAME" ]; then
        echo "Error: Executable name cannot be empty."
        read -p "Press any key to continue..."
        exit 1
    fi
fi

# 构建可执行文件完整路径（注意路径中有两个 BUILD_TYPE）
CLIENT_EXE="$PROJECT_ROOT/build/$BUILD_TYPE/$BUILD_TYPE/bin/$EXE_NAME"

echo "Starting client..."
echo "Build type: $BUILD_TYPE"
echo "Executable: $CLIENT_EXE"
echo "Resource: $RES_PATH"
echo

# 检查可执行文件是否存在
if [ ! -f "$CLIENT_EXE" ]; then
    echo "ERROR: Client not found at $CLIENT_EXE"
    read -p "Press any key to continue..."
    exit 1
fi

# 运行客户端
"$CLIENT_EXE" -t 1 -r "$RES_PATH"

read -p "Press any key to continue..."
