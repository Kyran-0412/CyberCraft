#pragma once

#include <RED4ext/RED4ext.hpp>

namespace cybercraft::overlay
{
	// Called once from Main with the RED4ext handles, so this file can write to the log.
	void Init(RED4ext::v1::PluginHandle a_handle, const RED4ext::v1::Sdk* a_sdk);

	// Call from the game's main thread once the game is running (it is cheap to call every frame). The first
	// time, finds the game's swapchain and hooks its Present, so Minecraft's HUD can be drawn over each frame.
	void Install();

	// The game's window, once the swapchain has been found (else null).
	void* GameWindow();

	// Puts Present back as it was (game exit).
	void Uninstall();
}
