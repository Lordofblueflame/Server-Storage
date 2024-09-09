workspace "ServerStorage"
    configurations { "Debug", "Release" }
    platforms { "x64" }

project "ServerStorage"
    kind "ConsoleApp"
    language "C++"
    targetdir ("../bin/%{cfg.buildcfg}-%{cfg.platform}")

    pchheader "include/pch.h"
    pchsource "src/pch.cpp"

    files { 
        "src/**.cpp", 
        "include/**.h", 
        "src/**/**.cpp",
        "include/**/**.h",
    }

    local boostRoot = os.getenv("BOOST_ROOT")
    local boostLibDir = boostRoot .. "/stage/lib"

    includedirs { "include", boostRoot }
    libdirs { "lib", boostLibDir, boostRoot }

    filter "configurations:Debug"
        defines { "DEBUG" }
        symbols "On"
        buildoptions { "-std=c++17" }
        links {
            "ws2_32",
            "wsock32",
            "boost_filesystem-mgw13-mt-x64-1_86",
            "boost_log_setup-mgw13-mt-x64-1_86",
            "boost_log-mgw13-mt-x64-1_86",
            "boost_wserialization-mgw13-mt-x64-1_86",
            "boost_thread-mgw13-mt-x64-1_86",
            "boost_system-mgw13-mt-x64-1_86",
            "boost_serialization-mgw13-mt-x64-1_86",
            "boost_chrono-mgw13-mt-x64-1_86",
            "boost_atomic-mgw13-mt-x64-1_86"
        }

    filter "configurations:Release"
        defines { "NDEBUG" }
        optimize "On"
        buildoptions { "-std=c++17" }
        links {
            "ws2_32",
            "wsock32",
            "boost_filesystem-mgw13-mt-x64-1_86",
            "boost_log_setup-mgw13-mt-x64-1_86",
            "boost_log-mgw13-mt-x64-1_86",
            "boost_wserialization-mgw13-mt-x64-1_86",
            "boost_thread-mgw13-mt-x64-1_86",
            "boost_system-mgw13-mt-x64-1_86",
            "boost_serialization-mgw13-mt-x64-1_86",
            "boost_chrono-mgw13-mt-x64-1_86",
            "boost_atomic-mgw13-mt-x64-1_86"
        }

    filter "platforms:x64"
        architecture "x86_64"

    defines { "BOOST_LOG_DYN_LINK"}
    defines { "BOOST_ASIO_HEADER_ONLY" }

