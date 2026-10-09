// Drawing into the game's own scene. (Mode 1 to 3 draw a square, the proof of concept, which worked; mode 4 draws Minecraft's blocks.)
//
// Everything the game does after the scene is complete (bloom, exposure, tone mapping, colour grading, film grain, and laying the interface
// over the picture) is applied to the game's HDR scene texture. If Minecraft's blocks are drawn *into* that texture at the right moment instead of
// onto the finished picture, all of that applies to them too, and the game puts its own interface on top of them.
//
// Where is the right moment? The captures (/ccdebug ui) showed that the list that follows the last scene drawing begins by copying the scene's
// HDR texture (1920 x 1080, R16G16B16A16_FLOAT) into a second texture of the same kind (a copy that glass and refraction read), and does no drawing of its
// own. So the plan is:
//   * while the game records its command lists, notice the one that copies one screen-sized R16G16B16A16_FLOAT texture into another, and what
//     state the source texture is in when that list begins (learnt from the first barrier that names it);
//   * when the game hands that list to the queue, hand the queue a small list of our own just before it: it moves the scene texture to render
//     target, draws, and moves it back to the state the game expects. (ExecuteCommandLists is called with the lists before it first, then ours, then
//     the rest: the same order of work, with ours slotted in.)
//
// The proof of concept draws a bright square in the middle of the screen. If it glows (bloom), is tone mapped, and is *under* the game's interface,
// the route works. Mode 0 is off; modes 1 to 3 are a dim, medium and very bright square.
//
// Robustness: the copy is not necessarily done every frame (it exists for glass and refraction), and the barrier that tells us the scene texture's state may be missing from the list that copies it. So the state is
// remembered once learnt (it is the same every frame), and when drawing stops the plugin says what it saw instead (and, for the lists of a batch that touch the scene texture, what they did with it).
//
// Safety: our own lists are skipped by every hook; if the pattern isn't found, nothing is changed; the game's command lists are never altered.

#include "InScene.hpp"
#include "Log.hpp"
#include "Depth.hpp"
#include "Overlay.hpp"

#include <RED4ext/GpuApi/DeviceData.hpp>

#include <d3d12.h>
#include <d3dcompiler.h>
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

namespace cybercraft::inscene
{
	namespace
	{
		using Microsoft::WRL::ComPtr;
		using List = ID3D12GraphicsCommandList;

		// Slots of ID3D12GraphicsCommandList's and ID3D12CommandQueue's function tables.
		constexpr UINT kReset = 10;
		constexpr UINT kCopyTextureRegion = 16;
		constexpr UINT kCopyResource = 17;
		constexpr UINT kResourceBarrier = 26;
		constexpr UINT kExecuteCommandLists = 10;

		using ResetFn = HRESULT(STDMETHODCALLTYPE*)(List*, ID3D12CommandAllocator*, ID3D12PipelineState*);
		using CopyTextureRegionFn = void(STDMETHODCALLTYPE*)(List*, const D3D12_TEXTURE_COPY_LOCATION*, UINT, UINT, UINT, const D3D12_TEXTURE_COPY_LOCATION*, const D3D12_BOX*);
		using CopyResourceFn = void(STDMETHODCALLTYPE*)(List*, ID3D12Resource*, ID3D12Resource*);
		using ResourceBarrierFn = void(STDMETHODCALLTYPE*)(List*, UINT, const D3D12_RESOURCE_BARRIER*);
		using ExecuteCommandListsFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

		RED4ext::v1::PluginHandle g_handle = nullptr;
		const RED4ext::v1::Sdk* g_sdk = nullptr;

		ResetFn g_origReset = nullptr;
		CopyTextureRegionFn g_origCopyTexture = nullptr;
		CopyResourceFn g_origCopyResource = nullptr;
		ResourceBarrierFn g_origBarrier = nullptr;
		ExecuteCommandListsFn g_origExecute = nullptr;

		std::atomic<int> g_mode{ 0 };
		std::atomic<UINT> g_screenW{ 0 };
		std::atomic<UINT> g_screenH{ 0 };
		bool g_hooked = false;
		bool g_gaveUp = false;

