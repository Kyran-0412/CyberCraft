#pragma once

#include <RED4ext/RED4ext.hpp>

namespace cybercraft::camera
{
	void Init(RED4ext::v1::PluginHandle a_handle, const RED4ext::v1::Sdk* a_sdk);

	// Call every frame while V exists. Reads the game's active camera (position, direction, field of view) and
	// publishes it to shared memory, so Minecraft can draw its blocks through the same camera. For the first few
	// seconds it also logs what it sees, including a check of the field of view against the game's own
	// world-to-screen function.
	void Update(RED4ext::Handle<RED4ext::IScriptable>& a_player);

	// Which camera source to publish (CamSource in the protocol). Set from what Minecraft asks for.
	void SetSource(int a_source);

	// Forget everything cached from the old world (loading screen, main menu).
	void Reset();
}
