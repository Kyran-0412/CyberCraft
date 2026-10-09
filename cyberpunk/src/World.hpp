#pragma once

#include <RED4ext/RED4ext.hpp>

namespace cybercraft::world
{
	void Init(RED4ext::v1::PluginHandle a_handle, const RED4ext::v1::Sdk* a_sdk);

	// Called every frame there is a player: reads Minecraft's time of day and weather, and when the player has turned the sync on (/ccsync) makes the game's follow them.
	// For now it only says in the log what it sees (the calls that set the game's time and weather are still to be found with /ccdebug class).
	void Update();
}
