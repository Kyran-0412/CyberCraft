// Phase 5 (depth): which texture is the game's depth buffer, and what is done to it?
//
// To hide Minecraft's blocks behind buildings and the ground, the overlay needs the game's depth buffer. A game like this
// draws its scene into a depth buffer, then uses it for later passes, and may reuse the memory afterwards, so by the time
// a frame is presented the depth is often gone. The way to get it is to copy it at the right moment, which needs to know:
//   * which texture it is (there are many depth textures: shadows, reflections, the main view);
//   * what state it is in, and when the game moves it out of "depth write";
//   * whether its memory is reused (aliased) by other things afterwards.
// The probe only watches and reports: it replaces ID3D12GraphicsCommandList::ResourceBarrier on the game's command lists
// with a function that looks at each barrier and then passes it on unchanged.
//
// The capture (switched on separately, /ccdepthcapture) goes one step further. The probe showed that each frame the game's main
// depth texture is written in about seven passes and then, once, moved from depth write to a plain shader-readable state: that
// last move is the end of all depth writing for the frame. (With DLSS on the game also copies the texture itself at some point,
// but with DLSS off it doesn't, so the copy moment can't be relied on.) Right after passing that barrier on, the hook records a copy
// of the whole texture into one of ours, and a few single-texel copies of it into a buffer that can be read on the CPU. Those texels
// are compared with how far the game's own rays say the world is at the same places on the screen, which shows whether the copy
// has the right depth in it and how its numbers relate to distance.

#include "Depth.hpp"
#include "Log.hpp"

#include <RED4ext/GpuApi/DeviceData.hpp>

