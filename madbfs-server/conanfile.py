from conan import ConanFile
from conan.errors import ConanException
from conan.tools.cmake import cmake_layout

# https://developer.android.com/ndk/guides/abis
SUPPORTED_ARCH = ["armv7", "armv8", "x86", "x86_64"]


class Recipe(ConanFile):
    settings = ["os", "compiler", "build_type", "arch"]
    generators = ["CMakeToolchain", "CMakeDeps"]
    requires = [
        "asio/1.34.2",
        "fmt/11.1.3",
        "spdlog/1.15.1",
    ]

    def layout(self):
        arch = self.settings.get_safe("arch")
        if arch is None or arch not in SUPPORTED_ARCH:
            raise ConanException(
                f"arch should be defined with one of these values: {SUPPORTED_ARCH}"
            )
        self.folders.build_folder_vars = [
            "settings.os",
            "settings.arch",
            "settings.build_type",
        ]
        cmake_layout(self)
