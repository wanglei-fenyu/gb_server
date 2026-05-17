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

# 安装第三方包
echo "[3/4] Installing third-party packages..."
cd 3rd
chmod +x setup.sh
./setup.sh
if [ $? -ne 0 ]; then
    cd ..
    echo -e "${RED}[ERROR] Failed to install third-party packages${NC}"
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