cmake_minimum_required(VERSION 3.20)

project(DaiBed LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(DAIBED_USE_SYSTEM_RAYLIB "Use an already installed raylib package" ON)

if(DAIBED_USE_SYSTEM_RAYLIB)
    find_package(raylib QUIET)
endif()

if(NOT raylib_FOUND)
    include(FetchContent)
    set(FETCHCONTENT_QUIET FALSE)

    # raylib 5.0 still declares an old CMake compatibility range. CMake 4.x
    # requires this policy floor when configuring older third-party projects.
    if(CMAKE_VERSION VERSION_GREATER_EQUAL 4.0)
        set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "Minimum policy version for third-party CMake projects" FORCE)
    endif()

    FetchContent_Declare(
        raylib
        URL https://github.com/raysan5/raylib/archive/refs/tags/5.0.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(raylib)
endif()

add_executable(DaiBed
    src/main.cpp
    src/AudioSystem.cpp
    src/BotAI.cpp
    src/CameraController.cpp
    src/Block.cpp
    src/CombatSystem.cpp
    src/Core.cpp
    src/Game.cpp
    src/GameBotAI.cpp
    src/GameUI.cpp
    src/GameRules.cpp
    src/Generator.cpp
    src/InputSystem.cpp
    src/Inventory.cpp
    src/Player.cpp
    src/Renderer.cpp
    src/Resource.cpp
    src/Shop.cpp
    src/Team.cpp
    src/World.cpp
    src/Network/Client.cpp
    src/Network/LocalNetworkMock.cpp
    src/Network/Server.cpp
)

target_include_directories(DaiBed PRIVATE src)
target_link_libraries(DaiBed PRIVATE raylib)

if(MSVC)
    target_compile_options(DaiBed PRIVATE /W4 /permissive-)
else()
    target_compile_options(DaiBed PRIVATE -Wall -Wextra -Wpedantic)
endif()
