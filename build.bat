@echo off
chcp 65001 >nul
setlocal enabledelayedexpansion

echo ==========================================
echo 开始 Windows 编译流程
echo ==========================================

REM 设置 VS 2026 环境
echo 初始化 Visual Studio 2026 x64 环境...
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64

REM 验证编译器是否可用
where cl >nul 2>nul
if %errorlevel% neq 0 (
    echo 错误: VS 环境初始化失败
    pause
    exit /b 1
)
echo VS x64 环境初始化成功!
echo.

echo 步骤 1/3: 安装依赖 (conan install)...
conan install . -pr=profiles/msvc_debug_pr --build=missing
if %errorlevel% neq 0 (
    echo 错误: conan install 失败
    pause
    exit /b %errorlevel%
)
echo 步骤 1/3 完成!
echo.

echo 步骤 2/3: 生成构建文件 (cmake configure)...
cmake --preset conan-debug
if %errorlevel% neq 0 (
    echo 错误: cmake configure 失败
    pause
    exit /b %errorlevel%
)
echo 步骤 2/3 完成!
echo.

echo ==========================================
echo 准备执行步骤 3/3: 编译项目
echo ==========================================
echo.
echo 即将执行命令: cmake --build --preset conan-debug --parallel
echo.
set /p confirm="是否继续编译? (Y/N): "

if /i "!confirm!" neq "Y" (
    echo 用户取消编译
    pause
    exit /b 0
)

echo.
echo 步骤 3/3: 编译项目...
cmake --build --preset conan-debug --parallel
if %errorlevel% neq 0 (
    echo 错误: cmake build 失败
    pause
    exit /b %errorlevel%
)

echo.
echo ==========================================
echo 编译成功完成!
echo ==========================================
echo.
echo 输出目录: build\conan-debug
pause