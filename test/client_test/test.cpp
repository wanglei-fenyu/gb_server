#include "test.h"
#include "network/io/message_meta.h"
#include "network/http/server_http.hpp"
#include "network/http/client_http.hpp"
#include <thread>
void hello(TestMsg& msg)
{
	LOG_INFO("index:{}  msg{}",msg.index(), msg.msg());
}

void Test_Register()
{
    gb::NetworkManager::Instance()->Listen(1, hello);
}

void SendMsg1(std::shared_ptr<gb::Client> client)
{
    TestMsg msg;
    msg.set_index(111);
    msg.set_msg("gb gb gb");

    gb::Meta meta;
    meta.mode = gb::MsgMode::Msg;
    meta.type = 1;
    meta.user_unique_id = 2;
    meta.compress_type = CompressTypeGzip;

    client->Send(gb::CONNECT_TYPE::CT_GATEWAY, &meta, &msg);
}




void SendRpc(std::shared_ptr<gb::Client> client)
{
	gb::RpcCallPtr call = std::make_shared<gb::RpcCall>();
	call->SetSession(client->GetSession(gb::CONNECT_TYPE::CT_GATEWAY));
	call->SetCallBack([](gb::RpcErrorCode err, int a, std::string str) {
		if (err != gb::RpcErrorCode::None) {
			LOG_ERROR("RPC failed: {}", static_cast<int>(err));
			return;
		}
		LOG_INFO("test lua reply: {} {}",a, str);
	});
    gb::NetworkManager::Instance()->Call(call, "test_ret_args", 2, "asadsadsadsdaefasgajf中国人大大撒大苏打 ddbgasufgsajbasadsadsadsdaefasgajf中国人大大撒大苏打 ddbgasufgsajbfasvfafasvfa");
	
}


void http_test(std::shared_ptr<gb::Client> client)
{
    using HttpServer = gb::http::Server<gb::http::HTTP>;
    HttpServer server;
    server.config.port = 18081;
    server.resource["^/hello$"]["GET"] = [](std::shared_ptr<HttpServer::Response> res, std::shared_ptr<HttpServer::Request> req) {
        res->write("hello from local server");
        res->send();
    };
    std::thread server_thread([&server]() { server.start(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    gb::http::Client<gb::http::HTTP> http_client("127.0.0.1:18081");
    auto res = http_client.request("GET", "/hello");
    LOG_INFO("HTTP test status: {} body: {}", res->status_code, res->content.string());

    server.stop();
    server_thread.join();
}

//async_simple::coro::Lazy<> test_coro(gb::SessionPtr& session)
//{
//	gb::RpcCall call;
//	call.SetSession(session);
//	//co_await Net::CoRpcCall<std::string, std::string>(call, "lua_rpc_test_args", "helo");
//	auto str = co_await gb::CoRpcCall<std::string>(call,"lua_rpc_test_args","helo");
//	LOG_INFO("CORO_TEST  {}", str);
//    
//	auto [a, b] = co_await gb::CoRpcCall<std::tuple<int, std::string>>(call, "test_ret_args", 1, "world");
//	LOG_INFO("CORO_TEST_2  {} {}", a,b);
//
//}


//async_simple::coro::Lazy<> test_coro_2(gb::SessionPtr& session)
//{
//	gb::RpcCall call;
//	call.SetSession(session);
//	//co_await Net::CoRpcCall<std::string, std::string>(call, "lua_rpc_test_args", "helo");
//    auto str = co_await gb::CoRpc<int>::execute(call, "hello");
//	LOG_INFO("CORO_TEST  {}", str);
//    
//	auto [a, b] = co_await gb::CoRpcCall<std::tuple<int, std::string>>(call, "test_ret_args", 1, "world");
//	LOG_INFO("CORO_TEST_2  {} {}", a,b);
//
//}