		// ---- what the game records -------------------------------------------------------------------------------------------------
		struct First
		{
			ID3D12Resource* resource;
			std::uint32_t state;  // the state the first barrier that names it moves it out of: its state when the list begins
		};
		struct Tag
		{
			ID3D12Resource* source = nullptr;
			ID3D12Resource* destination = nullptr;
			std::uint32_t state = 0;
			bool haveState = false;
		};
		struct Touch
		{
			char kind;           // 'B' a barrier, 'S' a copy out of the scene texture, 'D' a copy into it
			std::uint32_t a, b;  // the states a barrier moves between
		};
		struct VolTouch
		{
			ID3D12Resource* res;
			std::uint32_t after;  // the state subresource 0 of a froxel volume is moved into
		};
		struct VolState
		{
			std::uint32_t state;
			std::uint64_t seen;  // the insertion count when it was last moved
			std::uint32_t uav = 0;  // how many times it was moved into the unordered access state (written by compute) since the last insertion
		};
		struct ListInfo
		{
			std::vector<VolTouch> volumes;  // what this recording does with the game's 3D fog textures, to be replayed in the order the GPU runs the lists
			std::vector<std::uint32_t> depthStates;  // the states the game's main depth texture is moved into (all subresources), likewise
			Tag tag;
			std::vector<First> firsts;
			std::vector<Touch> touches;  // what this recording does with the scene texture we last knew (for the report when drawing stops)
		};
		std::uint32_t g_depthState = 0;        // the state of the game's main depth texture as of the lists replayed so far (under g_mutex)
		bool g_haveDepthState = false;
		ID3D12Resource* g_depthResource = nullptr;
		std::unordered_map<ID3D12Resource*, VolState> g_volStates;  // the game's 3D fog textures and the state each is in, as of the lists replayed so far (under g_mutex)
		std::atomic<ID3D12Resource*> g_knownSource{ nullptr };  // the scene texture as last found
		std::atomic<std::uint32_t> g_knownState{ 0 };           // the state it is in when the scene-copy list begins (learnt once, the same every frame)
		std::atomic<bool> g_haveKnownState{ false };
		std::atomic<std::uint64_t> g_copyMatches{ 0 };          // copies of the pattern seen while recording
		std::atomic<std::uint64_t> g_noStateTags{ 0 };          // scene-copy lists handed to the queue whose state was not known
		std::uint64_t g_matchesAtDraw = 0;
		std::uint64_t g_noStateAtDraw = 0;
		std::atomic<std::int64_t> g_dumpUntilMs{ 0 };           // while drawing has stopped: the next batch that touches the scene texture is described in the log
		std::int64_t g_lastDumpMs = 0;
		std::mutex g_mutex;
		std::unordered_map<List*, ListInfo> g_lists;

		// ---- our own lists ---------------------------------------------------------------------------------------------------------
		constexpr int kSlots = 3;
		struct Slot
		{
			ComPtr<ID3D12CommandAllocator> allocator;
			ComPtr<ID3D12GraphicsCommandList> list;
			std::uint64_t fenceValue = 0;
		};
		Slot g_slots[kSlots];
		ComPtr<ID3D12Device> g_device;
		ComPtr<ID3D12Fence> g_fence;
		std::uint64_t g_fenceValue = 0;
		ComPtr<ID3D12RootSignature> g_rootSignature;
		ComPtr<ID3D12PipelineState> g_pso;
		ComPtr<ID3D12DescriptorHeap> g_rtvHeap;
		UINT g_rtvSize = 0;
		int g_slotIndex = 0;
		bool g_gpuReady = false;
		bool g_gpuFailed = false;
		std::atomic<std::uint64_t> g_inserted{ 0 };
		std::atomic<std::uint64_t> g_skipped{ 0 };
		bool g_foundLogged = false;
		std::atomic<std::int64_t> g_lastDrawMs{ 0 };        // when the blocks (or the square) were last drawn into the scene
		std::atomic<const char*> g_lastFailure{ "" };       // why the last attempt drew nothing
		bool g_wasDrawing = false;
		bool g_everDrew = false;

		std::int64_t NowMs()
		{
			return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
		}
		std::chrono::steady_clock::time_point g_armedAt{};

		const char* kShader = R"HLSL(
cbuffer C : register(b0) { float4 colour; };
struct VSOut { float4 pos : SV_Position; };
VSOut VS(uint id : SV_VertexID)
{
	VSOut o;
	float2 uv = float2((id << 1) & 2, id & 2);
	o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
	return o;
}
float4 PS(VSOut i) : SV_Target { return float4(colour.rgb, 1.0); }
)HLSL";

