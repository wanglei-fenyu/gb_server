#include "script/register_script.h"
#include "log/log.h"
#include "network/nats/nats_manager.h"

using namespace gb;

void register_nats(std::shared_ptr<Script>& scriptPtr)
{
    auto nats = scriptPtr->create_table("nats");

    // ── Connect / Disconnect ──────────────────────────
    nats["Connect"] = [](const std::string& url) -> int {
        return NatsManager::Instance()->Connect(url);
    };
    nats["Disconnect"] = []() {
        NatsManager::Instance()->Disconnect();
    };
    nats["IsConnected"] = []() -> bool {
        return NatsManager::Instance()->IsConnected();
    };

    // ── Publish (overloaded: raw bytes / protobuf / msgpack) ──
    // Publish(subject, meta_bytes, data_str)
    // Publish(subject, meta_bytes, proto_msg)
    // Publish(subject, meta_bytes, ...)
    // meta_bytes = BuildMeta output
    nats["Publish"] = sol::overload(
        // Raw bytes variant
        [](const std::string&          subject,
           const std::vector<uint8_t>& meta_bytes,
           const std::string&          data_str) {
            Meta meta{};
            if (meta_bytes.size() >= sizeof(meta))
                std::memcpy(&meta, meta_bytes.data(), sizeof(meta));
            std::vector<uint8_t> data(data_str.begin(), data_str.end());
            NatsManager::Instance()->Publish(subject, meta, data);
        },
        // Protobuf variant
        [](const std::string&          subject,
           const std::vector<uint8_t>& meta_bytes,
           sol::object                  proto_obj) {
            google::protobuf::Message* msg = proto_obj.as<google::protobuf::Message*>();
            if (!msg)
            {
                LOG_ERROR("[Lua] Publish: argument is not a protobuf message");
                return;
            }
            Meta meta{};
            if (meta_bytes.size() >= sizeof(meta))
                std::memcpy(&meta, meta_bytes.data(), sizeof(meta));
            NatsManager::Instance()->Publish(subject, meta, *msg);
        },
        // Msgpack variadic variant
        [](const std::string&          subject,
           const std::vector<uint8_t>& meta_bytes,
           sol::variadic_args           args) {
            Meta meta{};
            if (meta_bytes.size() >= sizeof(meta))
                std::memcpy(&meta, meta_bytes.data(), sizeof(meta));
            auto data = gb::msgpack::pack(args);
            NatsManager::Instance()->Publish(subject, meta, data);
        }
    );

    // ── Reply (overloaded: raw bytes / protobuf / msgpack) ──
    // Reply(reply_to, meta_bytes, data_str)
    // Reply(reply_to, meta_bytes, proto_msg)
    // Reply(reply_to, meta_bytes, ...)
    // Called inside a Subscribe handler to respond to a Request.
    nats["Reply"] = sol::overload(
        // Raw bytes variant
        [](const std::string&          reply_to,
           const std::vector<uint8_t>& meta_bytes,
           const std::string&          data_str) {
            Meta meta{};
            if (meta_bytes.size() >= sizeof(meta))
                std::memcpy(&meta, meta_bytes.data(), sizeof(meta));
            std::vector<uint8_t> data(data_str.begin(), data_str.end());
            NatsManager::Instance()->Reply(reply_to, meta, data);
        },
        // Protobuf variant
        [](const std::string&          reply_to,
           const std::vector<uint8_t>& meta_bytes,
           sol::object                  proto_obj) {
            google::protobuf::Message* msg = proto_obj.as<google::protobuf::Message*>();
            if (!msg)
            {
                LOG_ERROR("[Lua] Reply: argument is not a protobuf message");
                return;
            }
            Meta meta{};
            if (meta_bytes.size() >= sizeof(meta))
                std::memcpy(&meta, meta_bytes.data(), sizeof(meta));
            NatsManager::Instance()->Reply(reply_to, meta, *msg);
        },
        // Msgpack variadic variant
        [](const std::string&          reply_to,
           const std::vector<uint8_t>& meta_bytes,
           sol::variadic_args           args) {
            Meta meta{};
            if (meta_bytes.size() >= sizeof(meta))
                std::memcpy(&meta, meta_bytes.data(), sizeof(meta));
            auto data = gb::msgpack::pack(args);
            NatsManager::Instance()->Reply(reply_to, meta, data);
        }
    );

    // ── Subscribe (overloaded: raw bytes / protobuf / msgpack) ──
    // Subscribe(subject, handler)
    // Subscribe(subject, handler, proto_name)
    // Subscribe(subject, handler, "msgpack")
    nats["Subscribe"] = sol::overload(
        // Raw bytes: handler(meta_tbl, body_str, reply_to)
        [](const std::string& subject, sol::function handler_fn) {
            NatsHandler handler;
            if (!handler_fn.valid()) return;
            handler = [handler_fn](const Meta& meta,
                                   const std::vector<uint8_t>& body,
                                   const std::string& reply_to) {
                sol::state_view lua(handler_fn.lua_state());
                sol::table meta_tbl = lua.create_table();
                meta_tbl["mode"]          = static_cast<int>(meta.mode);
                meta_tbl["user_unique_id"]     = meta.user_unique_id;
                meta_tbl["type"]          = meta.type;
                meta_tbl["method"]        = meta.method;
                meta_tbl["sequence"]      = meta.sequence;
                meta_tbl["compress_type"] = static_cast<int>(meta.compress_type);

                std::string body_str(reinterpret_cast<const char*>(body.data()),
                                     body.size());
                handler_fn(meta_tbl, body_str, reply_to);
            };
            NatsManager::Instance()->Subscribe(subject, std::move(handler));
        },
        // Proto / Msgpack: handler(meta_tbl, proto_tbl, reply_to)
        //             or  handler(meta_tbl, values_tbl, reply_to)
        [](const std::string& subject, sol::function handler_fn, const std::string& mode) {
            NatsHandler handler;
            if (!handler_fn.valid()) return;

            if (mode == "msgpack")
            {
                handler = [handler_fn](const Meta& meta,
                                       const std::vector<uint8_t>& body,
                                       const std::string& reply_to) {
                    sol::state_view lua(handler_fn.lua_state());

                    sol::table meta_tbl = lua.create_table();
                    meta_tbl["mode"]          = static_cast<int>(meta.mode);
                    meta_tbl["user_unique_id"]     = meta.user_unique_id;
                    meta_tbl["type"]          = meta.type;
                    meta_tbl["method"]        = meta.method;
                    meta_tbl["sequence"]      = meta.sequence;
                    meta_tbl["compress_type"] = static_cast<int>(meta.compress_type);

                    sol::variadic_args unpacked = gb::msgpack::unpack(lua, body);
                    sol::table values_tbl = lua.create_table();
                    int idx = 1;
                    for (auto it = unpacked.begin(); it != unpacked.end(); ++it, ++idx)
                        values_tbl[idx] = *it;

                    handler_fn(meta_tbl, values_tbl, reply_to);
                };
            }
            else
            {
                // Treat mode as protobuf type name
                handler = [handler_fn, mode](const Meta& meta,
                                             const std::vector<uint8_t>& body,
                                             const std::string& reply_to) {
                    sol::state_view lua(handler_fn.lua_state());

                    sol::table meta_tbl = lua.create_table();
                    meta_tbl["mode"]          = static_cast<int>(meta.mode);
                    meta_tbl["user_unique_id"]     = meta.user_unique_id;
                    meta_tbl["type"]          = meta.type;
                    meta_tbl["method"]        = meta.method;
                    meta_tbl["sequence"]      = meta.sequence;
                    meta_tbl["compress_type"] = static_cast<int>(meta.compress_type);

                    auto create_msg_fn = lua["create_msg"];
                    if (!create_msg_fn.valid())
                    {
                        handler_fn(meta_tbl, sol::nil, reply_to);
                        return;
                    }
                    sol::object lua_msg = create_msg_fn(mode);
                    google::protobuf::Message* msg = lua_msg.as<google::protobuf::Message*>();
                    if (!msg || !msg->ParseFromArray(body.data(), static_cast<int>(body.size())))
                    {
                        handler_fn(meta_tbl, sol::nil, reply_to);
                        return;
                    }
                    handler_fn(meta_tbl, lua_msg, reply_to);
                };
            }
            NatsManager::Instance()->Subscribe(subject, std::move(handler));
        }
    );

    // ── AsyncRequest ───────────────────────────────────────────────
    // AsyncRequest(subject, meta_bytes, data_str, callback(err, body_str), timeout_ms?)
    auto async_request = [](const std::string&          subject,
                            const std::vector<uint8_t>& meta_bytes,
                            const std::string&          data_str,
                            sol::function               callback,
                            sol::optional<int>          timeout_ms) {
        if (!callback.valid())
            return;

        Meta meta{};
        if (meta_bytes.size() >= sizeof(meta))
            std::memcpy(&meta, meta_bytes.data(), sizeof(meta));

        std::vector<uint8_t> data(data_str.begin(), data_str.end());
        auto cb_ptr = std::make_shared<sol::function>(std::move(callback));

        NatsManager::Instance()->AsyncRequest(
            subject,
            meta,
            data,
            [cb_ptr](int ec, std::vector<uint8_t> body) {
                if (!cb_ptr || !cb_ptr->valid())
                    return;

                if (ec != NatsError::OK)
                {
                    (*cb_ptr)("nats request failed", sol::lua_nil);
                    return;
                }

                std::string body_str(reinterpret_cast<const char*>(body.data()), body.size());
                (*cb_ptr)("", body_str);
            },
            std::chrono::milliseconds(timeout_ms.value_or(5000)));
    };

    nats["AsyncRequest"] = async_request;

    // AsyncRequestMsgpack(subject, meta_bytes, ...[, timeout_ms], callback)
    // callback(err, unpacked...)
    nats["AsyncRequestMsgpack"] = [scriptPtr](const std::string& subject,
                                               const std::vector<uint8_t>& meta_bytes,
                                               sol::variadic_args args) {
        std::vector<sol::object> argv;
        argv.reserve(static_cast<size_t>(args.size()));
        for (auto it = args.begin(); it != args.end(); ++it)
            argv.emplace_back(*it);

        if (argv.empty() || !argv.back().is<sol::function>())
        {
            LOG_ERROR("[Lua] AsyncRequestMsgpack: missing callback");
            return;
        }

        sol::function callback = argv.back().as<sol::function>();
        argv.pop_back();

        int timeout_ms = 5000;
        if (!argv.empty() && argv.back().is<int>())
        {
            timeout_ms = argv.back().as<int>();
            argv.pop_back();
        }

        sol::state_view lua(scriptPtr->lua_state());
        sol::table req_vals = lua.create_table();
        for (size_t i = 0; i < argv.size(); ++i)
            req_vals[i + 1] = argv[i];

        std::vector<uint8_t> req_data = gb::msgpack::pack(req_vals);
        std::string req_payload(reinterpret_cast<const char*>(req_data.data()), req_data.size());

        async_request(subject, meta_bytes, req_payload, std::move(callback), timeout_ms);
    };

    // AsyncRequestProto(subject, meta_bytes, request_proto_obj, response_proto_name[, timeout_ms], callback)
    // callback(err, response_proto_obj)
    nats["AsyncRequestProto"] = [scriptPtr](const std::string&          subject,
                                             const std::vector<uint8_t>& meta_bytes,
                                             sol::object                  request_obj,
                                             const std::string&           response_proto,
                                             sol::variadic_args           args) {
        std::vector<sol::object> argv;
        argv.reserve(static_cast<size_t>(args.size()));
        for (auto it = args.begin(); it != args.end(); ++it)
            argv.emplace_back(*it);

        if (argv.empty() || !argv.back().is<sol::function>())
        {
            LOG_ERROR("[Lua] AsyncRequestProto: missing callback");
            return;
        }

        sol::function callback = argv.back().as<sol::function>();
        argv.pop_back();

        int timeout_ms = 5000;
        if (!argv.empty() && argv.back().is<int>())
        {
            timeout_ms = argv.back().as<int>();
            argv.pop_back();
        }

        google::protobuf::Message* req_msg = request_obj.as<google::protobuf::Message*>();
        if (!req_msg)
        {
            callback("invalid protobuf request", sol::lua_nil);
            return;
        }

        std::string req_payload;
        if (!req_msg->SerializeToString(&req_payload))
        {
            callback("protobuf serialize failed", sol::lua_nil);
            return;
        }

        auto cb_ptr = std::make_shared<sol::function>(std::move(callback));
        async_request(subject, meta_bytes, req_payload,
            [scriptPtr, cb_ptr, response_proto](const std::string& err, sol::object body_obj) {
                if (!cb_ptr || !cb_ptr->valid())
                    return;

                if (!err.empty())
                {
                    (*cb_ptr)(err, sol::lua_nil);
                    return;
                }

                if (!body_obj.is<std::string>())
                {
                    (*cb_ptr)("invalid response body", sol::lua_nil);
                    return;
                }

                std::string body = body_obj.as<std::string>();
                sol::state_view lua(scriptPtr->lua_state());
                sol::object create_msg_fn_obj = lua["create_msg"];
                if (!create_msg_fn_obj.valid() || !create_msg_fn_obj.is<sol::function>())
                {
                    (*cb_ptr)("create_msg not found", sol::lua_nil);
                    return;
                }

                sol::function create_msg_fn = create_msg_fn_obj.as<sol::function>();
                sol::object rsp_obj = create_msg_fn(response_proto);
                google::protobuf::Message* rsp_msg = rsp_obj.as<google::protobuf::Message*>();
                if (!rsp_msg || !rsp_msg->ParseFromArray(body.data(), static_cast<int>(body.size())))
                {
                    (*cb_ptr)("protobuf parse failed", sol::lua_nil);
                    return;
                }

                (*cb_ptr)("", rsp_obj);
            },
            timeout_ms);
    };

    // ═══════════════════════════════════════════════════════════════════════
    // Lua 协程桥接 — nats.Await(method, ...)
    //
    // 目前支持：
    //   nats.Await("Request", subject, meta_bytes, body_str [, timeout_ms])
    //   nats.Await("RequestMsgpack", subject, meta_bytes, ... [, timeout_ms])
    //   nats.Await("RequestProto", subject, meta_bytes, request_proto, response_proto_name [, timeout_ms])
    // ═══════════════════════════════════════════════════════════════════════

    lua_State* L = nats.lua_state();
    luaL_dostring(L, R"(
        if not nats.Await then
            function nats.Await(method, ...)
                local co = coroutine.running()
                if not co then
                    error("nats.Await() must be called from a coroutine")
                end

                local args = { ... }
                local results = nil
                local yielded = false

                local function cb(...)
                    results = { ... }
                    if yielded then
                        local ok, err = coroutine.resume(co)
                        if not ok then
                            error("nats.Await() resume failed: " .. tostring(err))
                        end
                    end
                end

                args[#args + 1] = cb
                if method ~= "Request" and method ~= "RequestMsgpack" and method ~= "RequestProto" then
                    error("Unknown await method: nats." .. tostring(method))
                end
                local async_fn = nats["Async" .. method]
                if not async_fn then
                    error("Unknown async method: nats.Async" .. method)
                end

                async_fn(table.unpack(args))

                if results == nil then
                    yielded = true
                    coroutine.yield()
                    yielded = false
                end

                return table.unpack(results)
            end
        end
    )");

    LOG_INFO("NATS Lua API registered (async + await)");
}
