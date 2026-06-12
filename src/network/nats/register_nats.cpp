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
}
