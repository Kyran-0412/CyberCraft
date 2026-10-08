#pragma once

#include <RED4ext/RED4ext.hpp>

namespace cybercraft::collision
{
	void Init(RED4ext::v1::PluginHandle a_handle, const RED4ext::v1::Sdk* a_sdk);

	// A debug command from Minecraft (/ccdebug): see proto::DebugAction.
	void Command(int a_action, double a_arg);

	// Call every frame while V exists: keeps the collision boxes in Night City matching the list Minecraft publishes.
	void Update();

	// The game has no player right now (loading screen, main menu): forget every entity made, they are gone with the world.
	void Reset();
}
