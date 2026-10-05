// Phase 5 (depth): which texture is the game's depth buffer, and what is done to it?
//
// To hide Minecraft's blocks behind buildings and the ground, the overlay needs the game's depth buffer. A game like this
// draws its scene into a depth buffer, then uses it for later passes, and may reuse the memory afterwards, so by the time
// a frame is presented the depth is often gone. The way to get it is to copy it at the right moment, which needs to know:
//   * which texture it is (there are many depth textures: shadows, reflections, the main view);
//   * what state it is in, and when the game moves it out of "depth write";
//   * whether its memory is reused (aliased) by other things afterwards.
// This file only watches and reports. It replaces ID3D12GraphicsCommandList::ResourceBarrier on the game's command lists
// with a function that looks at each barrier and then passes it on unchanged.

#include "Depth.hpp"

#include <RED4ext/GpuApi/DeviceData.hpp>

#include <d3d12.h>
#include <wrl/client.h>

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace cybercraft::depth
{
	namespace
	{
		constexpr UINT kResourceBarrierIndex = 26;  // ID3D12GraphicsCommandList::ResourceBarrier
		constexpr int kTableSize = 512;
		constexpr int kPairs = 6;

		RED4ext::v1::PluginHandle g_handle = nullptr;
		const RED4ext::v1::Sdk* g_sdk = nullptr;

		using ResourceBarrierFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);
		ResourceBarrierFn g_original = nullptr;
		void** g_vtable = nullptr;
		std::atomic<bool> g_active{ false };
		bool g_probeWanted = false;

		struct Pair
		{
			std::atomic<std::uint32_t> before{ 0xFFFFFFFFu };
			std::atomic<std::uint32_t> after{ 0 };
			std::atomic<std::uint32_t> count{ 0 };
		};

		struct Entry
		{
			std::atomic<ID3D12Resource*> resource{ nullptr };
			std::atomic<bool> ready{ false };
			UINT64 width = 0;
			UINT height = 0;
			UINT16 depthOrArray = 0;
			UINT16 mips = 0;
			DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
			D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
			D3D12_RESOURCE_DIMENSION dimension = D3D12_RESOURCE_DIMENSION_UNKNOWN;
			std::atomic<std::uint32_t> enter{ 0 };  // moved into depth write
			std::atomic<std::uint32_t> leave{ 0 };  // moved out of depth write
			std::atomic<std::uint32_t> alias{ 0 };  // named in an aliasing barrier
			Pair pairs[kPairs];
		};
		Entry g_table[kTableSize];

		std::chrono::steady_clock::time_point g_lastReport{};
		int g_reports = 0;

		std::size_t HashOf(const void* a_pointer)
		{
			std::uintptr_t v = reinterpret_cast<std::uintptr_t>(a_pointer);
			v ^= v >> 17;
			v *= 0x9E3779B97F4A7C15ull;
			return static_cast<std::size_t>(v >> 32);
		}

		// Finds (or, if a_create, adds) the table entry for a resource. Called from many threads.
		Entry* Find(ID3D12Resource* a_resource, bool a_create)
		{
			std::size_t i = HashOf(a_resource) % kTableSize;
			for (int probe = 0; probe < kTableSize; ++probe, i = (i + 1) % kTableSize) {
				Entry& e = g_table[i];
				ID3D12Resource* current = e.resource.load(std::memory_order_acquire);
				if (current == a_resource) {
					while (!e.ready.load(std::memory_order_acquire)) {
						// another thread is filling in the description: wait a moment
					}
					return &e;
				}
				if (current == nullptr) {
					if (!a_create) {
						return nullptr;
					}
					ID3D12Resource* expected = nullptr;
					if (e.resource.compare_exchange_strong(expected, a_resource, std::memory_order_acq_rel)) {
						const D3D12_RESOURCE_DESC desc = a_resource->GetDesc();
						e.width = desc.Width;
						e.height = desc.Height;
						e.depthOrArray = desc.DepthOrArraySize;
						e.mips = desc.MipLevels;
						e.format = desc.Format;
						e.flags = desc.Flags;
						e.dimension = desc.Dimension;
						e.ready.store(true, std::memory_order_release);
						return &e;
					}
					if (expected == a_resource) {
						while (!e.ready.load(std::memory_order_acquire)) {
						}
						return &e;
					}
				}
			}
			return nullptr;
		}

		void NotePair(Entry& a_entry, std::uint32_t a_before, std::uint32_t a_after)
		{
			for (int i = 0; i < kPairs; ++i) {
				Pair& p = a_entry.pairs[i];
				std::uint32_t before = p.before.load(std::memory_order_relaxed);
				if (before == 0xFFFFFFFFu) {
					std::uint32_t expected = 0xFFFFFFFFu;
					if (p.before.compare_exchange_strong(expected, a_before)) {
						p.after.store(a_after);
						before = a_before;
					} else {
						before = expected;
					}
				}
				if (before == a_before && p.after.load(std::memory_order_relaxed) == a_after) {
					p.count.fetch_add(1, std::memory_order_relaxed);
					return;
				}
			}
		}

		void STDMETHODCALLTYPE HookedResourceBarrier(ID3D12GraphicsCommandList* a_list, UINT a_count, const D3D12_RESOURCE_BARRIER* a_barriers)
		{
			if (g_active.load(std::memory_order_relaxed) && a_barriers) {
				for (UINT i = 0; i < a_count; ++i) {
					const D3D12_RESOURCE_BARRIER& b = a_barriers[i];
					if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION && b.Transition.pResource) {
						const auto before = static_cast<std::uint32_t>(b.Transition.StateBefore);
						const auto after = static_cast<std::uint32_t>(b.Transition.StateAfter);
						const bool entering = (after & D3D12_RESOURCE_STATE_DEPTH_WRITE) != 0;
						const bool leaving = (before & D3D12_RESOURCE_STATE_DEPTH_WRITE) != 0;
						if (entering || leaving) {
							if (Entry* e = Find(b.Transition.pResource, true)) {
								(entering ? e->enter : e->leave).fetch_add(1, std::memory_order_relaxed);
								NotePair(*e, before, after);
							}
						}
					} else if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING) {
						if (b.Aliasing.pResourceBefore) {
							if (Entry* e = Find(b.Aliasing.pResourceBefore, false)) {
								e->alias.fetch_add(1, std::memory_order_relaxed);
							}
						}
						if (b.Aliasing.pResourceAfter) {
							if (Entry* e = Find(b.Aliasing.pResourceAfter, false)) {
								e->alias.fetch_add(1, std::memory_order_relaxed);
							}
						}
					}
				}
			}
			g_original(a_list, a_count, a_barriers);
		}

		std::string StateName(std::uint32_t a_state)
		{
			if (a_state == 0) {
				return "COMMON/PRESENT";
			}
			struct Bit
			{
				std::uint32_t value;
				const char* name;
			};
			static const Bit kBits[] = {
				{ 0x1, "VERTEX/CONST" },
				{ 0x2, "INDEX" },
				{ 0x4, "RENDER_TARGET" },
				{ 0x8, "UAV" },
				{ 0x10, "DEPTH_WRITE" },
				{ 0x20, "DEPTH_READ" },
				{ 0x40, "NON_PIXEL_SRV" },
				{ 0x80, "PIXEL_SRV" },
				{ 0x100, "STREAM_OUT" },
				{ 0x200, "INDIRECT" },
				{ 0x400, "COPY_DEST" },
				{ 0x800, "COPY_SOURCE" },
				{ 0x1000, "RESOLVE_DEST" },
				{ 0x2000, "RESOLVE_SOURCE" },
			};
			std::string text;
			for (const Bit& bit : kBits) {
				if (a_state & bit.value) {
					if (!text.empty()) {
						text += "|";
					}
					text += bit.name;
				}
			}
			if (text.empty()) {
				text = "0x" + std::to_string(a_state);
			}
			return text;
		}

		const char* FormatName(DXGI_FORMAT a_format)
		{
			switch (a_format) {
			case DXGI_FORMAT_D32_FLOAT: return "D32_FLOAT";
			case DXGI_FORMAT_R32_TYPELESS: return "R32_TYPELESS";
			case DXGI_FORMAT_D24_UNORM_S8_UINT: return "D24_UNORM_S8_UINT";
			case DXGI_FORMAT_R24G8_TYPELESS: return "R24G8_TYPELESS";
			case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return "D32_FLOAT_S8X24_UINT";
			case DXGI_FORMAT_R32G8X24_TYPELESS: return "R32G8X24_TYPELESS";
			case DXGI_FORMAT_D16_UNORM: return "D16_UNORM";
			case DXGI_FORMAT_R16_TYPELESS: return "R16_TYPELESS";
			default: return nullptr;
			}
		}

		// A command list of the game's, to find the shared function table through.
		ID3D12GraphicsCommandList* FindGameCommandList()
		{
			auto* data = RED4ext::GpuApi::GetDeviceData();
			if (!data) {
				return nullptr;
			}
			for (std::size_t i = 0; i < 128; ++i) {
				auto& slot = data->commandLists.resources[i];
				if (slot.refCount >= 0 && slot.instance.GetPtr() != nullptr) {
					if (auto* list = slot.instance->commandList.Get()) {
						return list;
					}
				}
			}
			return nullptr;
		}

		void Patch(void* a_hook, void** a_original)
		{
			void** slot = &g_vtable[kResourceBarrierIndex];
			DWORD oldProtect = 0;
			if (::VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
				*a_original = *slot;
				*slot = a_hook;
				::VirtualProtect(slot, sizeof(void*), oldProtect, &oldProtect);
			}
		}
	}

	void Init(RED4ext::v1::PluginHandle a_handle, const RED4ext::v1::Sdk* a_sdk)
	{
		g_handle = a_handle;
		g_sdk = a_sdk;
	}

	void SetProbe(bool a_on)
	{
		if (a_on == g_probeWanted && (g_active.load() == a_on)) {
			return;
		}
		g_probeWanted = a_on;

		if (!a_on) {
			g_active = false;  // the hook stays in place (it is a pass-through), just stops looking
			g_sdk->logger->Info(g_handle, "depth: probe off");
			return;
		}

		if (!g_original) {
			auto* list = FindGameCommandList();
			if (!list) {
				return;  // try again on a later call: the game may not have a command list in use yet
			}
			g_vtable = *reinterpret_cast<void***>(list);
			Patch(reinterpret_cast<void*>(&HookedResourceBarrier), reinterpret_cast<void**>(&g_original));
			if (!g_original) {
				g_sdk->logger->Error(g_handle, "depth: could not hook the game's command lists");
				g_probeWanted = false;
				return;
			}
			g_sdk->logger->InfoF(g_handle, "depth: watching resource barriers on the game's command lists (function table %p, ResourceBarrier %p)",
				static_cast<void*>(g_vtable), reinterpret_cast<void*>(g_original));
		}
		for (Entry& e : g_table) {
			e.enter = 0;
			e.leave = 0;
			e.alias = 0;
		}
		g_reports = 0;
		g_lastReport = std::chrono::steady_clock::now();
		g_active = true;
		g_sdk->logger->Info(g_handle, "depth: probe on; the first report comes in a few seconds");
	}

	void Report()
	{
		if (g_probeWanted && !g_active.load()) {
			SetProbe(true);  // the game had no command list to hook the last time: try again
		}
		if (!g_active.load()) {
			return;
		}
		const auto now = std::chrono::steady_clock::now();
		if (now - g_lastReport < std::chrono::seconds(4)) {
			return;
		}
		g_lastReport = now;
		++g_reports;

		struct Row
		{
			Entry* entry;
			std::uint32_t enter, leave, alias;
		};
		std::vector<Row> rows;
		for (Entry& e : g_table) {
			if (e.resource.load() == nullptr || !e.ready.load()) {
				continue;
			}
			const std::uint32_t enter = e.enter.exchange(0);
			const std::uint32_t leave = e.leave.exchange(0);
			const std::uint32_t alias = e.alias.exchange(0);
			if (enter + leave > 0) {
				rows.push_back({ &e, enter, leave, alias });
			}
		}
		std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
			return std::uint64_t(a.entry->width) * a.entry->height * (a.enter + 1) > std::uint64_t(b.entry->width) * b.entry->height * (b.enter + 1);
		});

		g_sdk->logger->InfoF(g_handle, "depth: report %d: %u textures were moved into or out of depth write in the last 4 s", g_reports, static_cast<unsigned>(rows.size()));
		int shown = 0;
		for (const Row& row : rows) {
			if (shown++ >= 10) {
				break;
			}
			const Entry& e = *row.entry;
			const char* name = FormatName(e.format);
			char formatText[32];
			if (!name) {
				std::snprintf(formatText, sizeof(formatText), "format %d", static_cast<int>(e.format));
			}
			g_sdk->logger->InfoF(g_handle, "  texture %p: %llu x %u, %s, %u mips, array %u, %s depth-stencil flag; into depth write %u times, out of it %u times, named in %u aliasing barriers",
				static_cast<void*>(e.resource.load()), static_cast<unsigned long long>(e.width), e.height, name ? name : formatText, e.mips, e.depthOrArray,
				(e.flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) ? "has the" : "NO", row.enter, row.leave, row.alias);
			for (int i = 0; i < kPairs; ++i) {
				const std::uint32_t before = e.pairs[i].before.load();
				const std::uint32_t count = e.pairs[i].count.load();
				if (before == 0xFFFFFFFFu || count == 0) {
					continue;
				}
				g_sdk->logger->InfoF(g_handle, "      %s -> %s  (%u times so far)", StateName(before).c_str(), StateName(e.pairs[i].after.load()).c_str(), count);
			}
		}
	}

	void Shutdown()
	{
		g_active = false;
		if (g_original && g_vtable) {
			// Put the game's own function back. (g_original itself stays as it is: a thread still inside the hook needs it.)
			void* ignored = nullptr;
			Patch(reinterpret_cast<void*>(g_original), &ignored);
		}
	}
}