		std::string StateName(std::uint32_t a_state)
		{
			if (a_state == 0) {
				return "PRESENT/COMMON";
			}
			std::string text;
			const struct { std::uint32_t v; const char* n; } bits[] = { { 0x1, "VERTEX/CBV" }, { 0x4, "RENDER_TARGET" }, { 0x8, "UAV" }, { 0x40, "NON_PIXEL_SRV" }, { 0x80, "PIXEL_SRV" }, { 0x400, "COPY_DEST" }, { 0x800, "COPY_SOURCE" } };
			for (const auto& b : bits) {
				if (a_state & b.v) {
					text += text.empty() ? "" : "|";
					text += b.n;
				}
			}
			char hex[16];
			std::snprintf(hex, sizeof(hex), " (0x%X)", a_state);
			return text + hex;
		}

		bool IsOurs(const List* a_list)
		{
			for (const Slot& s : g_slots) {
				if (s.list.Get() == a_list) {
					return true;
				}
			}
			return false;
		}

		// ---- the hooks -------------------------------------------------------------------------------------------------------------
		bool Active()
		{
			return g_mode.load(std::memory_order_relaxed) > 0;
		}

		HRESULT STDMETHODCALLTYPE HookReset(List* a_list, ID3D12CommandAllocator* a_allocator, ID3D12PipelineState* a_pso)
		{
			if (Active() && !IsOurs(a_list)) {
				std::lock_guard lock(g_mutex);
				g_lists.erase(a_list);  // a new recording begins
			}
			return g_origReset(a_list, a_allocator, a_pso);
		}

		void NoteBarriers(List* a_list, UINT a_count, const D3D12_RESOURCE_BARRIER* a_barriers)
		{
			std::lock_guard lock(g_mutex);
			ListInfo& info = g_lists[a_list];
			for (UINT i = 0; i < a_count; ++i) {
				const D3D12_RESOURCE_BARRIER& b = a_barriers[i];
				if (b.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION || !b.Transition.pResource) {
					continue;
				}
				ID3D12Resource* res = b.Transition.pResource;
				bool known = false;
				for (const First& f : info.firsts) {
					if (f.resource == res) {
						known = true;
						break;
					}
				}
				if (!known && info.firsts.size() < 400) {
					info.firsts.push_back({ res, static_cast<std::uint32_t>(b.Transition.StateBefore) });
				}
				if (b.Transition.Subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES && res == depth::CandidateResource() && info.depthStates.size() < 32) {
					info.depthStates.push_back(static_cast<std::uint32_t>(b.Transition.StateAfter));
				}
				// The game's volumetric fog lives in 3D textures of float colour (the screen divided by 8, 128 slices deep): their state is tracked so that one can be read safely.
				if ((b.Transition.Subresource == 0 || b.Transition.Subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) && info.volumes.size() < 32) {
					const D3D12_RESOURCE_DESC d = res->GetDesc();
					if (d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D && d.Format == DXGI_FORMAT_R16G16B16A16_FLOAT && d.DepthOrArraySize >= 96 && d.Width >= 128) {
						info.volumes.push_back({ res, static_cast<std::uint32_t>(b.Transition.StateAfter) });
					}
				}
				if (res == g_knownSource.load(std::memory_order_relaxed) && info.touches.size() < 24) {
					info.touches.push_back({ 'B', static_cast<std::uint32_t>(b.Transition.StateBefore), static_cast<std::uint32_t>(b.Transition.StateAfter) });
				}
				if (info.tag.source == res && !info.tag.haveState) {
					info.tag.state = static_cast<std::uint32_t>(b.Transition.StateBefore);
					info.tag.haveState = true;
				}
			}
		}

		void STDMETHODCALLTYPE HookBarrier(List* a_list, UINT a_count, const D3D12_RESOURCE_BARRIER* a_barriers)
		{
			if (Active() && a_barriers && !IsOurs(a_list)) {
				NoteBarriers(a_list, a_count, a_barriers);
			}
			g_origBarrier(a_list, a_count, a_barriers);
		}