#include <d3d12.h>
#include <wrl/client.h>
#include <mutex>

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
		constexpr int kTableSize = 1024;
		constexpr int kPairs = 6;

		RED4ext::v1::PluginHandle g_handle = nullptr;
		const RED4ext::v1::Sdk* g_sdk = nullptr;

		using ResourceBarrierFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);
		ResourceBarrierFn g_original = nullptr;
		void** g_vtable = nullptr;
		std::atomic<bool> g_active{ false };
		bool g_probeWanted = false;
		bool g_gaveUp = false;  // the hook or its self-test failed: don't try again until the probe is switched off and on
		std::chrono::steady_clock::time_point g_nextTry{};

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
			std::atomic<std::uint32_t> copySrcAll{ 0 };   // moved into a copy-source state, all subresources at once
			std::atomic<std::uint32_t> copySrcPart{ 0 };  // the same, but only some of its subresources
			std::atomic<std::uint32_t> copySrcAllTotal{ 0 };  // since capture started (never reset by a report)
			std::atomic<std::uint32_t> finalLeave{ 0 };       // moved from depth write to plain shader-readable, in this report's window
			std::atomic<std::uint32_t> finalLeaveTotal{ 0 };  // since capture started
			std::atomic<std::uint64_t> activityTotal{ 0 };    // passes into or out of depth write since capture started (the main depth has the most)
			Pair pairs[kPairs];
		};
		Entry g_table[kTableSize];

		std::chrono::steady_clock::time_point g_lastReport{};
		int g_reports = 0;

		// ---- capture ----
		constexpr int kSamples = 5;
		constexpr UINT kSampleStride = 512;  // the buffer's placement alignment
		bool g_captureWanted = false;
		bool g_captureGaveUp = false;
		std::atomic<bool> g_copyReady{ false };
		std::atomic<ID3D12Resource*> g_candidate{ nullptr };
		std::atomic<ID3D12Resource*> g_copyPtr{ nullptr };
		std::atomic<ID3D12Resource*> g_readbackPtr{ nullptr };
		Microsoft::WRL::ComPtr<ID3D12Resource> g_copy;
		Microsoft::WRL::ComPtr<ID3D12Resource> g_readback;
		std::uint8_t* g_readbackData = nullptr;
		UINT g_texWidth = 0, g_texHeight = 0;
		DXGI_FORMAT g_texFormat = DXGI_FORMAT_UNKNOWN;
		DXGI_FORMAT g_planeFormat = DXGI_FORMAT_R32_TYPELESS;
		UINT g_samplePixel[kSamples][2] = {};
		std::atomic<std::uint32_t> g_copies{ 0 };
		std::atomic<std::uint64_t> g_copiesTotal{ 0 };
		int g_candidateMisses = 0;
		std::vector<ID3D12Resource*> g_rejected;  // textures whose depth did not agree with the game's rays: not chosen again
		int g_sampleGood = 0;
		int g_sampleBad = 0;

		struct Graveyard
		{
			Microsoft::WRL::ComPtr<ID3D12Resource> resource;
			std::chrono::steady_clock::time_point since;
		};
		std::vector<Graveyard> g_graveyard;  // resources the GPU may still be using: kept alive for a while after they are dropped

		std::mutex g_referenceMutex;
		Reference g_references[kSamples];
		std::chrono::steady_clock::time_point g_referenceTime{};

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

		// Records, into the game's command list right after the barrier that made the texture readable as a copy source:
		// a copy of the whole texture into ours, and a few single-texel copies of it into the buffer the plugin reads.
		void RecordCapture(ID3D12GraphicsCommandList* a_list, D3D12_RESOURCE_STATES a_sourceState)
		{
			ID3D12Resource* source = g_candidate.load(std::memory_order_acquire);
			ID3D12Resource* copy = g_copyPtr.load(std::memory_order_acquire);
			ID3D12Resource* readback = g_readbackPtr.load(std::memory_order_acquire);
			if (!source || !copy || !readback) {
				return;
			}
			// The source is in a_sourceState (what the barrier just made it): lift it to copy source for the copy and put it back.
			const bool liftSource = (a_sourceState & D3D12_RESOURCE_STATE_COPY_SOURCE) == 0;
			if (liftSource) {
				D3D12_RESOURCE_BARRIER lift{};
				lift.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
				lift.Transition.pResource = source;
				lift.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
				lift.Transition.StateBefore = a_sourceState;
				lift.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
				g_original(a_list, 1, &lift);
			}
			a_list->CopyResource(copy, source);
			if (liftSource) {
				D3D12_RESOURCE_BARRIER drop{};
				drop.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
				drop.Transition.pResource = source;
				drop.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
				drop.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
				drop.Transition.StateAfter = a_sourceState;
				g_original(a_list, 1, &drop);
			}

			D3D12_RESOURCE_BARRIER toSource{};
			toSource.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			toSource.Transition.pResource = copy;
			toSource.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			toSource.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
			toSource.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
			g_original(a_list, 1, &toSource);  // (not through the hooked entry: that would count and recurse)

			for (int i = 0; i < kSamples; ++i) {
				D3D12_TEXTURE_COPY_LOCATION dst{};
				dst.pResource = readback;
				dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
				dst.PlacedFootprint.Offset = UINT64(i) * kSampleStride;
				dst.PlacedFootprint.Footprint.Format = g_planeFormat;
				dst.PlacedFootprint.Footprint.Width = 1;
				dst.PlacedFootprint.Footprint.Height = 1;
				dst.PlacedFootprint.Footprint.Depth = 1;
				dst.PlacedFootprint.Footprint.RowPitch = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
				D3D12_TEXTURE_COPY_LOCATION src{};
				src.pResource = copy;
				src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
				src.SubresourceIndex = 0;  // the depth plane
				D3D12_BOX box{ g_samplePixel[i][0], g_samplePixel[i][1], 0, g_samplePixel[i][0] + 1, g_samplePixel[i][1] + 1, 1 };
				a_list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
			}

			D3D12_RESOURCE_BARRIER back = toSource;
			back.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
			back.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
			g_original(a_list, 1, &back);
			g_copies.fetch_add(1, std::memory_order_relaxed);
			g_copiesTotal.fetch_add(1, std::memory_order_relaxed);
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
								e->activityTotal.fetch_add(1, std::memory_order_relaxed);
								NotePair(*e, before, after);
								if (leaving && after == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE && !(b.Flags & D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY)) {
									e->finalLeave.fetch_add(1, std::memory_order_relaxed);
									e->finalLeaveTotal.fetch_add(1, std::memory_order_relaxed);
								}
							}
						}
						if ((after & D3D12_RESOURCE_STATE_COPY_SOURCE) && !(b.Flags & D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY)) {
							// Only depth textures already in the table (seen moving into depth write) count here.
							if (Entry* e = Find(b.Transition.pResource, false)) {
								if (b.Transition.Subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) {
									e->copySrcAll.fetch_add(1, std::memory_order_relaxed);
									e->copySrcAllTotal.fetch_add(1, std::memory_order_relaxed);
								} else {
									e->copySrcPart.fetch_add(1, std::memory_order_relaxed);
								}
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

			// The capture: right after the candidate texture was moved out of depth write into a plain shader-readable state (all of it at once,
			// so a whole-texture copy is valid), which happens once a frame, after the last depth pass.
			if (g_copyReady.load(std::memory_order_acquire) && a_barriers) {
				ID3D12Resource* candidate = g_candidate.load(std::memory_order_acquire);
				for (UINT i = 0; i < a_count; ++i) {
					const D3D12_RESOURCE_BARRIER& b = a_barriers[i];
					if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION && b.Transition.pResource == candidate &&
						(b.Transition.StateBefore & D3D12_RESOURCE_STATE_DEPTH_WRITE) && b.Transition.StateAfter == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE &&
						!(b.Flags & D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY) && b.Transition.Subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) {
						// The same address could by now be another texture: only copy one that still has the shape that was chosen.
						const D3D12_RESOURCE_DESC now = candidate->GetDesc();
						if (now.Width == g_texWidth && now.Height == g_texHeight && now.Format == g_texFormat) {
							RecordCapture(a_list, b.Transition.StateAfter);
						}
						break;
					}
				}
			}
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

		// Every command list made by the game's device is an object of the same class, so they share one function table. Rather
		// than hunting for one of the game's own lists (the SDK's list of them turned out to be empty while a frame is
		// presented), make a temporary one from the game's device and take the table from that.
		struct TemporaryList
		{
			Microsoft::WRL::ComPtr<ID3D12Device> device;
			Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
			Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
		};

		bool MakeTemporaryList(TemporaryList& a_out)
		{
			auto* data = RED4ext::GpuApi::GetDeviceData();
			if (!data || !data->device) {
				return false;
			}
			a_out.device = data->device;
			if (FAILED(a_out.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a_out.allocator)))) {
				return false;
			}
			return SUCCEEDED(a_out.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a_out.allocator.Get(), nullptr, IID_PPV_ARGS(&a_out.list)));
		}

		void ClearTable()
		{
			for (Entry& e : g_table) {
				e.ready.store(false);
				e.resource.store(nullptr);
				e.enter = 0;
				e.leave = 0;
				e.alias = 0;
				for (Pair& p : e.pairs) {
					p.before = 0xFFFFFFFFu;
					p.after = 0;
					p.count = 0;
				}
			}
		}

		// Does the hook really see barriers? Makes a small depth texture, moves it out of depth write on the temporary
		// list, and checks the hook noticed. If this fails, the function table index is wrong.
		bool SelfTest(TemporaryList& a_temp)
		{
			D3D12_HEAP_PROPERTIES heap{};
			heap.Type = D3D12_HEAP_TYPE_DEFAULT;
			heap.CreationNodeMask = 1;
			heap.VisibleNodeMask = 1;
			D3D12_RESOURCE_DESC desc{};
			desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			desc.Width = 16;
			desc.Height = 16;
			desc.DepthOrArraySize = 1;
			desc.MipLevels = 1;
			desc.Format = DXGI_FORMAT_D32_FLOAT;
			desc.SampleDesc.Count = 1;
			desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
			desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
			D3D12_CLEAR_VALUE clear{};
			clear.Format = DXGI_FORMAT_D32_FLOAT;
			Microsoft::WRL::ComPtr<ID3D12Resource> texture;
			if (FAILED(a_temp.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear, IID_PPV_ARGS(&texture)))) {
				g_sdk->logger->Warn(g_handle, "depth: self-test could not make a test texture");
				return false;
			}
			D3D12_RESOURCE_BARRIER barrier{};
			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barrier.Transition.pResource = texture.Get();
			barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
			barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			g_active = true;
			a_temp.list->ResourceBarrier(1, &barrier);
			g_active = false;
			a_temp.list->Close();
			Entry* e = Find(texture.Get(), false);
			const bool seen = e && e->leave.load() > 0 && e->width == 16;
			return seen;
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

	namespace
	{
		// Where on the screen the capture reads the depth (and where the game's own rays measure it), as fractions across and down.
		constexpr float kReferenceUV[kSamples][2] = { { 0.5f, 0.5f }, { 0.25f, 0.5f }, { 0.75f, 0.5f }, { 0.5f, 0.25f }, { 0.5f, 0.75f } };

		void Bury(Microsoft::WRL::ComPtr<ID3D12Resource>& a_resource)
		{
			if (a_resource) {
				g_graveyard.push_back({ a_resource, std::chrono::steady_clock::now() });
				a_resource.Reset();
			}
		}

		void StopCapture()
		{
			if (!g_copyReady.exchange(false)) {
				return;
			}
			g_candidate = nullptr;
			g_copyPtr = nullptr;
			g_readbackPtr = nullptr;
			if (g_readback && g_readbackData) {
				g_readback->Unmap(0, nullptr);
			}
			g_readbackData = nullptr;
			// The GPU may still be running command lists that name these: keep them alive for a while.
			Bury(g_copy);
			Bury(g_readback);
			g_sdk->logger->Info(g_handle, "depth: capture stopped");
		}

		const char* FormatText(DXGI_FORMAT a_format, char* a_buffer, std::size_t a_size)
		{
			if (const char* name = FormatName(a_format)) {
				return name;
			}
			std::snprintf(a_buffer, a_size, "format %d", static_cast<int>(a_format));
			return a_buffer;
		}

		bool StartCapture(Entry& a_entry)
		{
			auto* data = RED4ext::GpuApi::GetDeviceData();
			ID3D12Resource* source = a_entry.resource.load();
			if (!data || !data->device || !source) {
				return false;
			}
			Microsoft::WRL::ComPtr<ID3D12Device> device = data->device;
			const D3D12_RESOURCE_DESC desc = source->GetDesc();

			D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
			device->GetCopyableFootprints(&desc, 0, 1, 0, &layout, nullptr, nullptr, nullptr);

			D3D12_HEAP_PROPERTIES defaultHeap{};
			defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
			defaultHeap.CreationNodeMask = 1;
			defaultHeap.VisibleNodeMask = 1;
			Microsoft::WRL::ComPtr<ID3D12Resource> copy;
			if (FAILED(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&copy)))) {
				g_sdk->logger->Error(g_handle, "depth: capture could not make the texture to copy into");
				return false;
			}

			D3D12_HEAP_PROPERTIES readbackHeap = defaultHeap;
			readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;
			D3D12_RESOURCE_DESC bufferDesc{};
			bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			bufferDesc.Width = 4096;
			bufferDesc.Height = 1;
			bufferDesc.DepthOrArraySize = 1;
			bufferDesc.MipLevels = 1;
			bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
			bufferDesc.SampleDesc.Count = 1;
			bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			Microsoft::WRL::ComPtr<ID3D12Resource> readback;
			if (FAILED(device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)))) {
				g_sdk->logger->Error(g_handle, "depth: capture could not make the readback buffer");
				return false;
			}
			void* mapped = nullptr;
			if (FAILED(readback->Map(0, nullptr, &mapped)) || !mapped) {
				g_sdk->logger->Error(g_handle, "depth: capture could not map the readback buffer");
				return false;
			}

			g_texWidth = static_cast<UINT>(desc.Width);
			g_texHeight = desc.Height;
			g_texFormat = desc.Format;
			g_planeFormat = layout.Footprint.Format;
			for (int i = 0; i < kSamples; ++i) {
				g_samplePixel[i][0] = std::min<UINT>(g_texWidth - 1, static_cast<UINT>(kReferenceUV[i][0] * float(g_texWidth - 1) + 0.5f));
				g_samplePixel[i][1] = std::min<UINT>(g_texHeight - 1, static_cast<UINT>(kReferenceUV[i][1] * float(g_texHeight - 1) + 0.5f));
			}
			g_copy = copy;
			g_readback = readback;
			g_readbackData = static_cast<std::uint8_t*>(mapped);
			g_copies = 0;
			g_candidateMisses = 0;
			g_sampleGood = 0;
			g_sampleBad = 0;
			g_candidate.store(source, std::memory_order_release);
			g_copyPtr.store(g_copy.Get(), std::memory_order_release);
			g_readbackPtr.store(g_readback.Get(), std::memory_order_release);
			g_copyReady.store(true, std::memory_order_release);

			char formatText[32];
			g_sdk->logger->InfoF(g_handle, "depth: capture started: copying texture %p (%u x %u, %s; the depth plane reads as %s) into a texture of ours each frame, right after its last depth pass",
				static_cast<void*>(source), g_texWidth, g_texHeight, FormatText(desc.Format, formatText, sizeof(formatText)),
				layout.Footprint.Format == DXGI_FORMAT_R32_TYPELESS ? "R32_TYPELESS" : "another format");
			return true;
		}

		// Which depth texture does the game copy from once a frame, at roughly the screen's shape? That's the one to capture.
		Entry* ChooseCaptureTexture()
		{
			Entry* best = nullptr;
			std::uint64_t bestScore = 0;
			for (Entry& e : g_table) {
				if (e.resource.load() == nullptr || !e.ready.load() || e.dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
					continue;
				}
				if (!(e.flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) {
					continue;
				}
				if (std::find(g_rejected.begin(), g_rejected.end(), e.resource.load()) != g_rejected.end()) {
					continue;
				}
				if (e.format != DXGI_FORMAT_R32G8X24_TYPELESS && e.format != DXGI_FORMAT_R24G8_TYPELESS && e.format != DXGI_FORMAT_R32_TYPELESS) {
					continue;
				}
				const double aspect = double(e.width) / double(std::max<UINT>(1, e.height));
				if (e.width < 640 || e.height < 360 || aspect < 1.4 || aspect > 2.5) {
					continue;
				}
				if (e.finalLeaveTotal.load() == 0) {
					continue;  // never seen moving from depth write to plain readable: no moment to copy at
				}
				const std::uint64_t score = e.activityTotal.load();
				if (score > bestScore) {
					bestScore = score;
					best = &e;
				}
			}
			return best;
		}

		void Poll()
		{
			const auto now = std::chrono::steady_clock::now();
			g_graveyard.erase(std::remove_if(g_graveyard.begin(), g_graveyard.end(), [&](const Graveyard& g) { return now - g.since > std::chrono::seconds(4); }),
				g_graveyard.end());

			if (!g_captureWanted || g_captureGaveUp || g_copyReady.load() || !g_active.load()) {
				return;
			}
			static auto nextTry = std::chrono::steady_clock::time_point{};
			static auto lastComplaint = std::chrono::steady_clock::time_point{};
			if (now < nextTry) {
				return;
			}
			nextTry = now + std::chrono::seconds(1);
			if (Entry* chosen = ChooseCaptureTexture()) {
				if (!StartCapture(*chosen)) {
					g_captureGaveUp = true;
				}
			} else if (!g_rejected.empty()) {
				// Every candidate failed the check against the game's rays: forget the verdicts and start over (the scene may just
				// have been a bad one to check, say with a car in the way).
				g_sdk->logger->Info(g_handle, "depth: capture: no candidate passed the check against the game's rays; trying them all again");
				g_rejected.clear();
			} else if (now - lastComplaint > std::chrono::seconds(6)) {
				lastComplaint = now;
				g_sdk->logger->Info(g_handle, "depth: capture is waiting for a depth texture that is moved out of depth write for good once a frame (none seen yet)");
			}
		}

		// The monitoring itself: the hook on the game's command lists, and the table it fills.
		void SetMonitoring(bool a_on)
		{
			if (!a_on) {
				g_gaveUp = false;
			}
			if (a_on && g_gaveUp) {
				return;
			}
			if (a_on == g_probeWanted && (g_active.load() == a_on)) {
				return;
			}
			if (a_on && std::chrono::steady_clock::now() < g_nextTry) {
				return;
			}
			g_probeWanted = a_on;

			if (!a_on) {
				g_active = false;  // the hook stays in place (it is a pass-through), just stops looking
				g_sdk->logger->Info(g_handle, "depth: tools off");
				return;
			}

			if (!g_original) {
				TemporaryList temp;
				if (!MakeTemporaryList(temp)) {
					static auto lastComplaint = std::chrono::steady_clock::time_point{};
					const auto now = std::chrono::steady_clock::now();
					g_nextTry = now + std::chrono::seconds(1);
					if (now - lastComplaint > std::chrono::seconds(5)) {
						lastComplaint = now;
						g_sdk->logger->Warn(g_handle, "depth: could not make a command list from the game's device yet; will keep trying");
					}
					return;
				}
				g_vtable = *reinterpret_cast<void***>(temp.list.Get());
				Patch(reinterpret_cast<void*>(&HookedResourceBarrier), reinterpret_cast<void**>(&g_original));
				if (!g_original) {
					g_sdk->logger->Error(g_handle, "depth: could not hook the game's command lists");
					g_probeWanted = false;
					g_gaveUp = true;
					return;
				}
				g_sdk->logger->InfoF(g_handle, "depth: hooked ResourceBarrier on the game's command lists (function table %p, original function %p)",
					static_cast<void*>(g_vtable), reinterpret_cast<void*>(g_original));
				const bool works = SelfTest(temp);
				g_sdk->logger->Info(g_handle, works ? "depth: self-test passed: the hook sees a depth texture move out of depth write"
				                                    : "depth: self-test FAILED: the hook did not see a test barrier, so the function table index is probably wrong; the tools are off");
				if (!works) {
					void* ignored = nullptr;
					Patch(reinterpret_cast<void*>(g_original), &ignored);
					g_original = nullptr;
					g_probeWanted = false;
					g_gaveUp = true;
					return;
				}
				ClearTable();
			}
			for (Entry& e : g_table) {
				e.enter = 0;
				e.leave = 0;
				e.alias = 0;
				e.copySrcAll = 0;
				e.copySrcPart = 0;
				e.finalLeave = 0;
			}
			g_reports = 0;
			g_lastReport = std::chrono::steady_clock::now();
			g_active = true;
			g_sdk->logger->Info(g_handle, "depth: tools on; the first report comes in a few seconds");
		}
	}

	void SetModes(bool a_probe, bool a_capture)
	{
		SetMonitoring(a_probe || a_capture);
		if (a_capture && !g_captureWanted) {
			g_captureWanted = true;
			g_captureGaveUp = false;
			for (Entry& e : g_table) {
				e.copySrcAllTotal = 0;
				e.finalLeaveTotal = 0;
				e.activityTotal = 0;
			}
			g_sdk->logger->Info(g_handle, "depth: capture requested");
		} else if (!a_capture && g_captureWanted) {
			g_captureWanted = false;
			g_rejected.clear();
			StopCapture();
		}
	}

	bool GetGameDepth(GameDepth& a_out)
	{
		a_out = GameDepth{};
		if (!g_copyReady.load(std::memory_order_acquire) || g_copiesTotal.load(std::memory_order_relaxed) == 0) {
			return false;
		}
		a_out.resource = g_copyPtr.load(std::memory_order_acquire);
		a_out.width = g_texWidth;
		a_out.height = g_texHeight;
		switch (g_texFormat) {
		case DXGI_FORMAT_R32G8X24_TYPELESS: a_out.srvFormat = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS; break;
		case DXGI_FORMAT_R24G8_TYPELESS: a_out.srvFormat = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; break;
		case DXGI_FORMAT_R32_TYPELESS: a_out.srvFormat = DXGI_FORMAT_R32_FLOAT; break;
		default: return false;
		}
		a_out.ready = a_out.resource != nullptr;
		return a_out.ready;
	}

	bool CaptureWanted()
	{
		return g_captureWanted && g_active.load();
	}

	void SetReferences(const Reference* a_references, int a_count)
	{
		std::lock_guard lock(g_referenceMutex);
		for (int i = 0; i < kSamples && i < a_count; ++i) {
			g_references[i] = a_references[i];
		}
		g_referenceTime = std::chrono::steady_clock::now();
	}

	void Report()
	{
		if (g_probeWanted && !g_active.load() && !g_gaveUp) {
			SetMonitoring(true);  // the game had no device to make a command list from the last time: try again
		}
		Poll();
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
			std::uint32_t enter, leave, alias, copyAll, copyPart, finalLeave;
		};
		std::vector<Row> rows;
		for (Entry& e : g_table) {
			if (e.resource.load() == nullptr || !e.ready.load()) {
				continue;
			}
			const std::uint32_t enter = e.enter.exchange(0);
			const std::uint32_t leave = e.leave.exchange(0);
			const std::uint32_t alias = e.alias.exchange(0);
			const std::uint32_t copyAll = e.copySrcAll.exchange(0);
			const std::uint32_t copyPart = e.copySrcPart.exchange(0);
			const std::uint32_t finalLeave = e.finalLeave.exchange(0);
			if (enter + leave > 0) {
				rows.push_back({ &e, enter, leave, alias, copyAll, copyPart, finalLeave });
			}
		}
		std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
			return std::uint64_t(a.entry->width) * a.entry->height * (a.enter + 1) > std::uint64_t(b.entry->width) * b.entry->height * (b.enter + 1);
		});

		if (log::Verbose()) g_sdk->logger->InfoF(g_handle, "depth: report %d: %u textures were moved into or out of depth write in the last 4 s", g_reports, static_cast<unsigned>(rows.size()));
		int shown = 0;
		for (const Row& row : rows) {
			if (!log::Verbose() || shown++ >= 10) {
				break;
			}
			const Entry& e = *row.entry;
			char formatText[32];
			g_sdk->logger->InfoF(g_handle,
				"  texture %p: %llu x %u, %s, %u mips, array %u, %s depth-stencil flag; into depth write %u times, out of it %u times, named in %u aliasing barriers; made a copy source %u times (%u of them only for some of its parts); left depth write for good %u times",
				static_cast<void*>(e.resource.load()), static_cast<unsigned long long>(e.width), e.height, FormatText(e.format, formatText, sizeof(formatText)), e.mips, e.depthOrArray,
				(e.flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) ? "has the" : "NO", row.enter, row.leave, row.alias, row.copyAll + row.copyPart, row.copyPart, row.finalLeave);
			for (int i = 0; i < kPairs; ++i) {
				const std::uint32_t before = e.pairs[i].before.load();
				const std::uint32_t count = e.pairs[i].count.load();
				if (before == 0xFFFFFFFFu || count == 0) {
					continue;
				}
				g_sdk->logger->InfoF(g_handle, "      %s -> %s  (%u times so far)", StateName(before).c_str(), StateName(e.pairs[i].after.load()).c_str(), count);
			}
		}

		// ---- the capture ----
		if (!g_copyReady.load()) {
			return;
		}
		ID3D12Resource* candidate = g_candidate.load();
		std::uint32_t copyMoments = 0;
		for (const Row& row : rows) {
			if (row.entry->resource.load() == candidate) {
				copyMoments = row.finalLeave;
			}
		}
		const std::uint32_t copies = g_copies.exchange(0);
		if (log::Verbose()) g_sdk->logger->InfoF(g_handle, "depth: capture: %u copies of the %u x %u texture %p in the last 4 s (copy moments seen: %u; readings that fit depth = 0.02 / distance: %d of %d)", copies,
			g_texWidth, g_texHeight, static_cast<void*>(candidate), copyMoments, g_sampleGood, g_sampleGood + g_sampleBad);
		g_candidateMisses = copyMoments == 0 ? g_candidateMisses + 1 : 0;
		if (g_candidateMisses >= 3) {
			g_sdk->logger->Info(g_handle, "depth: capture: the texture no longer leaves depth write once a frame; choosing again");
			StopCapture();
			return;
		}

		Reference refs[kSamples];
		bool fresh = false;
		{
			std::lock_guard lock(g_referenceMutex);
			for (int i = 0; i < kSamples; ++i) {
				refs[i] = g_references[i];
			}
			fresh = now - g_referenceTime < std::chrono::seconds(3);
		}
		if (!g_readbackData) {
			return;
		}
		if (g_sampleGood + g_sampleBad >= 15 && g_sampleGood * 100 < 35 * (g_sampleGood + g_sampleBad)) {
			g_sdk->logger->InfoF(g_handle, "depth: capture: this texture's values did not fit depth = 0.02 / distance in %d of %d readings: choosing another", g_sampleBad, g_sampleGood + g_sampleBad);
			g_rejected.push_back(candidate);
			StopCapture();
			return;
		}
		for (int i = 0; i < kSamples; ++i) {
			std::uint32_t bits = 0;
			std::memcpy(&bits, g_readbackData + std::size_t(i) * kSampleStride, sizeof(bits));
			float value;
			if (g_texFormat == DXGI_FORMAT_R24G8_TYPELESS) {
				value = float(bits & 0xFFFFFFu) / 16777215.0f;
			} else {
				std::memcpy(&value, &bits, sizeof(value));
			}
			if (fresh && refs[i].valid && refs[i].z > 2.0f) {
				// Does the value follow depth = 0.02 / distance where the game's own ray says how far the world is?
				if (std::fabs(value * refs[i].z - 0.02f) < 0.0015f) {
					++g_sampleGood;
				} else {
					++g_sampleBad;
				}
			}
			if (fresh && refs[i].valid && refs[i].z > 0.0f) {
				if (log::Verbose()) g_sdk->logger->InfoF(g_handle,
					"  depth sample %d at (%.2f, %.2f), pixel (%u, %u): value %.6f; the game's ray says the world is %.2f m ahead there; value x distance = %.5f (if depth = near / distance, near = that), (1 - value) x distance = %.5f (if depth = 1 - near / distance)",
					i, kReferenceUV[i][0], kReferenceUV[i][1], g_samplePixel[i][0], g_samplePixel[i][1], value, refs[i].z, value * refs[i].z, (1.0f - value) * refs[i].z);
			} else {
				if (log::Verbose()) g_sdk->logger->InfoF(g_handle, "  depth sample %d at (%.2f, %.2f), pixel (%u, %u): value %.6f; the game's ray found nothing there (or hasn't been measured; stand still)", i,
					kReferenceUV[i][0], kReferenceUV[i][1], g_samplePixel[i][0], g_samplePixel[i][1], value);
			}
		}
	}

	void Shutdown()
	{
		g_active = false;
		g_copyReady = false;  // the hook stops recording copies at once; the resources are left for the process to free
		if (g_original && g_vtable) {
			// Put the game's own function back. (g_original itself stays as it is: a thread still inside the hook needs it.)
			void* ignored = nullptr;
			Patch(reinterpret_cast<void*>(g_original), &ignored);
		}
	}
}
