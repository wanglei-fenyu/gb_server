#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import sys
import glob
import os
import subprocess
import tempfile
from google.protobuf import descriptor_pb2
from google.protobuf.descriptor import FieldDescriptor


def get_protoc_path():
    """
    根据操作系统返回正确的 protoc 可执行文件路径。
    默认假定 protoc 位于脚本相对路径 ../../tools/ 下。
    """
    base_dir = "../../tools/"
    if sys.platform == "win32":
        return base_dir + "protoc.exe"
    else:
        return base_dir + "protoc"


def generate_descriptor_set(proto_files):
    """
    调用 protoc 为指定的 .proto 文件生成描述符集（FileDescriptorSet），
    返回解析后的 FileDescriptorSet 对象。
    """
    if not proto_files:
        return None

    with tempfile.NamedTemporaryFile(suffix='.pb', delete=False) as tmp:
        tmp_path = tmp.name

    try:
        protoc = get_protoc_path()
        cmd = [protoc, f'--descriptor_set_out={tmp_path}', '--proto_path=.'] + proto_files
        result = subprocess.run(cmd, capture_output=True, text=True)
        if result.returncode != 0:
            print(f"Error running protoc: {result.stderr}")
            sys.exit(1)

        with open(tmp_path, 'rb') as f:
            data = f.read()
        file_desc_set = descriptor_pb2.FileDescriptorSet()
        file_desc_set.ParseFromString(data)
        return file_desc_set
    finally:
        if os.path.exists(tmp_path):
            os.remove(tmp_path)


def get_header_filenames_from_descriptors(file_descs):
    """从文件描述符列表中提取每个 proto 对应的头文件名（.pb.h）"""
    header_files = []
    for file_desc in file_descs:
        proto_file_name = file_desc.name
        header_file_name = proto_file_name.replace('.proto', '.pb.h')
        header_files.append(header_file_name)
    return header_files


