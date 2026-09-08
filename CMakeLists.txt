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
# All --*-smoke self-tests (src/Diag/Smokes.cpp). ON for developer/CI builds;
# turn OFF for shipping so no test logic is compiled into the binary (the CLI
# flags then answer with a "diagnostics disabled" stub).
option(DAIBED_DIAGNOSTICS "Compile the --*-smoke diagnostic self-tests" ON)
# Real UDP socket transport (Phase 0.1R). This adds NO fetched external
# dependency — it uses only the OS socket API (Winsock2 / BSD sockets), linking
# the system library ws2_32 on Windows. Turn OFF to build without any socket
# code (the transport then compiles to safe stubs). See docs/P2P_IMPLEMENTATION.md.
option(DAIBED_ENABLE_NETWORK "Build the real UDP socket transport (system sockets only; no external download)" ON)
set(DAIBED_LOCAL_STEAMWORKS_SDK "${CMAKE_SOURCE_DIR}/sdk")
set(DAIBED_STEAMWORKS_DEFAULT OFF)
if(EXISTS "${DAIBED_LOCAL_STEAMWORKS_SDK}/public/steam/steam_api.h")
    set(DAIBED_STEAMWORKS_DEFAULT ON)
endif()
option(DAIBED_ENABLE_STEAMWORKS
    "Include Steam P2P alongside UDP in the universal player release"
    ${DAIBED_STEAMWORKS_DEFAULT})
set(DAIBED_STEAMWORKS_SDK_ROOT "" CACHE PATH
    "Steamworks SDK root (the directory containing public/steam/steam_api.h)")
if(DAIBED_ENABLE_STEAMWORKS
    AND NOT DAIBED_STEAMWORKS_SDK_ROOT
    AND EXISTS "${DAIBED_LOCAL_STEAMWORKS_SDK}/public/steam/steam_api.h")
    set(DAIBED_STEAMWORKS_SDK_ROOT "${DAIBED_LOCAL_STEAMWORKS_SDK}" CACHE PATH
        "Steamworks SDK root (the directory containing public/steam/steam_api.h)" FORCE)
endif()
set(DAIBED_STEAM_APP_ID "480" CACHE STRING
    "Steam App ID used by local developer builds (480 is Valve Spacewar)")

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
    src/BotTuningTrainer.cpp
    src/BotAI.cpp
    src/BotCombatAssessment.cpp
    src/BotStrategicPolicy.cpp
    src/Navigation/BotNavigationGoal.cpp
    src/CameraController.cpp
    src/ChunkRenderer.cpp
    src/Block.cpp
    src/CombatSystem.cpp
    src/Core.cpp
    src/CrashLogger.cpp
    src/CreativeMap.cpp
    src/Diag/Smokes.cpp
    src/Diag/NavigationSmokes.cpp
    src/FirstPersonMotion.cpp
    src/Game.cpp
    src/GameBotAI.cpp
    src/GameNavigation.cpp
    src/GameCreative.cpp
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
    src/Navigation/BotControlArbiter.cpp
    src/Navigation/NavigationActionLibrary.cpp
    src/Navigation/NavigationController.cpp
    src/Navigation/NavigationGoal.cpp
    src/Navigation/NavigationProfile.cpp
    src/Navigation/NavigationWorldView.cpp
    src/Navigation/PathExecutor.cpp
    src/Navigation/RouteGraph.cpp
    src/Navigation/ThreatMap.cpp
    src/Navigation/VoxelPathfinder.cpp
    src/Player.cpp
    src/ParticleSystem.cpp
    src/Platform/PreciseTimer.cpp
    src/Platform/ServerProcess.cpp
    src/Platform/SteamLobbyService.cpp
    src/Platform/SteamRuntime.cpp
    src/PostProcessor.cpp
    src/Renderer.cpp
    src/Resource.cpp
    src/SceneShader.cpp
    src/Shop.cpp
    src/Team.cpp
    src/UiText.cpp
    src/World.cpp
    src/Network/Client.cpp
    src/Network/InProcessDatagramBackend.cpp
    src/Network/LocalNetworkMock.cpp
    src/Network/LocalServerSession.cpp
    src/Network/LoopbackTransport.cpp
    src/Network/NetworkProtocol.cpp
    src/Network/Server.cpp
    src/Network/SnapshotDelta.cpp
    src/Network/SnapshotVisibility.cpp
    src/Network/SteamDatagramBackend.cpp
    src/Network/UdpDatagramBackend.cpp
    src/Network/UdpTransport.cpp
    src/Simulation/MatchSimulation.cpp
)

