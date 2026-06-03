#pragma once
#include "singleton.h"
#include <string>
class ResPath : public Singleton<ResPath>
{
public:
    ResPath();
    void SetResRootPath(const std::string& path);
	std::string FindResPath(const std::string& res);
	std::string FindResPath(const char* res);
    std::string GetCurrentExePath();
	std::string GetCurrentExeDirectory();

private:
	std::string _resPath;
};


