// The UI probe: where in the game's frame does Cyberpunk draw its interface?
//
// To draw Minecraft's blocks *behind* the game's interface (the health bar, the minimap, the quest text), the blocks must be drawn into the
// picture before the interface is. Unlike the depth, the interface is not in a texture of its own that could be copied afterwards: the game
// draws it onto the finished picture. So the blocks have to be inserted into the game's own drawing commands, right where the interface starts,
// and for that we need to know where that is.
//
// It also lists every 3D texture the frame moves between states (or puts a UAV barrier on), with its size and format and the lists that touch it: the game's fog, volumetric lighting and colour
// grading tables are stored as 3D textures, so this is how they are found.
//
// The first two captures turned out to show only the plugin's own overlay list (it takes the back buffer out of PRESENT too), and merged the
// frames of command lists the game re-uses. So this probe keeps a separate record for each recording of a list (a list is recorded again after
// it is Reset), cuts frames at the swapchain's Present calls, notes which lists were run without being recorded in the capture (a list the game
// recorded once and runs every frame), counts indirect draws, and resolves each render target change to the texture it points at (by watching
// the render target views the game creates). It writes a summary of the last complete frame: one line per command list in the order they were
// run, and for the lists that matter the screen-sized textures they move around, the copies between them, and their draws grouped into runs that
// use the same pipeline. The interface should show up as a list with many draws in many different pipelines, writing into a screen-sized
// texture that is read later.
//
// It works the way the depth probe does: the functions of ID3D12GraphicsCommandList are shared by all the game's command lists, so patching them
// in the function table makes every list pass through our hooks; each hook looks only at the one list being captured and otherwise calls the
// original straight away. /ccdebug ui in Minecraft starts a capture of the next frame.

#include "UiProbe.hpp"
#include "Log.hpp"