target_include_directories(DaiBed PRIVATE src)
target_link_libraries(DaiBed PRIVATE raylib)
target_compile_definitions(DaiBed PRIVATE
    DAIBED_VERSION="${PROJECT_VERSION}"
    DAIBED_DEVELOPER_BUILD=$<BOOL:${DAIBED_DEVELOPER_BUILD}>
    DAIBED_DIAGNOSTICS=$<BOOL:${DAIBED_DIAGNOSTICS}>
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

# A universal player build contains both UDP and Steam P2P. If a local
# Steamworks SDK is available at sdk/, Steam support is enabled automatically;
# it may still be disabled explicitly for dependency-free CI/tooling builds.
# Valve distributes the SDK through the partner portal, so CMake cannot fetch
# it as a public dependency.
if(DAIBED_ENABLE_STEAMWORKS)
    if(NOT DAIBED_STEAMWORKS_SDK_ROOT)
        message(FATAL_ERROR
            "DAIBED_ENABLE_STEAMWORKS=ON requires DAIBED_STEAMWORKS_SDK_ROOT")
    endif()

    set(DAIBED_STEAM_PUBLIC_DIR "${DAIBED_STEAMWORKS_SDK_ROOT}/public")
    if(NOT EXISTS "${DAIBED_STEAM_PUBLIC_DIR}/steam/steam_api.h")
        message(FATAL_ERROR
            "Steamworks header not found: ${DAIBED_STEAM_PUBLIC_DIR}/steam/steam_api.h")
    endif()

    if(WIN32)
        if(CMAKE_SIZEOF_VOID_P EQUAL 8)
            set(DAIBED_STEAM_LIB
                "${DAIBED_STEAMWORKS_SDK_ROOT}/redistributable_bin/win64/steam_api64.lib")
            set(DAIBED_STEAM_RUNTIME
                "${DAIBED_STEAMWORKS_SDK_ROOT}/redistributable_bin/win64/steam_api64.dll")
        else()
            set(DAIBED_STEAM_LIB
                "${DAIBED_STEAMWORKS_SDK_ROOT}/redistributable_bin/steam_api.lib")
            set(DAIBED_STEAM_RUNTIME
                "${DAIBED_STEAMWORKS_SDK_ROOT}/redistributable_bin/steam_api.dll")
        endif()
    elseif(APPLE)
        set(DAIBED_STEAM_LIB
            "${DAIBED_STEAMWORKS_SDK_ROOT}/redistributable_bin/osx/libsteam_api.dylib")
        set(DAIBED_STEAM_RUNTIME "${DAIBED_STEAM_LIB}")
    else()
        set(DAIBED_STEAM_LIB
            "${DAIBED_STEAMWORKS_SDK_ROOT}/redistributable_bin/linux64/libsteam_api.so")
        set(DAIBED_STEAM_RUNTIME "${DAIBED_STEAM_LIB}")
    endif()

    if(NOT EXISTS "${DAIBED_STEAM_LIB}")
        message(FATAL_ERROR "Steamworks import/shared library not found: ${DAIBED_STEAM_LIB}")
    endif()
    target_include_directories(DaiBed PRIVATE "${DAIBED_STEAM_PUBLIC_DIR}")
    target_link_libraries(DaiBed PRIVATE "${DAIBED_STEAM_LIB}")
    target_compile_definitions(DaiBed PRIVATE DAIBED_HAVE_STEAMWORKS=1)
    add_custom_command(TARGET DaiBed POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${DAIBED_STEAM_RUNTIME}" "$<TARGET_FILE_DIR:DaiBed>"
        COMMENT "Copying Steamworks runtime"
    )
    # Local/debug launch hint. It is generated only in the build tree and is
    # intentionally not installed into a shipping Steam depot.
    file(GENERATE
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/steam_appid.txt"
        CONTENT "${DAIBED_STEAM_APP_ID}")
    message(STATUS "DaiBed: Steam Networking Sockets ENABLED")
else()
    target_compile_definitions(DaiBed PRIVATE DAIBED_HAVE_STEAMWORKS=0)
    message(STATUS "DaiBed: Steam Networking Sockets DISABLED (stub backend)")
endif()

if(MSVC)
    target_compile_options(DaiBed PRIVATE /W4 /permissive-)
    # A release build is launched as a game, including developer-flavoured
    # releases.  Keep diagnostics callable through command-line arguments, but
    # do not create a second console window beside the game window.
    target_link_options(DaiBed PRIVATE "$<$<CONFIG:Release>:/SUBSYSTEM:WINDOWS>" "$<$<CONFIG:Release>:/ENTRY:mainCRTStartup>")
else()
    target_compile_options(DaiBed PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS DaiBed RUNTIME DESTINATION . COMPONENT Runtime)
if(DAIBED_ENABLE_STEAMWORKS)
    install(FILES "${DAIBED_STEAM_RUNTIME}" DESTINATION . COMPONENT Runtime)
endif()

# A developer normally starts the executable directly from build/<config>.
# Keep that directory self-contained as well as the installed package: running
# DaiBed.exe by double-click otherwise uses the build directory as its working
# directory and misses newly added sounds, shaders and Creative maps.
add_custom_command(TARGET DaiBed POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/assets" "$<TARGET_FILE_DIR:DaiBed>/assets"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/maps" "$<TARGET_FILE_DIR:DaiBed>/maps"
    COMMENT "Synchronizing DaiBed runtime assets and maps"
)

install(
    DIRECTORY assets
    DESTINATION .
    COMPONENT Runtime
    PATTERN "*.md" EXCLUDE
    PATTERN "*.bbmodel" EXCLUDE
    PATTERN ".gitkeep" EXCLUDE
)

# Built-in CreativeMapDocument maps are runtime data, just like textures and
# sounds. Keep the maps/ directory layout in installed packages so
# --automatch --map works outside the repository root.
install(
    DIRECTORY maps
    DESTINATION .
    COMPONENT Runtime
)

set(CPACK_PACKAGE_NAME "DaiBed")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_FILE_NAME "DaiBed-${PROJECT_VERSION}-windows-x64")
set(CPACK_GENERATOR "ZIP")
set(CPACK_COMPONENTS_ALL Runtime)
set(CPACK_ARCHIVE_COMPONENT_INSTALL ON)
set(CPACK_COMPONENTS_GROUPING ALL_COMPONENTS_IN_ONE)
include(CPack)
