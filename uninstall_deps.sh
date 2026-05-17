#!/bin/bash

echo "========================================"
echo "GBServer - Uninstall Third-Party Packages"
echo "========================================"
echo ""

read -p "Are you sure? This will remove gbluasocket and gbnet! (y/N): " confirm
if [[ ! "$confirm" =~ ^[Yy]$ ]]; then
    echo "Cancelled."
    exit 0
fi
echo ""

# 颜色定义
GREEN='\033[0;32m'
NC='\033[0m'

# 卸载第三方包
echo "[1/2] Uninstalling third-party packages..."
cd 3rd
chmod +x uninstall.sh
./uninstall.sh
cd ..
echo ""

# 删除 build 目录
echo "[2/2] Removing build directory..."
if [ -d "build" ]; then
    rm -rf build
    echo "Build directory removed"
else
    echo "No build directory found"
fi

echo ""
echo "========================================"
echo -e "${GREEN}[SUCCESS] Third-party packages removed!${NC}"
echo "========================================"
echo ""
echo "Note: Conan itself is still installed."
echo ""