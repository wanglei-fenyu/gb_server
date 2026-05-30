#include "res_path.h"
#include <ostream>
#include "util_string.h"

#ifdef _WIN32
#   define WIN32_LEAN_AND_MEAN
#    include <stdlib.h>
#else
#    include <unistd.h>
#    include <limits.h>
#endif
#include <gbnet/common/def.h>

ResPath::ResPath()
{
}

void ResPath::SetResRootPath(const std::string& path)
{
    _resPath = path;

    // 判断路径末尾是否有 / 或 \，如果没有就加上
    if (!_resPath.empty())
    {
        char lastChar = _resPath.back();
        if (lastChar != '/' && lastChar != '\\')
        {
            _resPath += '/'; // 统一使用左斜杠
        }
    }
}

std::string ResPath::FindResPath(const std::string& res)
{
    // 如果第一个字符是 '/'，则去掉
    const char* path = res.c_str();
    if (res.size() > 0 && res[0] == '/')
    {
        path = res.c_str() + 1;
    }
    return FindResPath(path);
}

std::string ResPath::FindResPath(const char* res)
{
    auto fpath = _resPath + res;

    strutil::replace(fpath, "\\", "/");
    strutil::replace(fpath, "//", "/");

    return fpath;
}


std::string ResPath::GetCurrentExePath()
{
#ifdef _WIN32
    // Windows 平台下使用 _pgmptr，不需要 windows.h
    return std::string(_pgmptr);
#else
    // Linux 平台
    char    buffer[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (len != -1)
    {
        buffer[len] = '\0';
        return std::string(buffer);
    }
    return "";
#endif
}

std::string ResPath::GetCurrentExeDirectory()
{
    std::string exePath = GetCurrentExePath();
    if (exePath.empty()) return "";

    // 查找最后一个路径分隔符
    size_t pos = exePath.find_last_of("/\\");
    if (pos != std::string::npos)
    {
        return exePath.substr(0, pos + 1);
    }
    return "";
}
