#pragma once

/// PostgreSQL 选项测试 (独立工程, 不依赖 App/Worker/Lua)
/// 每个测试创建自己的 PgConnection, 运行完后销毁。
/// 数据保留在数据库中，方便后续检查。
int MenuTestPgConnectClose();
int MenuTestPgQuery();
int MenuTestPgExecute();
int MenuTestPgTransaction();
int MenuTestPgSubquery();
int MenuTestPgJoin();
