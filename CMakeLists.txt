cmake_minimum_required(VERSION 3.20)

project(DaiBed VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

option(DAIBED_USE_SYSTEM_RAYLIB "Use an already installed raylib package" ON)
option(DAIBED_DEVELOPER_BUILD "Expose automatch and experimental setup in the UI" OFF)
# Real UDP socket transport (Phase 0.1R). This adds NO fetched external
# dependency — it uses only the OS socket API (Winsock2 / BSD sockets), linking
# the system library ws2_32 on Windows. Turn OFF to build without any socket
# code (the transport then compiles to safe stubs). See docs/NETWORK_PREP_PLAN.md.
option(DAIBED_ENABLE_NETWORK "Build the real UDP socket transport (system sockets only; no external download)" ON)

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

    set(DAIBED_RAYLIB_FETCH_ARGS
        URL https://github.com/raysan5/raylib/archive/refs/tags/5.0.tar.gz
    )
    if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.24)
        list(APPEND DAIBED_RAYLIB_FETCH_ARGS DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    endif()
    FetchContent_Declare(raylib ${DAIBED_RAYLIB_FETCH_ARGS})
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(raylib)
endif()

add_executable(DaiBed
    src/main.cpp
    src/AudioSystem.cpp
    src/GameAutomatch.cpp
    src/BotAI.cpp
    src/CameraController.cpp
    src/ChunkRenderer.cpp
    src/Block.cpp
    src/CombatSystem.cpp
    src/Core.cpp
    src/CrashLogger.cpp
    src/Game.cpp
    src/GameBotAI.cpp
    src/GameNetwork.cpp
    src/GameUI.cpp
    src/GameRules.cpp
    src/GamePlacement.cpp
    src/GamePlayerActions.cpp
    src/GameSetup.cpp
    src/GameSettings.cpp
    src/GameWorldTick.cpp
    src/Generator.cpp
    src/Hero.cpp
    src/Hero/HeroAbilities.cpp
    src/HeroSystem.cpp
    src/HeroVisuals.cpp
    src/InputSystem.cpp
    src/Inventory.cpp
    src/MusicSystem.cpp
    src/Player.cpp
    src/ParticleSystem.cpp
    src/Platform/ServerProcess.cpp
    src/PostProcessor.cpp
    src/Renderer.cpp
    src/Resource.cpp
    src/SceneShader.cpp
    src/Shop.cpp
    src/Team.cpp
    src/UiText.cpp
    src/World.cpp
    src/Network/Client.cpp
    src/Network/LocalNetworkMock.cpp
    src/Network/LocalServerSession.cpp
    src/Network/LoopbackTransport.cpp
    src/Network/NetworkProtocol.cpp
    src/Network/Server.cpp
    src/Network/SnapshotDelta.cpp
    src/Network/SnapshotVisibility.cpp
    src/Network/UdpTransport.cpp
    src/Simulation/MatchSimulation.cpp
)

target_include_directories(DaiBed PRIVATE src)
target_link_libraries(DaiBed PRIVATE raylib)
target_compile_definitions(DaiBed PRIVATE
    DAIBED_VERSION="${PROJECT_VERSION}"
    DAIBED_DEVELOPER_BUILD=$<BOOL:${DAIBED_DEVELOPER_BUILD}>
)

# Real network transport: define DAIBED_HAVE_NETWORK and link the OS socket
# library. No FetchContent / no external package is involved.
if(DAIBED_ENABLE_NETWORK)
    target_compile_definitions(DaiBed PRIVATE DAIBED_HAVE_NETWORK=1)
    if(WIN32)
        target_link_libraries(DaiBed PRIVATE ws2_32)
    endif()
    message(STATUS "DaiBed: real UDP transport ENABLED (system sockets)")
else()
    target_compile_definitions(DaiBed PRIVATE DAIBED_HAVE_NETWORK=0)
    message(STATUS "DaiBed: real UDP transport DISABLED (stub transport)")
endif()

if(MSVC)
    target_compile_options(DaiBed PRIVATE /W4 /permissive-)
    if(NOT DAIBED_DEVELOPER_BUILD)
        target_link_options(DaiBed PRIVATE "$<$<CONFIG:Release>:/SUBSYSTEM:WINDOWS>" "$<$<CONFIG:Release>:/ENTRY:mainCRTStartup>")
    endif()
else()
    target_compile_options(DaiBed PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS DaiBed RUNTIME DESTINATION . COMPONENT Runtime)
install(
    DIRECTORY assets
    DESTINATION .
    COMPONENT Runtime
    PATTERN "*.md" EXCLUDE
    PATTERN "*.bbmodel" EXCLUDE
    PATTERN ".gitkeep" EXCLUDE
)

set(CPACK_PACKAGE_NAME "DaiBed")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_FILE_NAME "DaiBed-${PROJECT_VERSION}-windows-x64")
set(CPACK_GENERATOR "ZIP")
set(CPACK_COMPONENTS_ALL Runtime)
set(CPACK_ARCHIVE_COMPONENT_INSTALL ON)
set(CPACK_COMPONENTS_GROUPING ALL_COMPONENTS_IN_ONE)
include(CPack)
