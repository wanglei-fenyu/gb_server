#!/bin/bash

# Linux 编译脚本
# 使用方法: ./build.sh [venv_path]
# 例如: ./build.sh ~/my_venv

set -e

echo "=========================================="
echo "开始 Linux 编译流程"
echo "=========================================="

# 获取脚本所在目录
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# 虚拟环境路径（可通过参数指定，默认为 ~/.venv）
VENV_PATH="${1:-$HOME/.venv}"

# 检查并激活虚拟环境
if [ -d "$VENV_PATH" ]; then
    echo "激活虚拟环境: $VENV_PATH"
    source "$VENV_PATH/bin/activate"
    
    if [ -z "$VIRTUAL_ENV" ]; then
        echo "错误: 虚拟环境激活失败"
        exit 1
    fi
    echo "虚拟环境已激活: $VIRTUAL_ENV"
else
    echo "错误: 虚拟环境不存在: $VENV_PATH"
    echo ""
    echo "请先创建虚拟环境:"
    echo "  python3 -m venv $VENV_PATH"
    echo "  source $VENV_PATH/bin/activate"
    echo "  pip install conan"
    exit 1
fi
echo ""

# 检查 conan
if ! command -v conan &> /dev/null; then
    echo "错误: conan 未找到"
    echo "请安装: pip install conan"
    exit 1
fi

echo "环境信息:"
echo "  Python: $(python3 --version 2>&1)"
echo "  Conan: $(conan --version 2>&1)"
echo "  CMake: $(cmake --version | head -n1)"
echo ""

# 检查 clang 编译器
if ! command -v clang &> /dev/null; then
    echo "警告: clang 未找到，请确保已安装"
    echo "  Ubuntu/Debian: sudo apt install clang"
    echo "  CentOS/RHEL: sudo yum install clang"
    echo ""
fi

echo "步骤 1/3: 安装依赖 (conan install)..."
conan install . -pr=profiles/clang_debug_pr --build=missing
echo "步骤 1/3 完成!"
echo ""

echo "步骤 2/3: 生成构建文件 (cmake configure)..."
cmake --preset conan-debug
echo "步骤 2/3 完成!"
echo ""

echo "=========================================="
echo "准备执行步骤 3/3: 编译项目"
echo "=========================================="
echo ""
read -p "是否继续编译? (Y/N): " confirm

if [[ ! "$confirm" =~ ^[Yy]$ ]]; then
    echo "用户取消编译操作"
    exit 0
fi

echo ""
echo "步骤 3/3: 编译项目..."
cmake --build --preset conan-debug --parallel

echo ""
echo "=========================================="
echo "编译成功完成!"
echo "=========================================="