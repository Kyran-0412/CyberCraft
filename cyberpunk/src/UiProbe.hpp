#pragma once

#include <RED4ext/RED4ext.hpp>

struct IDXGISwapChain;

namespace cybercraft::uiprobe
{
	void Init(RED4ext::v1::PluginHandle a_handle, const RED4ext::v1::Sdk* a_sdk);

	// /ccdebug ui [from]: record the next four frames and write a summary of the last whole one to the log (see UiProbe.cpp). The summary has a line for every
	// command list; a_detailFrom is the position of the first list to print in full (the lists before it get their line only), or -1 for the usual short details of every
	// list that matters.
	void Request(int a_detailFrom);

	// Called from the swapchain's Present hook every frame (cheap when nothing is going on): keeps track of the swapchain's buffers, installs the hooks the
	// first time they are needed, and writes the summary when a capture has finished.
	void OnPresent(IDXGISwapChain* a_swapChain);
}