def generate_sol2_code(file_descs):
    """
    遍历文件描述符，生成 Sol2 注册代码。
    每个消息/枚举的 Lua 名称都会包含其 package 路径（如 "pkg.Message"），
    C++ 类型名也会加上命名空间（如 "pkg::Message"）。
    """
    vContents = []

    # 辅助函数：构造 Lua 注册名（点分隔）和 C++ 类型名（双冒号分隔）
    def build_names(package, parts):
        """
        parts: 一个列表，从外层到内层的名字，如 ['Outer', 'Inner']
        返回 (lua_name, cpp_name)
        """
        full_lua = ".".join([package] + parts) if package else ".".join(parts)
        full_cpp = "::".join([package] + parts) if package else "::".join(parts)
        return full_lua, full_cpp

    # 递归注册枚举
    def register_enum(enum_desc, package, prefix_parts, cpp_scope_prefix):
        lua_name, cpp_name = build_names(package, prefix_parts + [enum_desc.name])
        content = f'scriptPtr->new_enum("{lua_name}",\n'
        for value in enum_desc.value:
            content += f'    "{value.name}", {cpp_name}::{value.name},\n'
        content = content.rstrip(',\n') + '\n);\n\n'
        vContents.append(content)

    # 递归注册消息
    def register_message(message_desc, package, prefix_parts, cpp_scope_prefix):
        # 先处理内部枚举
        for enum_desc in message_desc.enum_type:
            register_enum(enum_desc, package, prefix_parts + [message_desc.name],
                          cpp_scope_prefix + "::" + message_desc.name)

        lua_name, cpp_name = build_names(package, prefix_parts + [message_desc.name])

        content = f'scriptPtr->new_usertype<{cpp_name}>("{lua_name}"\n\t,sol::base_classes\n\t,sol::bases<google::protobuf::Message>()\n\t,sol::call_constructor\n\t,[](google::protobuf::Message* o)->{cpp_name}*{{ return static_cast<{cpp_name}*>(o); }}\n\t'

        # 处理字段
        for field in message_desc.field:
            key = field.name.lower()

            # 处理字段类型，获取正确的 C++ 类型名称（带命名空间）
            if field.type == FieldDescriptor.TYPE_MESSAGE:
                type_name = field.type_name.lstrip('.').replace('.', '::')
            elif field.type == FieldDescriptor.TYPE_ENUM:
                type_name = field.type_name.lstrip('.').replace('.', '::')
            else:
                type_mapping = {
                    FieldDescriptor.TYPE_DOUBLE: "double",
                    FieldDescriptor.TYPE_FLOAT: "float",
                    FieldDescriptor.TYPE_INT64: "int64_t",
                    FieldDescriptor.TYPE_UINT64: "uint64_t",
                    FieldDescriptor.TYPE_INT32: "int32_t",
                    FieldDescriptor.TYPE_FIXED64: "uint64_t",
                    FieldDescriptor.TYPE_FIXED32: "uint32_t",
                    FieldDescriptor.TYPE_BOOL: "bool",
                    FieldDescriptor.TYPE_UINT32: "uint32_t",
                    FieldDescriptor.TYPE_ENUM: "int",
                    FieldDescriptor.TYPE_SFIXED32: "int32_t",
                    FieldDescriptor.TYPE_SFIXED64: "int64_t",
                    FieldDescriptor.TYPE_SINT32: "int32_t",
                    FieldDescriptor.TYPE_SINT64: "int64_t",
                }
                type_name = type_mapping.get(field.type, "unknown")

            if field.label == FieldDescriptor.LABEL_REPEATED:
                if field.type == FieldDescriptor.TYPE_MESSAGE:
                    content += f',"{key}_size",&{cpp_name}::{key}_size\n\t'
                    content += f',"clear_{key}",&{cpp_name}::clear_{key}\n\t'
                    content += f',"add_{key}",&{cpp_name}::add_{key}\n\t'
                    content += f',"mutable_{key}",[]({cpp_name}& o, int index)->{type_name}* {{ return o.mutable_{key}(index); }}\n\t'
                    content += f',"{key}",[](const {cpp_name}& o, int index)->const {type_name}& {{ return o.{key}(index); }}\n\t'
                elif field.type == FieldDescriptor.TYPE_STRING:
                    content += f',"{key}_size",&{cpp_name}::{key}_size\n\t'
                    content += f',"clear_{key}",&{cpp_name}::clear_{key}\n\t'
                    content += f',"add_{key}",[]({cpp_name}& o, int index, const char* str)->void {{ o.add_{key}(str); }}\n\t'
                    content += f',"{key}",[](const {cpp_name}& o, int index)->const std::string& {{ return o.{key}(index); }}\n\t'
                else:
                    content += f',"{key}_size",&{cpp_name}::{key}_size\n\t'
                    content += f',"clear_{key}",&{cpp_name}::clear_{key}\n\t'
                    content += f',"{key}",[](const {cpp_name}& o, int index)->{type_name} {{ return o.{key}(index); }}\n\t'
                    content += f',"set_{key}",&{cpp_name}::set_{key}\n\t'
                    content += f',"add_{key}",&{cpp_name}::add_{key}\n\t'
            else:
                if field.type == FieldDescriptor.TYPE_MESSAGE:
                    content += f',"has_{key}",&{cpp_name}::has_{key}\n\t'
                    content += f',"clear_{key}",&{cpp_name}::clear_{key}\n\t'
                    content += f',"{key}",&{cpp_name}::{key}\n\t'
                    content += f',"release_{key}",&{cpp_name}::release_{key}\n\t'
                    content += f',"mutable_{key}",&{cpp_name}::mutable_{key}\n\t'
                    content += f',"set_allocated_{key}",&{cpp_name}::set_allocated_{key}\n\t'
                elif field.type == FieldDescriptor.TYPE_STRING:
                    content += f',"clear_{key}",&{cpp_name}::clear_{key}\n\t'
                    content += f',"{key}",&{cpp_name}::{key}\n\t'
                    content += f',"set_{key}",[]({cpp_name}& o, const char* str)->void {{ o.set_{key}(str); }}\n\t'
                else:
                    content += f',"clear_{key}",&{cpp_name}::clear_{key}\n\t'
                    content += f',"{key}",&{cpp_name}::{key}\n\t'
                    content += f',"set_{key}",&{cpp_name}::set_{key}\n\t'

        content += ");\n\n"
        vContents.append(content)

        # 递归处理内嵌消息
        for nested_type in message_desc.nested_type:
            register_message(nested_type, package, prefix_parts + [message_desc.name],
                             cpp_scope_prefix + "::" + message_desc.name)

    # 遍历所有文件描述符，处理顶层消息和枚举
    for file_desc in file_descs:
        pkg = file_desc.package
        for enum_type in file_desc.enum_type:
            register_enum(enum_type, pkg, [], pkg if pkg else "")
        for message_type in file_desc.message_type:
            register_message(message_type, pkg, [], pkg if pkg else "")

    return vContents


def main():
    proto_files = glob.glob("*.proto")
    if not proto_files:
        print("No .proto files found in current directory.")
        sys.exit(1)

    file_desc_set = generate_descriptor_set(proto_files)
    if not file_desc_set:
        print("Failed to generate descriptor set.")
        sys.exit(1)

    file_descs = file_desc_set.file
    if not file_descs:
        print("No file descriptors found.")
        sys.exit(1)

    header_files = get_header_filenames_from_descriptors(file_descs)
    sol2_code = generate_sol2_code(file_descs)

    output_file = '../../src/script/register_proto_msg.cpp'
    with open(output_file, 'w') as f:
        f.write('#include "script.h"\n')
        for head_file in header_files:
            f.write(f'#include "protobuf/{head_file}"\n')
        f.write('\nvoid register_proto_msg(std::shared_ptr<Script>& scriptPtr) {\n')
        for line in sol2_code:
            f.write(line)
        f.write('}\n')

    print(f'Sol2 registration code generated in {output_file}')


if __name__ == '__main__':
    main()