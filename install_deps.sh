#!/bin/bash

set -e

echo "========================================"
echo "GBServer - Install Dependencies"
echo "========================================"
echo ""

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# 检查 Python
echo "[1/4] Checking Python..."
if ! command -v python3 &> /dev/null; then
    echo -e "${RED}[ERROR] Python3 is not installed${NC}"
    echo "Please install Python 3.7+"
    exit 1
fi
echo -e "${GREEN}[OK] Python3 found${NC}"
echo ""

# 检查 Conan
echo "[2/4] Checking Conan..."
if ! command -v conan &> /dev/null; then
    echo -e "${YELLOW}[INFO] Conan not found, installing...${NC}"
    pip3 install conan
    if [ $? -ne 0 ]; then
        echo -e "${RED}[ERROR] Failed to install Conan${NC}"
        exit 1
    fi
fi
echo -e "${GREEN}[OK] Conan found${NC}"
echo ""

# 处理参数：如果没有参数，则提示用户输入（模拟 bat 行为）
if [ $# -eq 0 ]; then
    echo -e "${YELLOW}[INFO] No arguments provided for third-party installation.${NC}"
    read -p "Enter arguments (e.g., --profile \"myprofile.profile\"): " user_args
    if [ -z "$user_args" ]; then
        echo -e "${YELLOW}[WARNING] No arguments entered, proceeding without arguments.${NC}"
        set --   # 清空参数列表
    else
        # 将用户输入的字符串转换为位置参数（用户需自行对含空格的参数加引号）
        eval "set -- $user_args"
    fi
fi

# 安装第三方包
echo "[3/4] Installing third-party packages..."
cd 3rd
chmod +x setup.sh
# 使用 "$@" 正确传递所有参数（包括空格和引号）
./setup.sh "$@"
if [ $? -ne 0 ]; then
    cd ..
    echo -e "${RED}[ERROR] Failed to install third-party packages${NC}"
    echo -e "${YELLOW}[HINT] If you see Git connection errors (e.g., Failed to connect to github.com), please check your network or configure proxy:${NC}"
    echo "  git config --global http.proxy http://proxy:port"
    echo "  git config --global https.proxy https://proxy:port"
    echo "  Or use SSH: set GIT_URL to git@github.com:user/repo.git in packages.json"
    exit 1
fi
cd ..
echo ""

# 安装 Conan 依赖
echo "[4/4] Installing Conan dependencies..."
conan install . --build=missing
if [ $? -ne 0 ]; then
    echo -e "${RED}[ERROR] Failed to install Conan dependencies${NC}"
    exit 1
fi
echo ""

echo "========================================"
echo -e "${GREEN}[SUCCESS] All dependencies installed!${NC}"
echo "========================================"
echo ""
echo "Next steps:"
echo "  cmake --preset conan-default"
echo "  cmake --build --preset conan-release"
echo ""