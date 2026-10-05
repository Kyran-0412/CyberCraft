#pragma once

#include "Link.hpp"

#include <RED4ext/RED4ext.hpp>

#include <Windows.h>

namespace cybercraft::input
{
	void Init(RED4ext::v1::PluginHandle a_handle, const RED4ext::v1::Sdk* a_sdk);

	// Replaces the game window's message handler with one that can capture the keyboard and mouse. Safe to call
	// every frame: it does nothing once installed (or once it has given up).
	void Install(HWND a_window);
	void Uninstall();

	// Call every frame from the game's main thread. a_followOn: Minecraft is asking V to follow its player.
	// a_playerHere: V exists. Works out whether the keyboard and mouse should go to Minecraft, and, if they do,
	// turns mouse movement into a look direction or a cursor.
	void Update(bool a_followOn, bool a_playerHere, const Link::McSnapshot& a_mc);

	// Are the keyboard and mouse going to Minecraft right now?
	bool Routing();

	// Where the player is looking (Minecraft degrees). Only meaningful while Routing().
	float LookYaw();
	float LookPitch();

	// The virtual cursor used while a Minecraft screen is open, in the HUD's own pixels. Only visible while Routing().
	bool CursorVisible();
	void CursorPosition(float& a_x, float& a_y);
}
