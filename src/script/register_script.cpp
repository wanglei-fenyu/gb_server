#include "script.h"
#include "register_script.h"
#include "msgpack/msgpack.hpp"
#include "network/io/session.h"
#include "network/manager/network_manager.h"
#include "log/log.h"
#include "network/io/message_meta.h"
#include <filesystem>
#include "gbnet/buffer/compressed_def.h"
#include "network/rpc/register_rpc.h"
using namespace gb;


static void register_net(std::shared_ptr<Script>& scriptPtr)
{
	auto network		= scriptPtr->create_table("net");

    // 注册 MsgMode 枚举
    scriptPtr->new_enum<gb::MsgMode>("MsgMode", {
        {"Msg",      gb::MsgMode::Msg},
        {"Request",  gb::MsgMode::Request},
        {"Response", gb::MsgMode::Response}
    });
    
    // 注册 CompressType 枚举
    scriptPtr->new_enum<CompressType>("CompressType", {
        {"None", CompressType::CompressTypeNone},
        {"Gzip", CompressType::CompressTypeGzip},
        {"Zlib", CompressType::CompressTypeZlib},
        {"LZ4",  CompressType::CompressTypeLZ4}
    });
    
    // 注册 Meta 结构体
    scriptPtr->new_usertype<gb::Meta>("Meta",
        sol::constructors<gb::Meta(), gb::Meta(const gb::Meta&)>(),
        
        // 成员变量可读写
        "mode",          &gb::Meta::mode,
        "entity_id",            &gb::Meta::entity_id,
        "type",          &gb::Meta::type,
        "method",        &gb::Meta::method,
        "sequence",      &gb::Meta::sequence,
        "compress_type", &gb::Meta::compress_type,
        
        // 成员函数（通过自定义函数实现）
        sol::meta_function::to_string, [](const gb::Meta& self) {
            return "Meta{mode=" + std::to_string(static_cast<int>(self.mode)) 
                 + ", entity_id=" + std::to_string(self.entity_id)
                 + ", type=" + std::to_string(self.type)
                 + ", method=" + std::to_string(self.method)
                 + ", sequence=" + std::to_string(self.sequence)
                 + ", compress_type=" + std::to_string(static_cast<int>(self.compress_type))
                 + "}";
        },
        
        // 比较操作符（可选）
        sol::meta_function::equal_to, [](const gb::Meta& lhs, const gb::Meta& rhs) {
            return lhs.mode == rhs.mode 
                && lhs.entity_id == rhs.entity_id
                && lhs.type == rhs.type
                && lhs.method == rhs.method
                && lhs.sequence == rhs.sequence
                && lhs.compress_type == rhs.compress_type;
        }
    );


	network["Listen"]	= [](uint32_t type, sol::function  f,std::string protoName = "") {  gb::NetworkManager::Instance()->Listen(type, f, protoName); };
	network["UnListen"] = [](uint32_t type,  std::string signal, int level = 0) { gb::NetworkManager::Instance()->UnListen(type, signal, level); };
    network["Send"]     = sol::overload([](Session* session, uint32_t type, uint64_t id, sol::object lua_msg) {
                                            google::protobuf::Message* messgae = lua_msg.as<google::protobuf::Message*>();
                                            if (messgae)
                                            {
                                                gb::NetworkManager::Instance()->Send(session, type, id, *messgae);
                                            }
                                        },
                                    [](std::shared_ptr<Session> session, uint32_t type, uint64_t id, sol::object lua_msg) {
                                        google::protobuf::Message* messgae = lua_msg.as<google::protobuf::Message*>();
                                            if (messgae)
                                            {
                                                gb::NetworkManager::Instance()->Send(session, type, id, *messgae);
                                            }
                                        });
    // ── BuildMeta ─────────────────────────────────
    // Lua: meta_table -> binary bytes
    network["BuildMeta"] = [](sol::table meta_tbl) -> std::vector<uint8_t> {
        Meta meta;
        meta.mode           = static_cast<MsgMode>(meta_tbl.get_or("mode", 0));
        meta.entity_id      = meta_tbl.get_or<uint64_t>("entity_id", 0);
        meta.type           = meta_tbl.get_or<uint32_t>("type", 0);
        meta.method         = meta_tbl.get_or<uint64_t>("method", 0);
        meta.sequence       = meta_tbl.get_or<uint64_t>("sequence", 0);
        meta.compress_type  = static_cast<CompressType>(meta_tbl.get_or("compress_type", 0));
        std::vector<uint8_t> out(sizeof(meta));
        std::memcpy(out.data(), &meta, sizeof(meta));
        return out;
    };

    // ── ParseMeta ─────────────────────────────────
    // binary bytes -> Lua table
    network["ParseMeta"] = [scriptPtr](const std::vector<uint8_t>& data) -> sol::table {
        Meta meta{};
        if (data.size() >= sizeof(meta))
            std::memcpy(&meta, data.data(), sizeof(meta));
        sol::state_view lua(scriptPtr->lua_state());
        sol::table tbl = lua.create_table();
        tbl["mode"]          = static_cast<int>(meta.mode);
        tbl["entity_id"]     = meta.entity_id;
        tbl["type"]          = meta.type;
        tbl["method"]        = meta.method;
        tbl["sequence"]      = meta.sequence;
        tbl["compress_type"] = static_cast<int>(meta.compress_type);
        return tbl;
    };

	scriptPtr->new_usertype<Session>("Session");

	scriptPtr->script(R"(   
	function create_msg(proto_name)
		message = _G[proto_name]
		if message then
			return message.new()
		else
			log.Error("message not found for "..proto_name)
			return nil
		end
	end
    )");

}


