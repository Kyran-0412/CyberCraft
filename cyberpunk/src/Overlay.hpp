#pragma once

#include <RED4ext/RED4ext.hpp>

#include <d3d12.h>

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

	// Draws Minecraft's blocks (the world layer, hidden behind the game's world by its depth) into the game's HDR scene texture, recording into a_list: a_rtv is a render
	// target view of the scene texture, which the caller has already moved to the render target state. False if there is nothing to draw (no frame yet, or no depth copy).
	// While blocks are drawn into the scene, the draw at Present leaves them out (it draws only the hand, hotbar and screens).
	bool DrawWorldIntoScene(ID3D12GraphicsCommandList* a_list, D3D12_CPU_DESCRIPTOR_HANDLE a_rtv, UINT a_width, UINT a_height);
	void SetSceneGain(float a_percent);
	void SetSceneGlow(float a_percent);  // the brightest pixels (lit whites, glowstone, torch flames) are boosted by up to this much, so that they glow with the game's bloom
	const char* SceneBlockedReason();    // why the last DrawWorldIntoScene drew nothing
	void SetSceneDelay(float a_ms);

	// The game's interface (HUD) over Minecraft's blocks, so that the interface is in front of them: 0 off, 1 on (the captured layer is premultiplied alpha),
	// 2 on (it is straight alpha), 3 show the captured layer alone (transparent parts magenta) to check that it is the right texture. Needs the depth capture.
	void SetUiLayerMode(int a_mode);

	// How the game's interface layer is placed over the blocks: stretched by a_scaleXPercent across and a_scaleYPercent down about the middle of the screen (a_setScale), or shifted by a pixels
	// (not a_setScale). The game seems to stretch the layer a little when it lays it over the picture; /ccdebug uilayer 4 shows a shadow of the layer to line it up.
	void SetUiTransform(float a_scaleXPercent, float a_scaleYPercent, float a_shiftX, float a_shiftY, bool a_setScale);

	// Re-aiming of the blocks at the game's current camera when they are drawn, to hide the time the picture takes to arrive. a_delayMs is
	// how far behind the newest published camera the game's own picture is (what to aim at).
	void SetWarp(bool a_enabled, float a_delayMs);

	// The game's window, once the swapchain has been found (else null).
	void* GameWindow();

	// Puts Present back as it was (game exit).
	void Uninstall();
}
