#pragma once

#include <RED4ext/RED4ext.hpp>

namespace cybercraft::overlay
{
	// Called once from Main with the RED4ext handles, so this file can write to the log.
	void Init(RED4ext::v1::PluginHandle a_handle, const RED4ext::v1::Sdk* a_sdk);

	// Call from the game's main thread once the game is running (it is cheap to call every frame). The first
	// time, finds the game's swapchain and hooks its Present, so Minecraft's HUD can be drawn over each frame.
	void Install();

	// A debug view of the depth test (0 off): 1 shows the game's depth as grey (near dark, far light) over the whole screen, 2 shows the
	// blocks' distance as grey, 3 colours block pixels red where the game's world hides them and green where they show.
	void SetDebugView(int a_view);

	// The game's window, once the swapchain has been found (else null).
	void* GameWindow();

	// Puts Present back as it was (game exit).
	void Uninstall();
}
