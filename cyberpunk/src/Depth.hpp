#pragma once

#include <RED4ext/RED4ext.hpp>

namespace cybercraft::depth
{
	void Init(RED4ext::v1::PluginHandle a_handle, const RED4ext::v1::Sdk* a_sdk);

	// Turns the depth-buffer probe on or off. While on, the game's command lists are watched for resource state changes
	// into "depth write", which finds out which textures the game uses as depth buffers, how big they are, and what
	// happens to them afterwards. It changes nothing in the picture, and it is off unless asked for (/ccdepthprobe).
	void SetProbe(bool a_on);

	// Call once per presented frame: while the probe is on, writes what it has seen to the log every few seconds.
	void Report();

	// Puts everything back (game exit).
	void Shutdown();
}
