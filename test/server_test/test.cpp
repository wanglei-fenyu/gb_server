#include "test.h"
#include "worker/worker_manager.h"
#include "network/io/message_meta.h"
void hello(const std::shared_ptr<gb::Session>& session)
{
    LOG_INFO("Hello");
}

void World(const std::shared_ptr<gb::Session>& session,TestMsg& msg)
{
	gb::Meta meta;
	meta.entity_id = 2;
	meta.type = 11;
	session->Send(&meta,&msg);
    LOG_INFO("wrold {}", msg.msg());
}

void test_rpc(gb::RpcReply reply)
{
    LOG_INFO("test_rpc");
    reply.Invoke();
}



void test_rpc2(int a)
{
    LOG_INFO("test_rpc {}", a);
}


void square(gb::RpcReply reply, int a)
{
    LOG_INFO("square {}", a);
	reply.Invoke(a*a);
}

void test_ret_args(gb::RpcReply reply, int a, std::string c)
{
    LOG_INFO("{}:{}", a, c);
    reply.GetMeta().compress_type = (CompressType)CompressTypeNone;
    reply.Invoke(a*2, c + " hello");
}

void SessionMsg(const gb::SessionPtr& session,TestMsg& msg)
{
    LOG_INFO("Session");
}


void Test_Register()
{
	gb::NetworkManager::Instance()->Register("test_rpc", test_rpc);
	gb::NetworkManager::Instance()->Register("test_rpc2", test_rpc2);
	gb::NetworkManager::Instance()->Register("square", square);
	gb::NetworkManager::Instance()->Register<void(gb::RpcReply, int, std::string)>("test_ret_args", test_ret_args);
}
