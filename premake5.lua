workspace "ServerStorage"
    configurations { "Debug", "Release" }
    platforms { "x64" }

project "ServerStorage"
    kind "ConsoleApp"
    language "C++"
    targetdir ("bin/%{cfg.buildcfg}-%{cfg.platform}")

    pchheader "include/pch.h"
    pchsource "src/pch.cpp"

    defines { "BOOST_ASIO_HEADER_ONLY" }

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

    filter "configurations:Release"
        defines { "NDEBUG" }
        optimize "On"
        buildoptions { "-std=c++17" }

    filter "platforms:x64"
        architecture "x86_64"
        links {
            "boost_filesystem-mgw13-mt-x64-1_85",
            "boost_log_setup-mgw13-mt-x64-1_85",
            "boost_log-mgw13-mt-x64-1_85",
            "boost_wserialization-mgw13-mt-x64-1_85",
            "boost_thread-mgw13-mt-x64-1_85",
            "boost_system-mgw13-mt-x64-1_85",
            "boost_serialization-mgw13-mt-x64-1_85", 
            "boost_chrono-mgw13-mt-x64-1_85",
            "boost_atomic-mgw13-mt-x64-1_85",
            "ws2_32"
        }

    defines { "BOOST_LOG_DYN_LINK" }
    filter "files:src/**.cpp"
