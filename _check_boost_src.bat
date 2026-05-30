@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x86_amd64 >nul 2>&1
echo === Boost Exception src dir ===
dir /s /b "C:\Users\wangl\.conan2\p\b\boost9f74a31980b8f\b\src\libs\exception\*.cpp" 2>&1
if errorlevel 1 echo No src/ dir, checking alternate paths...
dir /s /b "C:\Users\wangl\.conan2\p\b\boost9f74a31980b8f\*\libs\exception\*.cpp" 2>&1
echo === Checking build-debug for exception ===
dir "C:\Users\wangl\.conan2\p\b\boost9f74a31980b8f\b\build-debug\libs\exception" 2>&1
echo === throw_exception in log lib ===
dumpbin /symbols "C:\Users\wangl\.conan2\p\b\boost9f74a31980b8f\p\lib\libboost_log.lib" 2>&1 | findstr /i "throw_exception"