		// A copy of one screen-sized R16G16B16A16_FLOAT texture into another: the scene's HDR colour being copied for refraction.
		void NoteCopy(List* a_list, ID3D12Resource* a_destination, ID3D12Resource* a_source)
		{
			const UINT w = g_screenW.load(std::memory_order_relaxed), h = g_screenH.load(std::memory_order_relaxed);
			if (!a_destination || !a_source || a_destination == a_source || w == 0) {
				return;
			}
			{
				ID3D12Resource* known = g_knownSource.load(std::memory_order_relaxed);
				if (known && (a_source == known || a_destination == known)) {
					std::lock_guard lock(g_mutex);
					ListInfo& info = g_lists[a_list];
					if (info.touches.size() < 24) {
						info.touches.push_back({ a_source == known ? 'S' : 'D', 0, 0 });
					}
				}
			}
			const D3D12_RESOURCE_DESC d = a_destination->GetDesc();
			const D3D12_RESOURCE_DESC s = a_source->GetDesc();
			if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || s.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.Format != DXGI_FORMAT_R16G16B16A16_FLOAT ||
				s.Format != DXGI_FORMAT_R16G16B16A16_FLOAT || d.Width != w || d.Height != h || s.Width != w || s.Height != h) {
				return;
			}
			std::lock_guard lock(g_mutex);
			ListInfo& info = g_lists[a_list];
			if (info.tag.source) {
				return;  // the first such copy in a list is the one
			}
			info.tag.source = a_source;
			info.tag.destination = a_destination;
			g_knownSource.store(a_source);
			++g_copyMatches;
			for (const First& f : info.firsts) {
				if (f.resource == a_source) {
					info.tag.state = f.state;
					info.tag.haveState = true;
					break;
				}
			}
		}

		void STDMETHODCALLTYPE HookCopyResource(List* a_list, ID3D12Resource* a_destination, ID3D12Resource* a_source)
		{
			if (Active() && !IsOurs(a_list)) {
				NoteCopy(a_list, a_destination, a_source);
			}
			g_origCopyResource(a_list, a_destination, a_source);
		}

		void STDMETHODCALLTYPE HookCopyTexture(List* a_list, const D3D12_TEXTURE_COPY_LOCATION* a_dst, UINT a_x, UINT a_y, UINT a_z, const D3D12_TEXTURE_COPY_LOCATION* a_src, const D3D12_BOX* a_box)
		{
			if (Active() && a_dst && a_src && !a_box && !IsOurs(a_list)) {
				NoteCopy(a_list, a_dst->pResource, a_src->pResource);
			}
			g_origCopyTexture(a_list, a_dst, a_x, a_y, a_z, a_src, a_box);
		}

		// ---- our drawing -----------------------------------------------------------------------------------------------------------
		bool InitGpu()
		{
			if (g_gpuReady) {
				return true;
			}
			if (g_gpuFailed) {
				return false;
			}
			auto* data = RED4ext::GpuApi::GetDeviceData();
			if (!data || !data->device) {
				return false;
			}
			g_device = data->device;
			auto fail = [&](const char* a_what, HRESULT a_hr) {
				g_gpuFailed = true;
				g_sdk->logger->ErrorF(g_handle, "inscene: %s failed (HRESULT 0x%08X); drawing into the scene is off", a_what, static_cast<unsigned>(a_hr));
				return false;
			};
			for (Slot& s : g_slots) {
				HRESULT hr = g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s.allocator));
				if (FAILED(hr)) {
					return fail("making a command allocator", hr);
				}
				hr = g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s.allocator.Get(), nullptr, IID_PPV_ARGS(&s.list));
				if (FAILED(hr)) {
					return fail("making a command list", hr);
				}
				s.list->Close();
			}
			HRESULT hr = g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence));
			if (FAILED(hr)) {
				return fail("making a fence", hr);
			}
			D3D12_DESCRIPTOR_HEAP_DESC rtv{};
			rtv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
			rtv.NumDescriptors = kSlots;
			hr = g_device->CreateDescriptorHeap(&rtv, IID_PPV_ARGS(&g_rtvHeap));
			if (FAILED(hr)) {
				return fail("making the render target heap", hr);
			}
			g_rtvSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

			ComPtr<ID3DBlob> vs, ps, errors;
			hr = ::D3DCompile(kShader, std::strlen(kShader), nullptr, nullptr, nullptr, "VS", "vs_5_0", 0, 0, &vs, &errors);
			if (FAILED(hr)) {
				return fail("compiling the vertex shader", hr);
			}
			hr = ::D3DCompile(kShader, std::strlen(kShader), nullptr, nullptr, nullptr, "PS", "ps_5_0", 0, 0, &ps, &errors);
			if (FAILED(hr)) {
				return fail("compiling the pixel shader", hr);
			}
			D3D12_ROOT_PARAMETER param{};
			param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
			param.Constants.ShaderRegister = 0;
			param.Constants.Num32BitValues = 4;
			param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
			D3D12_ROOT_SIGNATURE_DESC rsDesc{};
			rsDesc.NumParameters = 1;
			rsDesc.pParameters = &param;
			rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
			ComPtr<ID3DBlob> rsBlob;
			hr = ::D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &errors);
			if (FAILED(hr)) {
				return fail("serialising the root signature", hr);
			}
			hr = g_device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&g_rootSignature));
			if (FAILED(hr)) {
				return fail("making the root signature", hr);
			}
			D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
			pso.pRootSignature = g_rootSignature.Get();
			pso.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
			pso.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
			pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
			pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
			pso.RasterizerState.DepthClipEnable = TRUE;
			pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
			pso.SampleMask = UINT_MAX;
			pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			pso.NumRenderTargets = 1;
			pso.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
			pso.SampleDesc.Count = 1;
			hr = g_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&g_pso));
			if (FAILED(hr)) {
				return fail("making the pipeline", hr);
			}
			g_gpuReady = true;
			g_sdk->logger->Info(g_handle, "inscene: ready to draw into the game's scene");
			return true;
		}

		// Records our list for one frame: the scene texture to render target, the square, and back to the state it was in.
		ID3D12CommandList* RecordSquare(const Tag& a_tag, Slot*& a_slotOut, bool& a_drew, std::uint32_t a_depthState, bool a_haveDepthState)
		{
			a_drew = false;
			if (!InitGpu()) {
				g_lastFailure.store("the GPU objects for drawing into the scene could not be made");
				return nullptr;
			}
			Slot& slot = g_slots[g_slotIndex % kSlots];
			if (g_fence->GetCompletedValue() < slot.fenceValue) {
				++g_skipped;
				g_lastFailure.store("the GPU was still using our list (a frame was skipped)");
				return nullptr;  // the GPU is still using this slot: skip this frame rather than wait
			}
			if (FAILED(slot.allocator->Reset()) || FAILED(slot.list->Reset(slot.allocator.Get(), nullptr))) {
				return nullptr;
			}
			const int index = g_slotIndex % kSlots;
			D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
			rtv.ptr += SIZE_T(index) * g_rtvSize;
			D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
			rtvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
			rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
			g_device->CreateRenderTargetView(a_tag.source, &rtvDesc, rtv);

			const D3D12_RESOURCE_STATES before = static_cast<D3D12_RESOURCE_STATES>(a_tag.state);
			D3D12_RESOURCE_BARRIER toTarget{};
			toTarget.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			toTarget.Transition.pResource = a_tag.source;
			toTarget.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			toTarget.Transition.StateBefore = before;
			toTarget.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
			slot.list->ResourceBarrier(1, &toTarget);

			const UINT w = g_screenW.load(), h = g_screenH.load();
			const float brightness[4] = { 0.0f, 1.0f, 4.0f, 16.0f };
			const int mode = std::clamp(g_mode.load(), 1, 3);  // (mode 4 draws the blocks, not the square)
			const float colour[4] = { 1.0f * brightness[mode], 0.30f * brightness[mode], 0.05f * brightness[mode], 0.0f };

			if (g_mode.load() == 4) {
				// A fresh copy of the game's depth first: the end-of-frame capture is a frame old by the time the blocks are drawn (the main depth is written to after this point of the frame).
				if (a_haveDepthState && depth::RecordSceneCapture(slot.list.Get(), a_depthState)) {
					static bool noted = false;
					if (!noted) {
						noted = true;
						g_sdk->logger->InfoF(g_handle, "inscene: copying the game's depth at the moment the scene is complete (it is in state 0x%X then), so the blocks are hidden by this frame's depth, not the last one's", a_depthState);
					}
				}
				// The real thing: Minecraft's blocks, drawn by the overlay's own pipeline into the scene.
				a_drew = overlay::DrawWorldIntoScene(slot.list.Get(), rtv, w, h);
				if (!a_drew) {
					g_lastFailure.store(overlay::SceneBlockedReason());
				}
			} else {
				slot.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
				D3D12_VIEWPORT viewport{ 0.0f, 0.0f, float(w), float(h), 0.0f, 1.0f };
				slot.list->RSSetViewports(1, &viewport);
				const LONG half = static_cast<LONG>(std::min(w, h) / 6);
				D3D12_RECT scissor{ LONG(w / 2) - half, LONG(h / 2) - half, LONG(w / 2) + half, LONG(h / 2) + half };
				slot.list->RSSetScissorRects(1, &scissor);
				slot.list->SetGraphicsRootSignature(g_rootSignature.Get());
				slot.list->SetPipelineState(g_pso.Get());
				slot.list->SetGraphicsRoot32BitConstants(0, 4, colour, 0);
				slot.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
				slot.list->DrawInstanced(3, 1, 0, 0);
				a_drew = true;
			}

			D3D12_RESOURCE_BARRIER back = toTarget;
			back.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
			back.Transition.StateAfter = before;
			slot.list->ResourceBarrier(1, &back);
			if (FAILED(slot.list->Close())) {
				return nullptr;
			}
			a_slotOut = &slot;
			++g_slotIndex;
			return slot.list.Get();
		}

		// The game's lists are handed to the queue in batches: find our tagged list in the batch, and slot ours in just before it.
		void STDMETHODCALLTYPE HookExecute(ID3D12CommandQueue* a_queue, UINT a_count, ID3D12CommandList* const* a_lists)
		{
			if (!Active() || !a_lists || a_count == 0 || g_gaveUp) {
				g_origExecute(a_queue, a_count, a_lists);
				return;
			}
			int found = -1;
			Tag tag;
			std::vector<overlay::FogVolume> atInsertion;  // the fog textures and their states when our list will run
			std::uint32_t depthStateAtInsertion = 0;       // the state of the game's main depth texture when our list will run
			bool haveDepthState = false;
			{
				std::lock_guard lock(g_mutex);
				bool touching = false;
				for (UINT i = 0; i < a_count; ++i) {
					auto it = g_lists.find(static_cast<List*>(a_lists[i]));
					if (it == g_lists.end()) {
						continue;
					}
					touching = touching || !it->second.touches.empty();
					if (found < 0 && it->second.tag.source) {
						if (it->second.tag.haveState) {
							found = static_cast<int>(i);
							tag = it->second.tag;
							g_knownState.store(tag.state);
							g_haveKnownState.store(true);
						} else if (g_haveKnownState.load()) {
							// The list copies the scene but no barrier in it names the texture: its state when the list begins is the same as every other frame.
							found = static_cast<int>(i);
							tag = it->second.tag;
							tag.state = g_knownState.load();
							tag.haveState = true;
						} else {
							++g_noStateTags;
						}
					}
				}
				// Replay what these lists do to the fog textures, in the order the GPU runs them. The states as of just before the scene-copy list are kept: that is where our drawing goes.
				{
					auto replay = [&](UINT i) {
						auto it = g_lists.find(static_cast<List*>(a_lists[i]));
						if (it != g_lists.end()) {
							for (const std::uint32_t s : it->second.depthStates) {
								g_depthState = s;
								g_haveDepthState = true;
							}
							for (const VolTouch& v : it->second.volumes) {
								VolState& st = g_volStates[v.res];
								st.state = v.after;
								st.seen = g_inserted.load();
								if (v.after & D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
									++st.uav;
								}
							}
						}
					};
					const UINT firstPart = found >= 0 ? static_cast<UINT>(found) : a_count;
					if (depth::CandidateResource() != g_depthResource) {
						g_depthResource = depth::CandidateResource();  // a different texture: what was known of the old one is no use
						g_haveDepthState = false;
					}
					for (UINT i = 0; i < firstPart; ++i) {
						replay(i);
					}
					if (found >= 0) {
						depthStateAtInsertion = g_depthState;
						haveDepthState = g_haveDepthState;
						for (auto it = g_volStates.begin(); it != g_volStates.end();) {
							if (g_inserted.load() > it->second.seen + 240) {
								it = g_volStates.erase(it);  // not touched for a while: it may be gone
							} else {
								atInsertion.push_back({ it->first, it->second.state, it->second.uav });
								it->second.uav = 0;  // counted again from here
								++it;
							}
						}
						for (UINT i = static_cast<UINT>(found); i < a_count; ++i) {
							replay(i);
						}
					}
				}
				// Drawing has stopped: describe what the lists of this batch do with the scene texture, once, so that the log shows how the game records it now.
				const std::int64_t nowMs = NowMs();
				if (found < 0 && touching && g_dumpUntilMs.load() > nowMs && nowMs - g_lastDumpMs > 1000) {
					g_lastDumpMs = nowMs;
					g_dumpUntilMs.store(0);
					g_sdk->logger->InfoF(g_handle, "inscene: while drawing is stopped, this batch of %u lists touches the scene texture %p like this:", a_count, static_cast<void*>(g_knownSource.load()));
					for (UINT i = 0; i < a_count; ++i) {
						auto it = g_lists.find(static_cast<List*>(a_lists[i]));
						if (it == g_lists.end() || it->second.touches.empty()) {
							continue;
						}
						std::string line;
						for (const Touch& t : it->second.touches) {
							char piece[48];
							if (t.kind == 'B') {
								std::snprintf(piece, sizeof(piece), "barrier 0x%X->0x%X; ", t.a, t.b);
							} else {
								std::snprintf(piece, sizeof(piece), "%s; ", t.kind == 'S' ? "copy out of it" : "copy into it");
							}
							line += piece;
						}
						g_sdk->logger->InfoF(g_handle, "inscene:   list %u of %u: %s", i, a_count, line.c_str());
					}
				}
			}
			if (found < 0 || a_queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
				g_origExecute(a_queue, a_count, a_lists);
				return;
			}
			if (!g_foundLogged) {
				g_foundLogged = true;
				g_sdk->logger->InfoF(g_handle, "inscene: found the list that copies the scene: it copies %p into %p, and begins with the source in state 0x%X; our drawing goes in just before it",
					static_cast<void*>(tag.source), static_cast<void*>(tag.destination), tag.state);
			}
			// The fog textures, in a stable order, for the overlay to show one of them on the blocks (see /ccdebug fogview).
			std::sort(atInsertion.begin(), atInsertion.end(), [](const overlay::FogVolume& a, const overlay::FogVolume& b) {
				return reinterpret_cast<std::uintptr_t>(a.resource) < reinterpret_cast<std::uintptr_t>(b.resource);
			});
			{
				static std::vector<ID3D12Resource*> lastSet;
				std::vector<ID3D12Resource*> now;
				for (const auto& v : atInsertion) {
					now.push_back(v.resource);
				}
				if (now != lastSet) {
					lastSet = now;
					g_sdk->logger->InfoF(g_handle, "inscene: %u froxel volumes (the game's volumetric fog textures) when the scene is complete:", static_cast<unsigned>(atInsertion.size()));
					for (std::size_t i = 0; i < atInsertion.size(); ++i) {
						g_sdk->logger->InfoF(g_handle, "inscene:   volume %u: %p, state %s, written by compute %u times since the last frame", static_cast<unsigned>(i), static_cast<void*>(atInsertion[i].resource),
							StateName(atInsertion[i].state).c_str(), atInsertion[i].uavWrites);
					}
				}
			}
			overlay::SetFogVolumes(atInsertion);
			Slot* slot = nullptr;
			bool drew = false;
			ID3D12CommandList* ours = RecordSquare(tag, slot, drew, depthStateAtInsertion, haveDepthState);
			if (!ours) {
				g_origExecute(a_queue, a_count, a_lists);
				return;
			}
			if (found > 0) {
				g_origExecute(a_queue, static_cast<UINT>(found), a_lists);
			}
			g_origExecute(a_queue, 1, &ours);
			a_queue->Signal(g_fence.Get(), ++g_fenceValue);
			slot->fenceValue = g_fenceValue;
			g_origExecute(a_queue, a_count - static_cast<UINT>(found), a_lists + found);
			if (drew) {
				g_lastDrawMs.store(NowMs());
				g_matchesAtDraw = g_copyMatches.load();
				g_noStateAtDraw = g_noStateTags.load();
			}
			const std::uint64_t n = ++g_inserted;
			if (n <= 3 || n % 600 == 0) {
				g_sdk->logger->InfoF(g_handle, "inscene: inserted our drawing %llu times (skipped %llu frames because the GPU was still using our list)", static_cast<unsigned long long>(n),
					static_cast<unsigned long long>(g_skipped.load()));
			}
		}

		// ---- installing ------------------------------------------------------------------------------------------------------------
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
			ComPtr<ID3D12CommandAllocator> allocator;
			ComPtr<ID3D12GraphicsCommandList> list;
			ComPtr<ID3D12CommandQueue> queue;
			D3D12_COMMAND_QUEUE_DESC queueDesc{};
			queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
			if (FAILED(data->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
				FAILED(data->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list))) ||
				FAILED(data->device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)))) {
				return false;
			}
			void** listTable = *reinterpret_cast<void***>(list.Get());
			void** queueTable = *reinterpret_cast<void***>(queue.Get());
			Patch(listTable, kReset, reinterpret_cast<void*>(&HookReset), reinterpret_cast<void**>(&g_origReset));
			Patch(listTable, kCopyTextureRegion, reinterpret_cast<void*>(&HookCopyTexture), reinterpret_cast<void**>(&g_origCopyTexture));
			Patch(listTable, kCopyResource, reinterpret_cast<void*>(&HookCopyResource), reinterpret_cast<void**>(&g_origCopyResource));
			Patch(listTable, kResourceBarrier, reinterpret_cast<void*>(&HookBarrier), reinterpret_cast<void**>(&g_origBarrier));
			Patch(queueTable, kExecuteCommandLists, reinterpret_cast<void*>(&HookExecute), reinterpret_cast<void**>(&g_origExecute));
			list->Close();
			const bool all = g_origReset && g_origCopyTexture && g_origCopyResource && g_origBarrier && g_origExecute;
			g_sdk->logger->InfoF(g_handle, "inscene: hooked the game's command lists (%p) and queues (%p)%s", static_cast<void*>(listTable), static_cast<void*>(queueTable), all ? "" : "; SOME HOOKS FAILED");
			return all;
		}
	}

	void Init(RED4ext::v1::PluginHandle a_handle, const RED4ext::v1::Sdk* a_sdk)
	{
		g_handle = a_handle;
		g_sdk = a_sdk;
	}

	void SetScreenSize(UINT a_width, UINT a_height)
	{
		g_screenW.store(a_width);
		g_screenH.store(a_height);
	}

	namespace
	{
		bool g_userChoice = false;  // the player has used /ccdebug scene: then the mode is theirs, and nothing starts it by itself
	}

	void SetMode(int a_mode)
	{
		g_userChoice = true;
		a_mode = std::clamp(a_mode, 0, 4);
		if (a_mode > 0 && g_gaveUp) {
			g_sdk->logger->Warn(g_handle, "inscene: the hooks failed earlier; restart the game to try again");
			return;
		}
		{
			std::lock_guard lock(g_mutex);
			g_lists.clear();
			g_volStates.clear();
			g_haveDepthState = false;
		}
		g_mode.store(a_mode);
		g_lastDrawMs.store(0);
		g_wasDrawing = false;
		g_armedAt = std::chrono::steady_clock::now();
		g_foundLogged = false;
		g_sdk->logger->InfoF(g_handle, "inscene: drawing into the game's scene: %s", a_mode == 0 ? "off" : a_mode == 1 ? "a dim square" : a_mode == 2 ? "a bright square" : a_mode == 3 ? "a very bright square" : "Minecraft's blocks");
	}

	void EnableByDefault()
	{
		if (g_userChoice || g_mode.load() != 0 || g_gaveUp) {
			return;
		}
		SetMode(4);
		g_userChoice = false;  // that was not the player's choice: /ccdebug scene 0 still turns it off for good
		g_sdk->logger->Info(g_handle, "inscene: started by itself because Minecraft is sending frames (/ccdebug scene 0 turns it off)");
	}

	void OnPresent()
	{
		if (g_mode.load() == 0) {
			return;
		}
		if (!g_hooked) {
			if (!InstallHooks()) {
				if (std::chrono::steady_clock::now() - g_armedAt > std::chrono::seconds(5)) {
					g_gaveUp = true;
					g_mode.store(0);
					g_sdk->logger->Error(g_handle, "inscene: could not hook the game's command lists");
				}
				return;
			}
			g_hooked = true;
		}
		// Say when drawing into the scene stops or starts again, and why: the effect disappearing is otherwise silent.
		{
			const bool drawing = NowMs() - g_lastDrawMs.load() < 1500 && g_lastDrawMs.load() != 0;
			if (drawing && !g_wasDrawing) {
				g_sdk->logger->Info(g_handle, g_everDrew ? "inscene: drawing into the game's scene again" : "inscene: drawing into the game's scene");
				g_everDrew = true;
			} else if (!drawing && g_wasDrawing) {
				const char* why = g_lastFailure.load();
				const std::uint64_t matches = g_copyMatches.load() - g_matchesAtDraw;
				const std::uint64_t noState = g_noStateTags.load() - g_noStateAtDraw;
				g_sdk->logger->InfoF(g_handle, "inscene: STOPPED drawing into the game's scene: %s (since the last drawing: %llu copies of the scene seen while the game recorded its lists, %llu scene-copy lists handed to the queue without a known state)",
					(why && *why) ? why : "the list that copies the scene has not been handed to the queue (the game has stopped copying the scene, records it differently, or the hooks lost it)",
					static_cast<unsigned long long>(matches), static_cast<unsigned long long>(noState));
				g_dumpUntilMs.store(NowMs() + 20000);
			}
			g_wasDrawing = drawing;
		}
		if (!g_foundLogged && std::chrono::steady_clock::now() - g_armedAt > std::chrono::seconds(6)) {
			static auto lastComplaint = std::chrono::steady_clock::time_point{};
			const auto now = std::chrono::steady_clock::now();
			if (now - lastComplaint > std::chrono::seconds(10)) {
				lastComplaint = now;
				g_sdk->logger->Info(g_handle, "inscene: the list that copies the scene has not been seen yet (needs the overlay's screen size, and frames to be recorded after this was switched on)");
			}
		}
	}
}
