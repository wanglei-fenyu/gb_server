#!/bin/bash

# 定义源文件模式
SRC_PROTO="*.proto"

# 生成描述符文件
../../tools/protoc --proto_path=. --descriptor_set_out=descriptors.pb $SRC_PROTO

# 生成 C++ 代码到 ../../protobuf/ 目录
../../tools/protoc --proto_path=. --cpp_out=../../protobuf/ $SRC_PROTO
