cmake_minimum_required(VERSION 3.23)
project(CyberCraft LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Link the C++ runtime into the DLL so the game doesn't need a matching Visual C++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(CYBERCRAFT_GAME_DIR "" CACHE PATH "Cyberpunk 2077 folder (the one containing bin and r6). Builds copy the plugin into it.")

include(FetchContent)
FetchContent_Declare(
  RED4ext.SDK
  GIT_REPOSITORY https://github.com/WopsS/RED4ext.SDK.git
  GIT_TAG ad7277714ad30d6885d7050c5ba24fa0102f6920 # July 2026, supports game version 2.31
)
FetchContent_MakeAvailable(RED4ext.SDK)

add_subdirectory(cyberpunk)
