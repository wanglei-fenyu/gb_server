#include "types.h"
#include "base/res_path.h"
#include "pugixml.hpp"
#include "define/def.h"
AppTypeMgr::AppTypeMgr()
{
	_names.insert(std::make_pair(APP_TYPE::APP_Global, "global"));

	_names.insert(std::make_pair(APP_TYPE::APP_DB_MGR, "dbmgr"));

	_names.insert(std::make_pair(APP_TYPE::APP_GAME_MGR, "gamemgr"));
	_names.insert(std::make_pair(APP_TYPE::APP_SPACE_MGR, "spacemgr"));

	_names.insert(std::make_pair(APP_TYPE::APP_APPMGR, "appmgr"));

	_names.insert(std::make_pair(APP_TYPE::APP_LOGIN, "login"));
	_names.insert(std::make_pair(APP_TYPE::APP_GAME, "game"));
	_names.insert(std::make_pair(APP_TYPE::APP_SPACE, "space"));
	_names.insert(std::make_pair(APP_TYPE::APP_ROBOT, "robot"));
	_names.insert(std::make_pair(APP_TYPE::APP_GATEWAY, "gateway"));

	_names.insert(std::make_pair(APP_TYPE::APP_ALL, "all"));
}

std::string AppTypeMgr::GetAppName(const APP_TYPE appType)
{
	const auto iter = _names.find(appType);
	if (iter == _names.end())
		return "";

	return iter->second;
}

std::pair<std::string, std::string> AppTypeMgr::GetServerIpPort(APP_TYPE app_type, int os_type)
{
	pugi::xml_document doc;
	const std::string file_path = ResPath::Instance()->FindResPath("config/server_config.xml");
	pugi::xml_parse_result result = doc.load_file(file_path.c_str());
	if (!result)
		return { "", "" };

	pugi::xml_node root = doc.document_element();
	std::string ip;
	std::string port;
	std::string os_entry;

	switch (os_type)
	{
	case UIR_TYPE::UT_None:
	#ifdef WIN32
		os_entry = "win_tcp";
	#else
		os_entry = "linux_tcp";
	#endif
		break;
	case UIR_TYPE::UT_WIN_TCP:
		os_entry = "win_tcp";
		break;
	case UIR_TYPE::UT_LINUX_TCP:
		os_entry = "linux_tcp";
		break;
	case UIR_TYPE::UT_WIN_HTTP:
		os_entry = "win_http";
		break;
	case UIR_TYPE::UT_LINUX_HTTP:
		os_entry = "linux_http";
		break;
	default:
		break;
	}

	pugi::xml_node node;

	if (app_type == APP_Global)
	{
		// Legacy flat config: <win_tcp ip="..." port="..."/>
		node = root.child(os_entry.c_str());
	}
	else
	{
		// Hierarchical config: <login><win_tcp ip="..." port="..."/></login>
		std::string app_name = GetAppName(app_type);
		if (!app_name.empty())
		{
			pugi::xml_node app_node = root.child(app_name.c_str());
			if (app_node)
				node = app_node.child(os_entry.c_str());
		}
	}

	if (node)
	{
		pugi::xml_attribute ip_attr = node.attribute("ip");
		if (ip_attr)
			ip = ip_attr.value();
		pugi::xml_attribute port_attr = node.attribute("port");
		if (port_attr)
			port = port_attr.value();
	}

	return { ip, port };
}
