from conan import ConanFile
from conan.tools.cmake import cmake_layout
from conan.tools.files import copy as conan_copy

class GBServer(ConanFile):
    name = "gbserver"
    version = "0.1.0"

    generators = "CMakeToolchain","CMakeDeps"
    settings = "os", "compiler", "build_type", "arch"

    def layout(self):
        cmake_layout(self)

    def requirements(self):
        self.requires("spdlog/1.15.0")
        self.requires("asio/1.38.2")
        self.requires("protobuf/6.33.5")
        self.requires("grpc/1.82.0")
        self.requires("openssl/3.0.13")        
        self.requires("zlib/1.3.1")           
        self.requires("async_simple/1.4")
        self.requires("concurrentqueue/1.0.5")
        self.requires("lua/5.4.6")
        self.requires("mimalloc/2.1.2")
        self.requires("glaze/7.8.4")
        self.requires("pugixml/1.16")
        self.requires("sol2/3.5.0")
        self.requires("cxxopts/3.3.1")
        self.requires("hiredis/1.3.0")
        self.requires("libpq/17.7")
        self.requires("catch2/3.15.0")
        self.requires("cnats/3.12.0")
        self.requires("kcp/2.1.1")
        self.requires("etcd-cpp-apiv3/0.15.4")

    def build_requirements(self):
        self.tool_requires("protobuf/6.33.5")

    def generate(self):
        protobuf = self.dependencies["protobuf"]
        if protobuf:
            conan_copy(self, "protoc", dst=self.source_folder + "/tools", src=protobuf.package_folder + "/bin")
            conan_copy(self, "protoc.exe", dst=self.source_folder + "/tools", src=protobuf.package_folder + "/bin")