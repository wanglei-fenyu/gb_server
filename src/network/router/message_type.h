#pragma once
#include <cstdint>

/*
	本文件只定义消息类型
*/
namespace gb
{
enum MessageType : uint32_t
{
    MT_Begin = 0, // 消息开始

	// 普通消息
    MT_Login				 = 0,
    MT_EnterScene			 = 1,

	// AI模块
    MT_AI_Run				 = 10000,
    MT_AI_Skill				 = 10001,

	// 寻路模块
	MT_AStar				= 20000,
	MT_LineFindPath			= 20001,

	MT_End,

};

}