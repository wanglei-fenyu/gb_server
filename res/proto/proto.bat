set SRC_PROTO=*.proto
"../../tools/protoc.exe" --proto_path=. --cpp_out=../../protobuf/ %SRC_PROTO%