add_library(CyberCraft SHARED src/main.cpp src/Link.cpp src/Link.hpp src/Ground.cpp src/Ground.hpp src/Overlay.cpp src/Overlay.hpp src/Input.cpp src/Input.hpp src/Camera.cpp src/Camera.hpp src/Mapping.cpp src/Mapping.hpp src/Depth.cpp src/Depth.hpp)

target_include_directories(CyberCraft PRIVATE "${CMAKE_SOURCE_DIR}/protocol")
target_link_libraries(CyberCraft PRIVATE RED4ext::SDK d3d12 dxgi d3dcompiler)
target_compile_definitions(CyberCraft PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
set_target_properties(CyberCraft PROPERTIES PREFIX "")

# If a game folder is given, every build copies the plugin straight into it:
#   <game>\red4ext\plugins\CyberCraft\CyberCraft.dll
if(CYBERCRAFT_GAME_DIR)
  set(CYBERCRAFT_PLUGIN_DIR "${CYBERCRAFT_GAME_DIR}/red4ext/plugins/CyberCraft")
  add_custom_command(
    TARGET CyberCraft POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CYBERCRAFT_PLUGIN_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:CyberCraft>" "${CYBERCRAFT_PLUGIN_DIR}/"
    COMMENT "Copying CyberCraft.dll to ${CYBERCRAFT_PLUGIN_DIR}"
  )
endif()
