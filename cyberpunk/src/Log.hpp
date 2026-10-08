#pragma once

#include <atomic>

namespace cybercraft::log
{
	// CyberCraft.log is quiet by default: it records what happens once (the plugin loading, Minecraft linking, hooks being found, V following,
	// warnings and errors). The detailed numbers that are printed every few seconds (camera accuracy, depth capture, ground scan, input counts,
	// frame statistics) only appear while this is on: /ccdebug log on in Minecraft, or a file called verbose-log.txt next to the plugin to have it
	// on from the start. The measuring code keeps running either way, so switching it on shows the current numbers within seconds.
	inline std::atomic<bool> g_verbose{ false };

	inline bool Verbose()
	{
		return g_verbose.load(std::memory_order_relaxed);
	}

	inline void SetVerbose(bool a_on)
	{
		g_verbose.store(a_on, std::memory_order_relaxed);
	}
}