class LuaSourceCache
{
public:
    const char* Intern(const std::string& file)
    {
        auto it = files_.find(file);
        if (it != files_.end())
            return it->second->c_str();

        auto        ptr = std::make_unique<std::string>(file);
        const char* ret = ptr->c_str();
        files_.emplace(file, std::move(ptr));
        return ret;
    }

private:
    std::unordered_map<std::string, std::unique_ptr<std::string>> files_;
};

static void register_log(std::shared_ptr<Script>& scriptPtr)
{
    auto logger = spdlog::get(LOG_NAME);
    if (!logger)
        return;
    static thread_local LuaSourceCache lua_files;

    auto log    = scriptPtr->create_table("log");
    log["Info"] = [&scriptPtr,logger](std::string str) {
		sol::state_view lua(scriptPtr->lua_state());
        sol::table debug_info = lua["debug"]["getinfo"](2, "Sl");
		std::string file_path = debug_info["short_src"];
		std::string file_name = std::filesystem::path(file_path).filename().string();
        int line = debug_info["currentline"];
        spdlog::source_loc loc(lua_files.Intern(file_name),line, nullptr);
        logger->log(loc, spdlog::level::info, str);
    };
    log["Error"] = [&scriptPtr,logger](std::string str) {
		sol::state_view lua(scriptPtr->lua_state());
        sol::table debug_info = lua["debug"]["getinfo"](2, "Sl");
		std::string file_path = debug_info["short_src"];
		std::string file_name = std::filesystem::path(file_path).filename().string();
        int line = debug_info["currentline"];
        spdlog::source_loc loc(lua_files.Intern(file_name),line, nullptr);
        logger->log(loc, spdlog::level::err, str);
    };
    log["Warning"] = [&scriptPtr,logger](std::string str) {
		sol::state_view lua(scriptPtr->lua_state());
        sol::table debug_info = lua["debug"]["getinfo"](2, "Sl");
		std::string file_path = debug_info["short_src"];
		std::string file_name = std::filesystem::path(file_path).filename().string();
        int line = debug_info["currentline"];
        spdlog::source_loc loc(lua_files.Intern(file_name),line, nullptr);
        logger->log(loc, spdlog::level::warn, str);
    };



}


static void register_msgpack(std::shared_ptr<Script>& scriptPtr)
{
	using namespace gb::msgpack;
	auto msgpack = scriptPtr->create_table("msgpack");
	msgpack["pack"] = sol::overload(
		[](sol::variadic_args args)->std::vector<uint8_t> {return pack(args); },
		[](sol::variadic_args&& args)->std::vector<uint8_t> {return pack(std::forward<sol::variadic_args&&>(args)); },
		[](sol::protected_function_result& args)->std::vector<uint8_t> {return pack(args); }
		//[](sol::protected_function_result&& args)->std::vector<uint8_t> {return std::move(pack(std::move(args))); }
	);
	msgpack["unpack"] = sol::overload(
	/*	[](sol::state_view& state, const uint8_t* dataStart, const std::size_t size)->sol::variadic_args {return unpack(state, dataStart, size); },
		[](sol::state_view& state, const uint8_t* dataStart, const std::size_t size, std::error_code& ec)->sol::variadic_args {return unpack(state, dataStart, size,ec); },
		[](sol::state_view& state, const std::vector<uint8_t>& data)->sol::variadic_args {return unpack(state, data); },
		[](sol::state_view& state, const std::vector<uint8_t>& data,  std::error_code& ec)->sol::variadic_args {return unpack(state, data, ec); }*/
		[scriptPtr](const uint8_t* dataStart, const std::size_t size)->sol::variadic_args {return unpack((sol::state_view&)(*scriptPtr), dataStart, size); },
		[scriptPtr](const uint8_t* dataStart, const std::size_t size, std::error_code& ec)->sol::variadic_args {return unpack((sol::state_view&)(*scriptPtr), dataStart, size,ec); },
		[scriptPtr](const std::vector<uint8_t>& data)->sol::variadic_args {return unpack((sol::state_view&)(*scriptPtr), data); },
		[scriptPtr](const std::vector<uint8_t>& data,  std::error_code& ec)->sol::variadic_args {return unpack((sol::state_view&)(*scriptPtr), data, ec); }

	);
	
	scriptPtr->new_usertype<std::vector<uint8_t>>("vec_uint8");
}

extern void register_proto_msg(std::shared_ptr<Script>& scriptPtr);
extern void register_redis(std::shared_ptr<Script>& scriptPtr);
extern void register_postgresql(std::shared_ptr<Script>& scriptPtr);
extern void register_nats(std::shared_ptr<Script>& scriptPtr);
extern void register_timer(std::shared_ptr<Script>& scriptPtr);

void _lua_(std::shared_ptr<Script>& scriptPtr)
{
	register_log(scriptPtr);
	register_msgpack(scriptPtr);
    register_proto_msg(scriptPtr);
	register_net(scriptPtr);
    gb::RegisterRpcLua(scriptPtr);
    register_redis(scriptPtr);
    register_postgresql(scriptPtr);
    register_nats(scriptPtr);
    register_timer(scriptPtr);
}
