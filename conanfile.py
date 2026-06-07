from conan import ConanFile
from conan.tools.cmake import cmake_layout

class GBServer(ConanFile):
    name = "gbserver"
    version = "0.1.0"

    generators = "CMakeToolchain","CMakeDeps"
    settings = "os", "compiler", "build_type", "arch"

    def layout(self):
        cmake_layout(self)

    def requirements(self):
        # 其他依赖...
        self.requires("gbnet/0.1.0")
        self.requires("spdlog/1.15.0")
        self.requires("boost/1.90.0")         
        self.requires("protobuf/3.21.12")     
        self.requires("openssl/3.0.13")        
        self.requires("zlib/1.3.1")           
        self.requires("async_simple/1.4")
        self.requires("concurrentqueue/1.0.5")
        self.requires("lua/5.4.6")
        self.requires("mimalloc/2.1.2")
        self.requires("rapidjson/1.1.0")
        self.requires("rapidxml/1.13")
        self.requires("sol2/3.5.0")
        self.requires("cxxopts/3.3.1")
        self.requires("libpq/17.7")
        self.requires("catch2/3.15.0")
        self.requires("cnats/3.12.0")

