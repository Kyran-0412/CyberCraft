#pragma once

#include <RED4ext/RED4ext.hpp>

#include <d3d12.h>

namespace cybercraft::inscene
{
	void Init(RED4ext::v1::PluginHandle a_handle, const RED4ext::v1::Sdk* a_sdk);

	// The size of the screen (the back buffer): the game's scene texture is this size (DLSS off).
	void SetScreenSize(UINT a_width, UINT a_height);

	// Drawing into the game's own HDR scene (see InScene.cpp): 0 off, 1 to 3 a dim, bright and very bright square in the middle of the screen (the proof of concept), 4 Minecraft's blocks.
	void SetMode(int a_mode);

	// Starts drawing Minecraft's blocks into the scene (mode 4) unless the player has chosen a mode with /ccdebug scene: called when Minecraft's first frame arrives.
	void EnableByDefault();

	// Writes to the log, for the next frame, everything that happens to the scene texture (and the depth and fog textures), list by list in the order the GPU runs them.
	void RequestTimeline();

	// The G-buffer probe: read-only; writes to the log every texture that could be a render target, compute output or depth buffer (with its format, size, name if the engine gave one and its
	// state changes) for one frame, in the order the GPU runs them, and which of them are moved into the render target together.
	void RequestGbufProbe();

	// When a frame has no scene copy (the game only makes one when something in view needs refraction), insert before the list in which post-processing starts reading the scene (on by default).
	void SetFallback(bool a_on);

	// Called from the swapchain's Present hook every frame (cheap when off): installs the hooks the first time, and says in the log if the scene is never found.
	void OnPresent();
}