#include <RED4ext/GpuApi/DeviceData.hpp>

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace cybercraft::uiprobe
{
	namespace
	{
		// Slots of ID3D12GraphicsCommandList's function table.
		constexpr UINT kReset = 10;
		constexpr UINT kDrawInstanced = 12;
		constexpr UINT kDrawIndexedInstanced = 13;
		constexpr UINT kDispatch = 14;
		constexpr UINT kCopyTextureRegion = 16;
		constexpr UINT kCopyResource = 17;
		constexpr UINT kRSSetViewports = 21;
		constexpr UINT kSetPipelineState = 25;
		constexpr UINT kResourceBarrier = 26;
		constexpr UINT kOMSetRenderTargets = 46;
		constexpr UINT kClearRenderTargetView = 48;
		constexpr UINT kExecuteIndirect = 59;
		// Slot of ID3D12Device's function table.
		constexpr UINT kCreateRenderTargetView = 20;
		// Slot of ID3D12CommandQueue's function table.
		constexpr UINT kExecuteCommandLists = 10;

		using List = ID3D12GraphicsCommandList;
		using ResetFn = HRESULT(STDMETHODCALLTYPE*)(List*, ID3D12CommandAllocator*, ID3D12PipelineState*);
		using ExecuteIndirectFn = void(STDMETHODCALLTYPE*)(List*, ID3D12CommandSignature*, UINT, ID3D12Resource*, UINT64, ID3D12Resource*, UINT64);
		using CreateRtvFn = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_RENDER_TARGET_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
		using DrawInstancedFn = void(STDMETHODCALLTYPE*)(List*, UINT, UINT, UINT, UINT);
		using DrawIndexedInstancedFn = void(STDMETHODCALLTYPE*)(List*, UINT, UINT, UINT, INT, UINT);
		using DispatchFn = void(STDMETHODCALLTYPE*)(List*, UINT, UINT, UINT);
		using CopyTextureRegionFn = void(STDMETHODCALLTYPE*)(List*, const D3D12_TEXTURE_COPY_LOCATION*, UINT, UINT, UINT, const D3D12_TEXTURE_COPY_LOCATION*, const D3D12_BOX*);
		using CopyResourceFn = void(STDMETHODCALLTYPE*)(List*, ID3D12Resource*, ID3D12Resource*);
		using RSSetViewportsFn = void(STDMETHODCALLTYPE*)(List*, UINT, const D3D12_VIEWPORT*);
		using SetPipelineStateFn = void(STDMETHODCALLTYPE*)(List*, ID3D12PipelineState*);
		using ResourceBarrierFn = void(STDMETHODCALLTYPE*)(List*, UINT, const D3D12_RESOURCE_BARRIER*);
		using OMSetRenderTargetsFn = void(STDMETHODCALLTYPE*)(List*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE*);
		using ClearRenderTargetViewFn = void(STDMETHODCALLTYPE*)(List*, D3D12_CPU_DESCRIPTOR_HANDLE, const FLOAT[4], UINT, const D3D12_RECT*);
		using ExecuteCommandListsFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

		RED4ext::v1::PluginHandle g_handle = nullptr;
		const RED4ext::v1::Sdk* g_sdk = nullptr;

		ResetFn g_origReset = nullptr;
		ExecuteIndirectFn g_origIndirect = nullptr;
		CreateRtvFn g_origCreateRtv = nullptr;
		DrawInstancedFn g_origDraw = nullptr;
		DrawIndexedInstancedFn g_origDrawIndexed = nullptr;
		DispatchFn g_origDispatch = nullptr;
		CopyTextureRegionFn g_origCopyTexture = nullptr;
		CopyResourceFn g_origCopyResource = nullptr;
		RSSetViewportsFn g_origViewports = nullptr;
		SetPipelineStateFn g_origSetPso = nullptr;
		ResourceBarrierFn g_origBarrier = nullptr;
		OMSetRenderTargetsFn g_origSetRT = nullptr;
		ClearRenderTargetViewFn g_origClearRT = nullptr;
		ExecuteCommandListsFn g_origExecute = nullptr;

		bool g_hooked = false;
		bool g_gaveUp = false;
		std::atomic<bool> g_armed{ false };
		std::chrono::steady_clock::time_point g_armedAt{};
		int g_detailFrom = -1;  // -1: short details of every list that matters; N: the whole of every list from position N on, and only the line of the lists before it
		std::atomic<bool> g_capturing{ false };
		std::atomic<std::uint32_t> g_captureId{ 0 };
		int g_presentsLeft = 0;
		std::atomic<ID3D12Resource*> g_back[8];  // the swapchain's buffers (not owned; only compared)
		constexpr int kFramesToRecord = 4;

		enum EventType : std::uint8_t
		{
			kBarrier,
			kSetPso,
			kDraw,
			kDrawIndexed,
			kDispatchEvent,
			kCopyTex,
			kCopyRes,
			kSetRT,
			kClearRT,
			kViewport,
			kIndirect,
		};

		// The size and format of a texture, packed: width (16 bits), height (16 bits), format (16 bits), flags (16 bits).
		std::uint64_t ResourceInfo(ID3D12Resource* a_resource)
		{
			if (!a_resource) {
				return 0;
			}
			const D3D12_RESOURCE_DESC d = a_resource->GetDesc();
			const std::uint64_t w = std::min<std::uint64_t>(d.Width, 0xFFFF);
			const std::uint64_t h = d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? 0 : std::min<std::uint64_t>(d.Height, 0xFFFF);
			return w | (h << 16) | (std::uint64_t(d.Format) << 32) | (std::uint64_t(d.Flags & 0xFFFF) << 48);
		}

		// The shape of a texture: depth or array size (16 bits), dimension (4 bits: 3 is a 2D texture, 4 a 3D one) and mip levels (8 bits).
		std::uint32_t ShapeInfo(ID3D12Resource* a_resource)
		{
			if (!a_resource) {
				return 0;
			}
			const D3D12_RESOURCE_DESC d = a_resource->GetDesc();
			return std::min<UINT>(d.DepthOrArraySize, 0xFFFF) | (static_cast<std::uint32_t>(d.Dimension) << 16) | (std::min<UINT>(d.MipLevels, 0xFF) << 20);
		}

		struct Event
		{
			std::uint8_t type;
			std::uint8_t flag;
			std::uint16_t pad;
			std::uint32_t a;
			std::uint32_t b;
			std::uint32_t c;
			std::uint64_t p;
			std::uint64_t q;
		};

		struct ListLog
		{
			List* list = nullptr;
			std::vector<Event> events;
			std::uint64_t lastRtRes = 0;   // the last texture moved into the render target state, and its info: the likely target of the next render target change
			std::uint64_t lastRtInfo = 0;
		};

		std::mutex g_mutex;
		std::unordered_map<List*, ListLog*> g_current;  // the latest recording of each command list
		std::vector<ListLog*> g_all;                    // every recording (owns them)
		struct Execution
		{
			List* list;        // null for a Present marker
			ListLog* log;      // the recording that was run (null: it was recorded before the capture began)
			std::uint32_t queueType;
		};
		struct RtvInfo
		{
			std::uint64_t resource;
			std::uint64_t info;
		};
		std::unordered_map<std::uint64_t, RtvInfo> g_rtvs;  // render target view descriptor -> the texture it points at
		std::atomic<bool> g_trackRtvs{ false };
		std::vector<Execution> g_executions;
		std::atomic<std::uint64_t> g_totalEvents{ 0 };
		constexpr std::uint64_t kMaxEvents = 2500000;

		thread_local List* t_list = nullptr;
		thread_local ListLog* t_log = nullptr;
		thread_local std::uint32_t t_captureId = 0;

		// The log of this command list, if a capture is going on (null if not, or if the capture is full).
		ListLog* LogFor(List* a_list)
		{
			if (!g_capturing.load(std::memory_order_relaxed)) {
				return nullptr;
			}
			const std::uint32_t id = g_captureId.load(std::memory_order_relaxed);
			if (t_captureId != id) {
				t_captureId = id;
				t_list = nullptr;
				t_log = nullptr;
			}
			if (t_list != a_list) {
				std::lock_guard lock(g_mutex);
				auto it = g_current.find(a_list);
				if (it == g_current.end()) {
					auto* fresh = new ListLog();
					fresh->list = a_list;
					g_all.push_back(fresh);
					it = g_current.emplace(a_list, fresh).first;
				}
				t_list = a_list;
				t_log = it->second;
			}
			if (g_totalEvents.load(std::memory_order_relaxed) >= kMaxEvents) {
				return nullptr;
			}
			return t_log;
		}

		inline void Add(ListLog* a_log, std::uint8_t a_type, std::uint32_t a_a = 0, std::uint32_t a_b = 0, std::uint32_t a_c = 0, std::uint64_t a_p = 0, std::uint64_t a_q = 0, std::uint8_t a_flag = 0)
		{
			a_log->events.push_back({ a_type, a_flag, 0, a_a, a_b, a_c, a_p, a_q });
			g_totalEvents.fetch_add(1, std::memory_order_relaxed);
		}

		bool IsBackBuffer(const ID3D12Resource* a_resource)
		{
			for (auto& b : g_back) {
				if (a_resource && b.load(std::memory_order_relaxed) == a_resource) {
					return true;
				}
			}
			return false;
		}

		// ---- the hooks ---------------------------------------------------------------------------------------------------------------

		void STDMETHODCALLTYPE HookBarrier(List* a_list, UINT a_count, const D3D12_RESOURCE_BARRIER* a_barriers)
		{
			if (ListLog* log = LogFor(a_list)) {
				for (UINT i = 0; i < a_count; ++i) {
					const auto& b = a_barriers[i];
					if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
						const auto& t = b.Transition;
						const std::uint64_t info = ResourceInfo(t.pResource);
						Add(log, kBarrier, t.StateBefore, t.StateAfter, ShapeInfo(t.pResource), reinterpret_cast<std::uint64_t>(t.pResource), info, IsBackBuffer(t.pResource) ? 1 : 0);
						if (t.StateAfter == D3D12_RESOURCE_STATE_RENDER_TARGET) {
							log->lastRtRes = reinterpret_cast<std::uint64_t>(t.pResource);
							log->lastRtInfo = info;
						}
					} else if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_UAV && b.UAV.pResource) {
						// A UAV barrier (the compute shaders that wrote it must finish before the next use): only for 3D textures, which is where fog and volumetric lighting live.
						const std::uint32_t shape = ShapeInfo(b.UAV.pResource);
						if (((shape >> 16) & 0xF) == static_cast<std::uint32_t>(D3D12_RESOURCE_DIMENSION_TEXTURE3D)) {
							Add(log, kBarrier, 0xFFFFFFFFu, 0xFFFFFFFFu, shape, reinterpret_cast<std::uint64_t>(b.UAV.pResource), ResourceInfo(b.UAV.pResource), 0);
						}
					}
				}
			}
			g_origBarrier(a_list, a_count, a_barriers);
		}

		void STDMETHODCALLTYPE HookDraw(List* a_list, UINT a_vertices, UINT a_instances, UINT a_startVertex, UINT a_startInstance)
		{
			if (ListLog* log = LogFor(a_list)) {
				Add(log, kDraw, a_vertices, a_instances);
			}
			g_origDraw(a_list, a_vertices, a_instances, a_startVertex, a_startInstance);
		}

		void STDMETHODCALLTYPE HookDrawIndexed(List* a_list, UINT a_indices, UINT a_instances, UINT a_startIndex, INT a_baseVertex, UINT a_startInstance)
		{
			if (ListLog* log = LogFor(a_list)) {
				Add(log, kDrawIndexed, a_indices, a_instances);
			}
			g_origDrawIndexed(a_list, a_indices, a_instances, a_startIndex, a_baseVertex, a_startInstance);
		}

		void STDMETHODCALLTYPE HookDispatch(List* a_list, UINT a_x, UINT a_y, UINT a_z)
		{
			if (ListLog* log = LogFor(a_list)) {
				Add(log, kDispatchEvent, a_x, a_y, a_z);
			}
			g_origDispatch(a_list, a_x, a_y, a_z);
		}

		void STDMETHODCALLTYPE HookCopyTexture(List* a_list, const D3D12_TEXTURE_COPY_LOCATION* a_dst, UINT a_x, UINT a_y, UINT a_z, const D3D12_TEXTURE_COPY_LOCATION* a_src, const D3D12_BOX* a_box)
		{
			if (ListLog* log = LogFor(a_list)) {
				if (a_dst && a_src) {
					Add(log, kCopyTex, 0, 0, 0, reinterpret_cast<std::uint64_t>(a_dst->pResource), reinterpret_cast<std::uint64_t>(a_src->pResource), IsBackBuffer(a_dst->pResource) ? 1 : 0);
					const std::uint64_t dstInfo = ResourceInfo(a_dst->pResource), srcInfo = ResourceInfo(a_src->pResource);
					log->events.back().a = static_cast<std::uint32_t>(dstInfo & 0xFFFFFFFFull);
					log->events.back().b = static_cast<std::uint32_t>(srcInfo & 0xFFFFFFFFull);
					log->events.back().c = static_cast<std::uint32_t>(((dstInfo >> 32) & 0xFFFF) | (((srcInfo >> 32) & 0xFFFF) << 16));
				}
			}
			g_origCopyTexture(a_list, a_dst, a_x, a_y, a_z, a_src, a_box);
		}

		void STDMETHODCALLTYPE HookCopyResource(List* a_list, ID3D12Resource* a_dst, ID3D12Resource* a_src)
		{
			if (ListLog* log = LogFor(a_list)) {
				const std::uint64_t dstInfo = ResourceInfo(a_dst), srcInfo = ResourceInfo(a_src);
				Add(log, kCopyRes, static_cast<std::uint32_t>(dstInfo & 0xFFFFFFFFull), static_cast<std::uint32_t>(srcInfo & 0xFFFFFFFFull),
					static_cast<std::uint32_t>(((dstInfo >> 32) & 0xFFFF) | (((srcInfo >> 32) & 0xFFFF) << 16)), reinterpret_cast<std::uint64_t>(a_dst), reinterpret_cast<std::uint64_t>(a_src), IsBackBuffer(a_dst) ? 1 : 0);
			}
			g_origCopyResource(a_list, a_dst, a_src);
		}

		void STDMETHODCALLTYPE HookViewports(List* a_list, UINT a_count, const D3D12_VIEWPORT* a_viewports)
		{
			if (ListLog* log = LogFor(a_list)) {
				if (a_count > 0 && a_viewports) {
					Add(log, kViewport, static_cast<std::uint32_t>(a_viewports[0].Width), static_cast<std::uint32_t>(a_viewports[0].Height), a_count);
				}
			}
			g_origViewports(a_list, a_count, a_viewports);
		}

		void STDMETHODCALLTYPE HookSetPso(List* a_list, ID3D12PipelineState* a_pso)
		{
			if (ListLog* log = LogFor(a_list)) {
				Add(log, kSetPso, 0, 0, 0, reinterpret_cast<std::uint64_t>(a_pso));
			}
			g_origSetPso(a_list, a_pso);
		}

		void STDMETHODCALLTYPE HookSetRT(List* a_list, UINT a_count, const D3D12_CPU_DESCRIPTOR_HANDLE* a_rtvs, BOOL a_single, const D3D12_CPU_DESCRIPTOR_HANDLE* a_dsv)
		{
			if (ListLog* log = LogFor(a_list)) {
				Add(log, kSetRT, a_count, static_cast<std::uint32_t>(log->lastRtRes & 0xFFFFFFFFull), static_cast<std::uint32_t>(log->lastRtRes >> 32),
					(a_rtvs && a_count > 0) ? static_cast<std::uint64_t>(a_rtvs[0].ptr) : 0ull, log->lastRtInfo, a_dsv ? 1 : 0);
			}
			g_origSetRT(a_list, a_count, a_rtvs, a_single, a_dsv);
		}

		void STDMETHODCALLTYPE HookClearRT(List* a_list, D3D12_CPU_DESCRIPTOR_HANDLE a_rtv, const FLOAT a_color[4], UINT a_rects, const D3D12_RECT* a_rectList)
		{
			if (ListLog* log = LogFor(a_list)) {
				Add(log, kClearRT, 0, 0, 0, static_cast<std::uint64_t>(a_rtv.ptr));
			}
			g_origClearRT(a_list, a_rtv, a_color, a_rects, a_rectList);
		}

		// A list that is Reset is recorded again: from here on its events belong to a new recording.
		HRESULT STDMETHODCALLTYPE HookReset(List* a_list, ID3D12CommandAllocator* a_allocator, ID3D12PipelineState* a_pso)
		{
			if (g_capturing.load(std::memory_order_relaxed)) {
				std::lock_guard lock(g_mutex);
				auto* fresh = new ListLog();
				fresh->list = a_list;
				g_all.push_back(fresh);
				g_current[a_list] = fresh;
				t_captureId = g_captureId.load(std::memory_order_relaxed);
				t_list = a_list;
				t_log = fresh;
			}
			return g_origReset(a_list, a_allocator, a_pso);
		}

		void STDMETHODCALLTYPE HookIndirect(List* a_list, ID3D12CommandSignature* a_signature, UINT a_max, ID3D12Resource* a_args, UINT64 a_argOffset, ID3D12Resource* a_count, UINT64 a_countOffset)
		{
			if (ListLog* log = LogFor(a_list)) {
				Add(log, kIndirect, a_max, 0, 0, 0, 0, a_count ? 1 : 0);
			}
			g_origIndirect(a_list, a_signature, a_max, a_args, a_argOffset, a_count, a_countOffset);
		}

		void STDMETHODCALLTYPE HookCreateRtv(ID3D12Device* a_device, ID3D12Resource* a_resource, const D3D12_RENDER_TARGET_VIEW_DESC* a_desc, D3D12_CPU_DESCRIPTOR_HANDLE a_dest)
		{
			if (a_resource && g_trackRtvs.load(std::memory_order_relaxed)) {
				const RtvInfo info{ reinterpret_cast<std::uint64_t>(a_resource), ResourceInfo(a_resource) };
				std::lock_guard lock(g_mutex);
				if (g_rtvs.size() > 200000) {
					g_rtvs.clear();
				}
				g_rtvs[static_cast<std::uint64_t>(a_dest.ptr)] = info;
			}
			g_origCreateRtv(a_device, a_resource, a_desc, a_dest);
		}

		// The order the GPU runs the command lists in: the order they are handed to the queues.
		void STDMETHODCALLTYPE HookExecute(ID3D12CommandQueue* a_queue, UINT a_count, ID3D12CommandList* const* a_lists)
		{
			if (g_capturing.load(std::memory_order_relaxed) && a_lists) {
				const std::uint32_t type = static_cast<std::uint32_t>(a_queue->GetDesc().Type);
				std::lock_guard lock(g_mutex);
				for (UINT i = 0; i < a_count; ++i) {
					List* list = static_cast<List*>(a_lists[i]);
					auto it = g_current.find(list);
					g_executions.push_back({ list, it == g_current.end() ? nullptr : it->second, type });
				}
			}
			g_origExecute(a_queue, a_count, a_lists);
		}

		// ---- installing the hooks ----------------------------------------------------------------------------------------------------

		void Patch(void** a_vtable, UINT a_index, void* a_hook, void** a_original)
		{
			void** slot = &a_vtable[a_index];
			DWORD oldProtect = 0;
			if (::VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
				*a_original = *slot;
				*slot = a_hook;
				::VirtualProtect(slot, sizeof(void*), oldProtect, &oldProtect);
			}
		}

		bool InstallHooks()
		{
			auto* data = RED4ext::GpuApi::GetDeviceData();
			if (!data || !data->device) {
				return false;
			}
			Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
			Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
			Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
			D3D12_COMMAND_QUEUE_DESC queueDesc{};
			queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
			if (FAILED(data->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)))
				|| FAILED(data->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)))
				|| FAILED(data->device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)))) {
				return false;
			}
			void** listTable = *reinterpret_cast<void***>(list.Get());
			void** queueTable = *reinterpret_cast<void***>(queue.Get());
			void** deviceTable = *reinterpret_cast<void***>(data->device.Get());
			Patch(listTable, kReset, reinterpret_cast<void*>(&HookReset), reinterpret_cast<void**>(&g_origReset));
			Patch(listTable, kExecuteIndirect, reinterpret_cast<void*>(&HookIndirect), reinterpret_cast<void**>(&g_origIndirect));
			Patch(deviceTable, kCreateRenderTargetView, reinterpret_cast<void*>(&HookCreateRtv), reinterpret_cast<void**>(&g_origCreateRtv));
			Patch(listTable, kDrawInstanced, reinterpret_cast<void*>(&HookDraw), reinterpret_cast<void**>(&g_origDraw));
			Patch(listTable, kDrawIndexedInstanced, reinterpret_cast<void*>(&HookDrawIndexed), reinterpret_cast<void**>(&g_origDrawIndexed));
			Patch(listTable, kDispatch, reinterpret_cast<void*>(&HookDispatch), reinterpret_cast<void**>(&g_origDispatch));
			Patch(listTable, kCopyTextureRegion, reinterpret_cast<void*>(&HookCopyTexture), reinterpret_cast<void**>(&g_origCopyTexture));
			Patch(listTable, kCopyResource, reinterpret_cast<void*>(&HookCopyResource), reinterpret_cast<void**>(&g_origCopyResource));
			Patch(listTable, kRSSetViewports, reinterpret_cast<void*>(&HookViewports), reinterpret_cast<void**>(&g_origViewports));
			Patch(listTable, kSetPipelineState, reinterpret_cast<void*>(&HookSetPso), reinterpret_cast<void**>(&g_origSetPso));
			Patch(listTable, kResourceBarrier, reinterpret_cast<void*>(&HookBarrier), reinterpret_cast<void**>(&g_origBarrier));
			Patch(listTable, kOMSetRenderTargets, reinterpret_cast<void*>(&HookSetRT), reinterpret_cast<void**>(&g_origSetRT));
			Patch(listTable, kClearRenderTargetView, reinterpret_cast<void*>(&HookClearRT), reinterpret_cast<void**>(&g_origClearRT));
			Patch(queueTable, kExecuteCommandLists, reinterpret_cast<void*>(&HookExecute), reinterpret_cast<void**>(&g_origExecute));
			list->Close();
			g_trackRtvs.store(true);
			const bool all = g_origReset && g_origIndirect && g_origCreateRtv && g_origDraw && g_origDrawIndexed && g_origDispatch && g_origCopyTexture && g_origCopyResource && g_origViewports && g_origSetPso && g_origBarrier && g_origSetRT
				&& g_origClearRT && g_origExecute;
			g_sdk->logger->InfoF(g_handle, "ui: hooked the game's command lists (function table %p) and queues (%p)%s", static_cast<void*>(listTable), static_cast<void*>(queueTable),
				all ? "" : "; SOME HOOKS FAILED");
			return all;
		}

		// ---- the summary -------------------------------------------------------------------------------------------------------------

		std::string StateText(std::uint32_t a_state)
		{
			if (a_state == 0) {
				return "PRESENT";
			}
			struct Bit
			{
				std::uint32_t value;
				const char* name;
			};
			static const Bit kBits[] = { { 0x1, "VERTEX/CBV" }, { 0x2, "INDEX" }, { 0x4, "RENDER_TARGET" }, { 0x8, "UAV" }, { 0x10, "DEPTH_WRITE" }, { 0x20, "DEPTH_READ" }, { 0x40, "NON_PIXEL_SRV" },
				{ 0x80, "PIXEL_SRV" }, { 0x100, "STREAM_OUT" }, { 0x200, "INDIRECT" }, { 0x400, "COPY_DEST" }, { 0x800, "COPY_SOURCE" }, { 0x1000, "RESOLVE_DEST" }, { 0x2000, "RESOLVE_SOURCE" } };
			std::string text;
			for (const Bit& b : kBits) {
				if (a_state & b.value) {
					text += text.empty() ? "" : "|";
					text += b.name;
				}
			}
			if (text.empty()) {
				char hex[16];
				std::snprintf(hex, sizeof(hex), "0x%X", a_state);
				return hex;
			}
			return text;
		}

		const char* FormatName(std::uint32_t a_format)
		{
			switch (a_format) {
			case 2: return "R32G32B32A32_FLOAT";
			case 10: return "R16G16B16A16_FLOAT";
			case 11: return "R16G16B16A16_UNORM";
			case 24: return "R10G10B10A2_UNORM";
			case 26: return "R11G11B10_FLOAT";
			case 27: return "R8G8B8A8_TYPELESS";
			case 28: return "R8G8B8A8_UNORM";
			case 29: return "R8G8B8A8_UNORM_SRGB";
			case 34: return "R16G16_FLOAT";
			case 41: return "R32_FLOAT";
			case 54: return "R16_FLOAT";
			case 61: return "R8_UNORM";
			case 87: return "B8G8R8A8_UNORM";
			case 91: return "B8G8R8A8_UNORM_SRGB";
			case 0: return "(buffer)";
			default: return nullptr;
			}
		}

		std::string InfoText(std::uint64_t a_info)
		{
			const unsigned w = static_cast<unsigned>(a_info & 0xFFFF), h = static_cast<unsigned>((a_info >> 16) & 0xFFFF), f = static_cast<unsigned>((a_info >> 32) & 0xFFFF);
			char text[96];
			if (h == 0) {
				std::snprintf(text, sizeof(text), "buffer %u bytes", w);
			} else if (const char* name = FormatName(f)) {
				std::snprintf(text, sizeof(text), "%ux%u %s", w, h, name);
			} else {
				std::snprintf(text, sizeof(text), "%ux%u format %u", w, h, f);
			}
			return text;
		}

		bool ScreenSized(std::uint64_t a_info)
		{
			const unsigned w = static_cast<unsigned>(a_info & 0xFFFF), h = static_cast<unsigned>((a_info >> 16) & 0xFFFF);
			return w >= 960 && h >= 540;
		}

		const char* QueueName(std::uint32_t a_type)
		{
			return a_type == 0 ? "DIRECT" : a_type == 1 ? "BUNDLE" : a_type == 2 ? "COMPUTE" : a_type == 3 ? "COPY" : "?";
		}

		struct Run
		{
			std::uint64_t pso = 0;
			bool indexed = false;
			std::uint32_t draws = 0;
			std::uint64_t first = 0, last = 0;
			std::uint64_t elements = 0;
			std::uint32_t maxInstances = 0;
			std::uint32_t smallest = 0xFFFFFFFFu, largest = 0;
		};

		// The texture a render target descriptor points at, if we saw the view being made.
		std::string TargetText(std::uint64_t a_descriptor, std::uint64_t a_hintInfo)
		{
			auto it = g_rtvs.find(a_descriptor);
			char text[200];
			if (it != g_rtvs.end()) {
				std::snprintf(text, sizeof(text), "%p (%s)", reinterpret_cast<void*>(it->second.resource), InfoText(it->second.info).c_str());
			} else {
				std::snprintf(text, sizeof(text), "unknown texture (its view was made before we looked; the texture last moved into RENDER_TARGET in this list is %s)", InfoText(a_hintInfo).c_str());
			}
			return text;
		}

		void DumpList(const Execution& a_exec, const ListLog& a_log, int a_position)
		{
			const bool full = g_detailFrom >= 0 && a_position >= g_detailFrom;
			const bool linesOnly = g_detailFrom >= 0 && a_position < g_detailFrom;
			std::uint64_t draws = 0, indexed = 0, indirect = 0, dispatches = 0, copies = 0, barriers = 0, targets = 0, clears = 0;
			bool takesBackBuffer = false;
			std::vector<std::uint64_t> psos;
			std::string viewports, targetNames;
			std::vector<std::uint64_t> seenTargets;
			std::uint32_t lastW = 0, lastH = 0;
			for (const Event& e : a_log.events) {
				switch (e.type) {
				case kDraw: ++draws; break;
				case kDrawIndexed: ++draws; ++indexed; break;
				case kIndirect: ++indirect; break;
				case kDispatchEvent: ++dispatches; break;
				case kCopyTex:
				case kCopyRes: ++copies; break;
				case kBarrier: ++barriers; if (e.flag && e.a == 0) { takesBackBuffer = true; } break;
				case kSetRT:
					++targets;
					if (std::find(seenTargets.begin(), seenTargets.end(), e.p) == seenTargets.end() && seenTargets.size() < 4) {
						seenTargets.push_back(e.p);
						auto it = g_rtvs.find(e.p);
						targetNames += it != g_rtvs.end() ? InfoText(it->second.info) + "; " : std::string("unknown; ");
					}
					break;
				case kClearRT: ++clears; break;
				case kSetPso:
					if (std::find(psos.begin(), psos.end(), e.p) == psos.end()) {
						psos.push_back(e.p);
					}
					break;
				case kViewport:
					if ((e.a != lastW || e.b != lastH) && viewports.size() < 90) {
						char t[32];
						std::snprintf(t, sizeof(t), "%ux%u ", e.a, e.b);
						viewports += t;
					}
					lastW = e.a;
					lastH = e.b;
					break;
				default: break;
				}
			}
			g_sdk->logger->InfoF(g_handle,
				"ui: list %d [%s]%s: %u events; %llu draws (%llu indexed) in %u pipelines, %llu indirect draws, %llu dispatches, %llu copies, %llu barriers, %llu render target sets, %llu clears; viewports: %s; render targets: %s",
				a_position, QueueName(a_exec.queueType), takesBackBuffer ? " <-- TAKES THE BACK BUFFER OUT OF PRESENT" : "", static_cast<unsigned>(a_log.events.size()), static_cast<unsigned long long>(draws),
				static_cast<unsigned long long>(indexed), static_cast<unsigned>(psos.size()), static_cast<unsigned long long>(indirect), static_cast<unsigned long long>(dispatches),
				static_cast<unsigned long long>(copies), static_cast<unsigned long long>(barriers), static_cast<unsigned long long>(targets), static_cast<unsigned long long>(clears),
				viewports.empty() ? "none" : viewports.c_str(), targetNames.empty() ? "none" : targetNames.c_str());

			// The details: for lists that matter (many draws, or screen-sized textures moving about).
			if (linesOnly) {
				return;
			}
			bool notable = full || draws >= 20 || indirect >= 3;
			for (const Event& e : a_log.events) {
				if ((e.type == kBarrier && ScreenSized(e.q)) || ((e.type == kCopyTex || e.type == kCopyRes) && (ScreenSized(e.a) || ScreenSized(e.b)))) {
					notable = true;
					break;
				}
			}
			if (!notable) {
				return;
			}
			int lines = 0;
			const int kMaxListLines = full ? 4000 : 70;
			std::uint64_t drawNumber = 0, pso = 0;
			Run run;
			bool haveRun = false;
			auto flush = [&]() {
				if (!haveRun) {
					return;
				}
				haveRun = false;
				if (lines++ < kMaxListLines) {
					g_sdk->logger->InfoF(g_handle, "ui:     draws %llu-%llu: %u %s, pipeline %p, %llu %s in all (per draw %u to %u), up to %u instances", static_cast<unsigned long long>(run.first),
						static_cast<unsigned long long>(run.last), run.draws, run.indexed ? "indexed" : "plain", reinterpret_cast<void*>(run.pso), static_cast<unsigned long long>(run.elements),
						run.indexed ? "indices" : "vertices", run.smallest, run.largest, run.maxInstances);
				}
			};
			auto line = [&](const char* a_fmt, auto... a_args) {
				flush();
				if (lines++ < kMaxListLines) {
					g_sdk->logger->InfoF(g_handle, a_fmt, a_args...);
				}
			};
			for (const Event& e : a_log.events) {
				switch (e.type) {
				case kSetPso:
					pso = e.p;
					break;
				case kDraw:
				case kDrawIndexed: {
					const bool isIndexed = e.type == kDrawIndexed;
					if (!haveRun || run.pso != pso || run.indexed != isIndexed) {
						flush();
						run = Run{};
						run.pso = pso;
						run.indexed = isIndexed;
						run.first = drawNumber;
						haveRun = true;
					}
					++run.draws;
					run.last = drawNumber;
					run.elements += e.a;
					run.maxInstances = std::max(run.maxInstances, e.b);
					run.smallest = std::min(run.smallest, e.a);
					run.largest = std::max(run.largest, e.a);
					++drawNumber;
					break;
				}
				case kIndirect:
					line("ui:     [draw %llu] indirect draw, up to %u commands%s", static_cast<unsigned long long>(drawNumber), e.a, e.flag ? " (the count is read from a buffer)" : "");
					break;
				case kBarrier:
					if (ScreenSized(e.q) || e.flag) {
						line("ui:     [draw %llu] barrier %p (%s): %s -> %s%s", static_cast<unsigned long long>(drawNumber), reinterpret_cast<void*>(e.p), InfoText(e.q).c_str(), StateText(e.a).c_str(),
							StateText(e.b).c_str(), e.flag ? "   <-- THE BACK BUFFER" : "");
					}
					break;
				case kSetRT:
					line("ui:     [draw %llu] render target set (%u): descriptor 0x%llx = %s", static_cast<unsigned long long>(drawNumber), e.a, static_cast<unsigned long long>(e.p),
						TargetText(e.p, e.q).c_str());
					break;
				case kClearRT:
					line("ui:     [draw %llu] clear render target, descriptor 0x%llx = %s", static_cast<unsigned long long>(drawNumber), static_cast<unsigned long long>(e.p),
						TargetText(e.p, 0).c_str());
					break;
				case kDispatchEvent:
					line("ui:     [draw %llu] dispatch %u x %u x %u", static_cast<unsigned long long>(drawNumber), e.a, e.b, e.c);
					break;
				case kCopyTex:
				case kCopyRes:
					line("ui:     [draw %llu] copy %p (%s) -> %p (%s)%s", static_cast<unsigned long long>(drawNumber), reinterpret_cast<void*>(e.q),
						InfoText(std::uint64_t(e.b) | (std::uint64_t(e.c >> 16) << 32)).c_str(), reinterpret_cast<void*>(e.p), InfoText(std::uint64_t(e.a) | (std::uint64_t(e.c & 0xFFFF) << 32)).c_str(),
						e.flag ? "   <-- INTO THE BACK BUFFER" : "");
					break;
				default:
					break;
				}
			}
			flush();
			if (lines > kMaxListLines) {
				g_sdk->logger->InfoF(g_handle, "ui:     ... %d more lines for this list left out", lines - kMaxListLines);
			}
		}

		void FreeRecordings()
		{
			for (ListLog* log : g_all) {
				delete log;
			}
			g_all.clear();
			g_current.clear();
			g_executions.clear();
			g_totalEvents.store(0);
		}

		void Dump()
		{
			::Sleep(150);  // threads that were inside a hook when the capture ended finish their last write
			std::lock_guard lock(g_mutex);
			unsigned runs = 0, recorded = 0;
			std::vector<int> markers;
			for (int i = 0; i < static_cast<int>(g_executions.size()); ++i) {
				if (!g_executions[i].list) {
					markers.push_back(i);
				} else {
					++runs;
					recorded += g_executions[i].log ? 1 : 0;
				}
			}
			g_sdk->logger->InfoF(g_handle, "ui: recorded %llu events in %u recordings of command lists; %u runs of lists were handed to the queues (%u of them recorded in this capture) in %u presents; %u render target views seen",
				static_cast<unsigned long long>(g_totalEvents.load()), static_cast<unsigned>(g_all.size()), runs, recorded, static_cast<unsigned>(markers.size()), static_cast<unsigned>(g_rtvs.size()));
			int start = 0, end = static_cast<int>(g_executions.size()) - 1;
			if (markers.size() >= 2) {
				start = markers[markers.size() - 2] + 1;
				end = markers[markers.size() - 1] - 1;
			}
			g_sdk->logger->InfoF(g_handle, "ui: the last whole frame between two presents: %d runs of command lists, in the order they were run:", end - start + 1);
			int position = 0;
			for (int i = start; i <= end; ++i) {
				const Execution& x = g_executions[i];
				if (!x.list) {
					continue;
				}
				if (x.log) {
					DumpList(x, *x.log, position);
				} else {
					g_sdk->logger->InfoF(g_handle, "ui: list %d [%s]: list %p was not recorded in this capture (it was recorded earlier and is run again: a list the game records once)", position, QueueName(x.queueType),
						static_cast<void*>(x.list));
				}
				++position;
			}
			// The 3D textures of the frame: where the game's fog, volumetric lighting and colour grading tables should show up.
			{
				struct Use
				{
					int position;
					std::uint32_t before, after;
				};
				struct Volume
				{
					std::uint64_t pointer, info;
					std::uint32_t shape;
					std::vector<Use> uses;
				};
				std::vector<Volume> volumes;
				int pos = 0;
				for (int i = start; i <= end; ++i) {
					const Execution& x = g_executions[i];
					if (!x.list) {
						continue;
					}
					if (x.log) {
						for (const Event& e : x.log->events) {
							if (e.type != kBarrier || ((e.c >> 16) & 0xF) != static_cast<std::uint32_t>(D3D12_RESOURCE_DIMENSION_TEXTURE3D)) {
								continue;
							}
							Volume* v = nullptr;
							for (Volume& candidate : volumes) {
								if (candidate.pointer == e.p) {
									v = &candidate;
									break;
								}
							}
							if (!v) {
								volumes.push_back({ e.p, e.q, e.c, {} });
								v = &volumes.back();
							}
							if (v->uses.size() < 40) {
								v->uses.push_back({ pos, e.a, e.b });
							}
						}
					}
					++pos;
				}
				g_sdk->logger->InfoF(g_handle, "ui: %u 3D textures were used in this frame (the game's fog, volumetric lighting and colour grading tables are 3D textures):", static_cast<unsigned>(volumes.size()));
				for (const Volume& v : volumes) {
					const unsigned w = static_cast<unsigned>(v.info & 0xFFFF), h = static_cast<unsigned>((v.info >> 16) & 0xFFFF), f = static_cast<unsigned>((v.info >> 32) & 0xFFFF);
					const unsigned d = v.shape & 0xFFFF, mips = (v.shape >> 20) & 0xFF;
					const char* fmt = FormatName(f);
					g_sdk->logger->InfoF(g_handle, "ui:   3D texture %p: %u x %u x %u, %s%s, %u mip levels", reinterpret_cast<void*>(v.pointer), w, h, d, fmt ? fmt : "format ", fmt ? "" : std::to_string(f).c_str(), mips);
					for (const Use& u : v.uses) {
						if (u.before == 0xFFFFFFFFu) {
							g_sdk->logger->InfoF(g_handle, "ui:       list %d: UAV barrier (its compute writes are made visible)", u.position);
						} else {
							g_sdk->logger->InfoF(g_handle, "ui:       list %d: %s -> %s", u.position, StateText(u.before).c_str(), StateText(u.after).c_str());
						}
					}
				}
			}
			g_sdk->logger->Info(g_handle, "ui: end of the capture");
			FreeRecordings();
		}

		void UpdateBackBuffers(IDXGISwapChain* a_swapChain)
		{
			DXGI_SWAP_CHAIN_DESC desc{};
			if (FAILED(a_swapChain->GetDesc(&desc))) {
				return;
			}
			const UINT count = std::min<UINT>(desc.BufferCount, 8);
			for (UINT i = 0; i < 8; ++i) {
				ID3D12Resource* buffer = nullptr;
				if (i < count && SUCCEEDED(a_swapChain->GetBuffer(i, IID_PPV_ARGS(&buffer))) && buffer) {
					g_back[i].store(buffer);
					buffer->Release();  // only the address is kept, to compare with
				} else {
					g_back[i].store(nullptr);
				}
			}
		}
	}

	void Init(RED4ext::v1::PluginHandle a_handle, const RED4ext::v1::Sdk* a_sdk)
	{
		g_handle = a_handle;
		g_sdk = a_sdk;
	}

	void Request(int a_detailFrom)
	{
		g_detailFrom = a_detailFrom;
		if (g_gaveUp) {
			g_sdk->logger->Warn(g_handle, "ui: the hooks failed earlier; restart the game to try again");
			return;
		}
		if (g_capturing.load()) {
			return;
		}
		g_armedAt = std::chrono::steady_clock::now();
		g_armed.store(true);
		g_sdk->logger->InfoF(g_handle, "ui: probe armed: the next four frames will be recorded (the game may hitch for a moment)%s", a_detailFrom >= 0 ? "; the lists from the given position on will be printed in full" : "");
	}

	void OnPresent(IDXGISwapChain* a_swapChain)
	{
		if (!g_armed.load() && !g_capturing.load()) {
			return;
		}
		UpdateBackBuffers(a_swapChain);
		if (!g_hooked) {
			if (!InstallHooks()) {
				if (std::chrono::steady_clock::now() - g_armedAt > std::chrono::seconds(5)) {
					g_armed.store(false);
					g_gaveUp = true;
					g_sdk->logger->Error(g_handle, "ui: could not hook the game's command lists");
				}
				return;
			}
			g_hooked = true;
		}
		if (g_armed.load()) {
			// Start: from now until four presents have gone by.
			g_armed.store(false);
			{
				std::lock_guard lock(g_mutex);
				FreeRecordings();
				g_executions.push_back({ nullptr, nullptr, 0 });
			}
			g_captureId.fetch_add(1);
			g_presentsLeft = kFramesToRecord;
			g_capturing.store(true);
			return;
		}
		if (g_capturing.load()) {
			std::lock_guard lock(g_mutex);
			g_executions.push_back({ nullptr, nullptr, 0 });  // a Present marks the end of a frame
		}
		if (g_capturing.load() && --g_presentsLeft <= 0) {
			g_capturing.store(false);
			Dump();
		}
	}
}
