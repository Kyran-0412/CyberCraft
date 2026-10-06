#pragma once

#include <filesystem>

namespace cybercraft::mapping
{
	// Cyberpunk's height (Z) and Minecraft's height (Y) differ by this much: Minecraft Y = Cyberpunk Z - Offset().
	//
	// Minecraft's blocks sit on whole-number heights, but Night City's streets are at heights like 22.6. A block built
	// "on" the street at 22.6 would really start at 22.0, 0.6 m underneath it, and since Minecraft's blocks are drawn
	// over the game without being hidden by the ground, that sunken part shows through the road and makes the block
	// look as if it slides about as you move. /ccalign sets this offset so that the street where you stand lands on a
	// whole-number height, and the blocks you build there sit exactly on it. It is remembered between runs.
	double Offset();

	// Sets it (and remembers it).
	void SetOffset(double a_offset);

	// The folder the plugin's DLL is in (red4ext\\plugins\\CyberCraft), where its small settings files live.
	std::filesystem::path PluginFolder();

	// Reads the remembered offset from the plugin's folder (call once at start).
	void Load();
}
