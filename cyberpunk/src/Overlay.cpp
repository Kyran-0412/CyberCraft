// Phase 3b: draws Minecraft's HUD over Cyberpunk.
//
// Minecraft renders only its hotbar, hearts, held item and screens on a transparent background and copies the
// pixels into shared memory (see OverlayCtl in the protocol). Here, just before the game presents each frame,
// the newest of those pixels are uploaded to a texture and drawn over the back buffer with the game's own
// D3D12 device and command queue.
//
// The game's swapchain is found through the RED4ext SDK (GpuApi::GetDeviceData), and its Present functions are
// replaced by patching the swapchain's vtable. Everything below Install() runs on the game's render thread,
// inside Present.
//
// A small cyan square is drawn in the top-left corner of the screen whenever there is no HUD to show: for the
// first 30 seconds after a save loads, and whenever Minecraft is linked but no HUD frames are arriving. If you
// can see it, the drawing itself works. Every 5 seconds a status line goes to the log.

#include "Overlay.hpp"
#include "Depth.hpp"
#include "Input.hpp"
#include "Link.hpp"

#include <cybercraft_protocol.h>

#include <RED4ext/GpuApi/DeviceData.hpp>

#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace cybercraft::overlay
{
	namespace
	{
		constexpr UINT kFrames = 3;                 // frames in flight
		constexpr UINT kPresentIndex = 8;           // IDXGISwapChain::Present
		constexpr UINT kPresent1Index = 22;         // IDXGISwapChain1::Present1
		constexpr double kBadgeSeconds = 30.0;      // how long after a save loads the test square is shown while Minecraft isn't linked
		constexpr double kStaleSeconds = 1.0;       // a HUD frame older than this is not drawn

		using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
		using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);

		RED4ext::v1::PluginHandle g_handle = nullptr;
		const RED4ext::v1::Sdk* g_sdk = nullptr;

		bool g_installed = false;
		bool g_triedInstall = false;
		void** g_vtable = nullptr;
		PresentFn g_origPresent = nullptr;
		Present1Fn g_origPresent1 = nullptr;
		IDXGISwapChain* g_gameSwapChain = nullptr;  // not owned
		HWND g_window = nullptr;
		std::chrono::steady_clock::time_point g_installTime{};

		// Counters for the status line.
		std::chrono::steady_clock::time_point g_lastStatus{};
		std::uint64_t g_presents = 0;
		std::uint64_t g_drawCalls = 0;
		std::uint64_t g_hudDraws = 0;
		std::uint64_t g_badgeDraws = 0;
		int g_debugView = 0;
		std::uint64_t g_framesUploaded = 0;
		std::uint64_t g_layeredDraws = 0;
		std::uint64_t g_plainDraws = 0;
		std::uint64_t g_depthDraws = 0;
		double g_ageSum = 0.0;
		double g_ageMax = 0.0;
		std::uint64_t g_ageCount = 0;
		bool g_wasInGame = false;
		std::chrono::steady_clock::time_point g_inGameSince{};

		const char* kShaderSource = R"HLSL(
cbuffer P : register(b0)
{
	float flipY; float mode; float cursorOn; float layered;
	float2 cursor; float zeroToOne; float haveGameDepth;
	float mcA; float mcB; float gameNear; float biasAbs;
	float biasRel; float debugView; float2 pad;
};
Texture2D tex : register(t0);          // the whole frame (plain mode), or the world's colour (layered)
Texture2D mcDepthTex : register(t1);   // layered: the world's depth, as Minecraft wrote it
Texture2D overlayTex : register(t2);   // layered: the hand, hotbar and screens
Texture2D gameDepthTex : register(t3); // the game's own depth (a copy), when there is one
SamplerState samp : register(s0);
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut VS(uint id : SV_VertexID)
{
	VSOut o;
	float2 uv = float2((id << 1) & 2, id & 2);
	o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
	o.uv = uv;
	return o;
}
float4 PS(VSOut i) : SV_Target
{
	if (mode > 0.5) {
		return float4(0.0, 0.5, 0.6, 0.6);   // the test square: cyan at 60%, premultiplied
	}
	float2 uv = i.uv;
	if (flipY > 0.5) { uv.y = 1.0 - uv.y; }
	float4 c;
	if (layered > 0.5) {
		float4 w = tex.Sample(samp, uv);       // the world, premultiplied alpha
		float zg = 1e9;
		if (haveGameDepth > 0.5) {
			// The game's world at this place on the screen: its depth is gameNear / distance (reversed, no far plane).
			uint gw, gh;
			gameDepthTex.GetDimensions(gw, gh);
			float sg = gameDepthTex.Load(int3(min(uint2(i.uv * float2(gw, gh)), uint2(gw - 1, gh - 1)), 0)).r;
			zg = sg > 1e-8 ? gameNear / sg : 1e9;
		}
		float zm = 1e9;
		bool hidden = false;
		if (w.a > 0.0) {
			// How far is this block pixel from the camera? Minecraft's depth is the matrix's: ndc = -A + B / distance.
			uint mw, mh;
			mcDepthTex.GetDimensions(mw, mh);
			float s = mcDepthTex.Load(int3(min(uint2(uv * float2(mw, mh)), uint2(mw - 1, mh - 1)), 0)).r;
			float ndc = zeroToOne > 0.5 ? s : s * 2.0 - 1.0;
			float den = ndc + mcA;
			zm = abs(den) > 1e-9 ? mcB / den : 1e9;
			if (zm <= 0.0) { zm = 1e9; }
			// Hidden if it is behind what the game drew there (with a little room for rounding, and for the game's jittered depth).
			hidden = haveGameDepth > 0.5 && zm > zg + biasAbs + biasRel * zg;
		}
		if (debugView > 2.5) {
			if (w.a <= 0.0) { return float4(0, 0, 0, 0); }
			return hidden ? float4(0.6, 0.0, 0.0, 0.6) : float4(0.0, 0.5, 0.0, 0.6);
		}
		if (debugView > 1.5) {
			if (w.a <= 0.0) { return float4(0, 0, 0, 0); }
			float g = saturate(zm / 60.0);
			return float4(g, g, g, 1);
		}
		if (debugView > 0.5) {
			float g = saturate(zg / 60.0);
			return float4(g, g, g, 1);
		}
		if (hidden) { w = float4(0, 0, 0, 0); }
		float4 o = overlayTex.Sample(samp, uv);
		c = o + w * (1.0 - o.a);                // the overlay over the world, both premultiplied
	} else {
		c = tex.Sample(samp, uv);               // premultiplied alpha straight from Minecraft
	}
	if (cursorOn > 0.5) {
		// The mouse cursor used while a Minecraft screen is open: a small white arrow with a black edge.
		float2 p = i.pos.xy - cursor;
		if (p.x >= 0 && p.y >= 0 && p.y < 18 && p.x <= p.y * 0.6) {
			bool edge = p.x < 1.5 || p.x > p.y * 0.6 - 1.5 || p.y > 16.5;
			c = float4(edge ? float3(0, 0, 0) : float3(1, 1, 1), 1);
		}
	}
	return c;
}
)HLSL";

		struct Gpu
		{
			bool failed = false;
			bool ready = false;

			ComPtr<ID3D12Device> device;
			ComPtr<ID3D12CommandQueue> queue;
			ComPtr<ID3D12RootSignature> rootSignature;
			ComPtr<ID3D12PipelineState> pso;
			DXGI_FORMAT psoFormat = DXGI_FORMAT_UNKNOWN;
			ComPtr<ID3DBlob> vs, ps;
			ComPtr<ID3D12DescriptorHeap> rtvHeap;
			ComPtr<ID3D12DescriptorHeap> srvHeap;
			UINT rtvSize = 0;
			ComPtr<ID3D12CommandAllocator> allocator[kFrames];
			ComPtr<ID3D12GraphicsCommandList> list;
			ComPtr<ID3D12Fence> fence;
			HANDLE fenceEvent = nullptr;
			UINT64 fenceValue = 0;
			UINT64 frameFence[kFrames] = {};
			UINT frame = 0;

			// The HUD texture (or, for a layered frame, the world's colour) and the upload buffers that feed it.
			ComPtr<ID3D12Resource> texture;
			UINT texW = 0, texH = 0;
			// Layered frames: the world's depth and the overlay, same size as the colour.
			ComPtr<ID3D12Resource> texDepth;
			ComPtr<ID3D12Resource> texOverlay;
			UINT layerW = 0, layerH = 0;
			bool layered = false;      // the newest frame has layers
			float mcA = 0.0f, mcB = 0.0f;
			bool zeroToOne = true;
			UINT srvInc = 0;
			// The game's depth copy, as last bound to descriptor 3.
			ID3D12Resource* boundGameDepth = nullptr;
			UINT boundGameW = 0, boundGameH = 0;
			DXGI_FORMAT boundGameFormat = DXGI_FORMAT_UNKNOWN;
			ComPtr<ID3D12Resource> upload[kFrames];
			UINT64 uploadSize[kFrames] = {};
			bool haveFrame = false;
			bool flipY = false;
			std::uint64_t lastFrameCount = 0;
			std::chrono::steady_clock::time_point lastNewFrame{};

			bool loggedFormat = false;
			bool loggedFirstFrame = false;
			UINT warnedW = 0, warnedH = 0;
		} g;

		void LogError(const char* a_what, HRESULT a_hr)
		{
			g_sdk->logger->ErrorF(g_handle, "overlay: %s failed (HRESULT 0x%08X); the HUD overlay is off", a_what, static_cast<unsigned>(a_hr));
		}

		void WaitIdle()
		{
			if (!g.fence || !g.fenceEvent) {
				return;
			}
			if (g.fence->GetCompletedValue() < g.fenceValue) {
				g.fence->SetEventOnCompletion(g.fenceValue, g.fenceEvent);
				::WaitForSingleObject(g.fenceEvent, 2000);
			}
		}

		DXGI_FORMAT Typed(DXGI_FORMAT a_format)
		{
			switch (a_format) {
			case DXGI_FORMAT_R8G8B8A8_TYPELESS:
				return DXGI_FORMAT_R8G8B8A8_UNORM;
			case DXGI_FORMAT_B8G8R8A8_TYPELESS:
				return DXGI_FORMAT_B8G8R8A8_UNORM;
			case DXGI_FORMAT_R10G10B10A2_TYPELESS:
				return DXGI_FORMAT_R10G10B10A2_UNORM;
			case DXGI_FORMAT_R16G16B16A16_TYPELESS:
				return DXGI_FORMAT_R16G16B16A16_FLOAT;
			default:
				return a_format;
			}
		}

		bool IsSrgb(DXGI_FORMAT a_format)
		{
			return a_format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || a_format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
		}

		// Everything that doesn't depend on the back buffer's size: device, queue, command lists, fence, heaps.
		bool InitDevice(IDXGISwapChain* a_swapChain)
		{
			// The swapchain won't hand over its command queue (GetDevice answers "no such interface" for it, which
			// can mean the game's swapchain is wrapped by something like NVIDIA Streamline), so use the game's own
			// device and main command queue, which the RED4ext SDK knows the location of.
			auto* data = RED4ext::GpuApi::GetDeviceData();
			if (!data || !data->device || !data->directCommandQueue) {
				g_sdk->logger->Error(g_handle, "overlay: the SDK has no device or command queue for the game; the HUD overlay is off");
				return false;
			}
			g.device = data->device;
			g.queue = data->directCommandQueue;
			HRESULT hr = S_OK;

			// Sanity checks, logged so that a wrong guess shows up in the log instead of as a crash later.
			ComPtr<ID3D12Device> queueDevice;
			if (SUCCEEDED(g.queue->GetDevice(IID_PPV_ARGS(&queueDevice)))) {
				if (queueDevice.Get() != g.device.Get()) {
					g_sdk->logger->Warn(g_handle, "overlay: the queue belongs to a different device than the SDK's device; using the queue's");
					g.device = queueDevice;
				}
			}
			const D3D12_COMMAND_QUEUE_DESC queueDesc = g.queue->GetDesc();
			g_sdk->logger->InfoF(g_handle, "overlay: using the game's command queue %p (type %d) on device %p", static_cast<void*>(g.queue.Get()),
				static_cast<int>(queueDesc.Type), static_cast<void*>(g.device.Get()));
			if (queueDesc.Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
				g_sdk->logger->Error(g_handle, "overlay: that is not a direct command queue, so it can't draw; the HUD overlay is off");
				return false;
			}
			{
				ComPtr<ID3D12Device> viaSwapChain;
				const HRESULT swapHr = a_swapChain->GetDevice(IID_PPV_ARGS(&viaSwapChain));
				g_sdk->logger->InfoF(g_handle, "overlay: swapchain->GetDevice(ID3D12Device) = 0x%08X%s", static_cast<unsigned>(swapHr),
					SUCCEEDED(swapHr) ? (viaSwapChain.Get() == g.device.Get() ? " (same device)" : " (a DIFFERENT device)") : "");
			}

			for (UINT i = 0; i < kFrames; ++i) {
				hr = g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g.allocator[i]));
				if (FAILED(hr)) {
					LogError("creating a command allocator", hr);
					return false;
				}
			}
			hr = g.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.allocator[0].Get(), nullptr, IID_PPV_ARGS(&g.list));
			if (FAILED(hr)) {
				LogError("creating the command list", hr);
				return false;
			}
			g.list->Close();

			hr = g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence));
			if (FAILED(hr)) {
				LogError("creating the fence", hr);
				return false;
			}
			g.fenceEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);

			D3D12_DESCRIPTOR_HEAP_DESC rtv{};
			rtv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
			rtv.NumDescriptors = kFrames;
			hr = g.device->CreateDescriptorHeap(&rtv, IID_PPV_ARGS(&g.rtvHeap));
			if (FAILED(hr)) {
				LogError("creating the render target heap", hr);
				return false;
			}
			g.rtvSize = g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

			D3D12_DESCRIPTOR_HEAP_DESC srv{};
			srv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
			srv.NumDescriptors = 4;
			srv.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
			hr = g.device->CreateDescriptorHeap(&srv, IID_PPV_ARGS(&g.srvHeap));
			if (FAILED(hr)) {
				LogError("creating the texture heap", hr);
				return false;
			}
			g.srvInc = g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			{
				// All four descriptors must be valid for the shader, even the ones a frame doesn't use: make them null ones.
				D3D12_SHADER_RESOURCE_VIEW_DESC nullView{};
				nullView.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
				nullView.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
				nullView.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
				nullView.Texture2D.MipLevels = 1;
				for (UINT i = 0; i < 4; ++i) {
					D3D12_CPU_DESCRIPTOR_HANDLE h = g.srvHeap->GetCPUDescriptorHandleForHeapStart();
					h.ptr += SIZE_T(i) * g.srvInc;
					g.device->CreateShaderResourceView(nullptr, &nullView, h);
				}
			}

			// Shaders and the root signature.
			ComPtr<ID3DBlob> errors;
			hr = ::D3DCompile(kShaderSource, std::strlen(kShaderSource), nullptr, nullptr, nullptr, "VS", "vs_5_0", 0, 0, &g.vs, &errors);
			if (FAILED(hr)) {
				LogError("compiling the vertex shader", hr);
				if (errors) {
					g_sdk->logger->Error(g_handle, static_cast<const char*>(errors->GetBufferPointer()));
				}
				return false;
			}
			errors.Reset();
			hr = ::D3DCompile(kShaderSource, std::strlen(kShaderSource), nullptr, nullptr, nullptr, "PS", "ps_5_0", 0, 0, &g.ps, &errors);
			if (FAILED(hr)) {
				LogError("compiling the pixel shader", hr);
				if (errors) {
					g_sdk->logger->Error(g_handle, static_cast<const char*>(errors->GetBufferPointer()));
				}
				return false;
			}

			D3D12_DESCRIPTOR_RANGE range{};
			range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
			range.NumDescriptors = 4;
			range.BaseShaderRegister = 0;
			range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

			D3D12_ROOT_PARAMETER params[2]{};
			params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			params[0].DescriptorTable.NumDescriptorRanges = 1;
			params[0].DescriptorTable.pDescriptorRanges = &range;
			params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
			params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
			params[1].Constants.ShaderRegister = 0;
			params[1].Constants.Num32BitValues = 16;
			params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

			D3D12_STATIC_SAMPLER_DESC sampler{};
			sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
			sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
			sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
			sampler.MaxLOD = D3D12_FLOAT32_MAX;
			sampler.ShaderRegister = 0;
			sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

			D3D12_ROOT_SIGNATURE_DESC rs{};
			rs.NumParameters = 2;
			rs.pParameters = params;
			rs.NumStaticSamplers = 1;
			rs.pStaticSamplers = &sampler;
			rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS | D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
			           D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS | D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

			ComPtr<ID3DBlob> blob;
			errors.Reset();
			hr = ::D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &errors);
			if (FAILED(hr)) {
				LogError("serializing the root signature", hr);
				return false;
			}
			hr = g.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g.rootSignature));
			if (FAILED(hr)) {
				LogError("creating the root signature", hr);
				return false;
			}
			return true;
		}

		// The pipeline depends on the back buffer's format; make it again if that changes.
		bool EnsurePipeline(DXGI_FORMAT a_format)
		{
			if (g.pso && g.psoFormat == a_format) {
				return true;
			}
			WaitIdle();
			g.pso.Reset();

			D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
			desc.pRootSignature = g.rootSignature.Get();
			desc.VS = { g.vs->GetBufferPointer(), g.vs->GetBufferSize() };
			desc.PS = { g.ps->GetBufferPointer(), g.ps->GetBufferSize() };
			auto& blend = desc.BlendState.RenderTarget[0];
			blend.BlendEnable = TRUE;
			blend.SrcBlend = D3D12_BLEND_ONE;  // premultiplied alpha
			blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
			blend.BlendOp = D3D12_BLEND_OP_ADD;
			blend.SrcBlendAlpha = D3D12_BLEND_ONE;
			blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
			blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
			blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
			desc.SampleMask = UINT_MAX;
			desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
			desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
			desc.RasterizerState.DepthClipEnable = TRUE;
			desc.DepthStencilState.DepthEnable = FALSE;
			desc.DepthStencilState.StencilEnable = FALSE;
			desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			desc.NumRenderTargets = 1;
			desc.RTVFormats[0] = a_format;
			desc.SampleDesc.Count = 1;

			const HRESULT hr = g.device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&g.pso));
			if (FAILED(hr)) {
				LogError("creating the pipeline for the back buffer's format", hr);
				return false;
			}
			g.psoFormat = a_format;
			return true;
		}

		D3D12_HEAP_PROPERTIES HeapProps(D3D12_HEAP_TYPE a_type)
		{
			D3D12_HEAP_PROPERTIES p{};
			p.Type = a_type;
			p.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
			p.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
			p.CreationNodeMask = 1;
			p.VisibleNodeMask = 1;
			return p;
		}

		D3D12_RESOURCE_DESC BufferDesc(UINT64 a_size)
		{
			D3D12_RESOURCE_DESC d{};
			d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			d.Width = a_size;
			d.Height = 1;
			d.DepthOrArraySize = 1;
			d.MipLevels = 1;
			d.Format = DXGI_FORMAT_UNKNOWN;
			d.SampleDesc.Count = 1;
			d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			return d;
		}

		// (Re)creates the HUD texture to fit a frame of a_w x a_h pixels.
		bool EnsureTexture(UINT a_w, UINT a_h, DXGI_FORMAT a_format)
		{
			if (g.texture && g.texW == a_w && g.texH == a_h) {
				return true;
			}
			WaitIdle();
			g.texture.Reset();

			D3D12_RESOURCE_DESC desc{};
			desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			desc.Width = a_w;
			desc.Height = a_h;
			desc.DepthOrArraySize = 1;
			desc.MipLevels = 1;
			desc.Format = a_format;
			desc.SampleDesc.Count = 1;
			desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

			const auto heap = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
			HRESULT hr = g.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
				IID_PPV_ARGS(&g.texture));
			if (FAILED(hr)) {
				LogError("creating the HUD texture", hr);
				return false;
			}
			g.texW = a_w;
			g.texH = a_h;

			D3D12_SHADER_RESOURCE_VIEW_DESC view{};
			view.Format = a_format;
			view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			view.Texture2D.MipLevels = 1;
			g.device->CreateShaderResourceView(g.texture.Get(), &view, g.srvHeap->GetCPUDescriptorHandleForHeapStart());
			return true;
		}

		// (Re)creates the textures for a layered frame's depth and overlay to fit a_w x a_h pixels. (The colour is g.texture.)
		bool EnsureLayerTextures(UINT a_w, UINT a_h, DXGI_FORMAT a_colourFormat)
		{
			if (g.texDepth && g.texOverlay && g.layerW == a_w && g.layerH == a_h) {
				return true;
			}
			WaitIdle();
			g.texDepth.Reset();
			g.texOverlay.Reset();

			D3D12_RESOURCE_DESC desc{};
			desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			desc.Width = a_w;
			desc.Height = a_h;
			desc.DepthOrArraySize = 1;
			desc.MipLevels = 1;
			desc.SampleDesc.Count = 1;
			desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
			const auto heap = HeapProps(D3D12_HEAP_TYPE_DEFAULT);

			desc.Format = DXGI_FORMAT_R32_FLOAT;
			HRESULT hr = g.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&g.texDepth));
			if (FAILED(hr)) {
				LogError("creating the world depth texture", hr);
				return false;
			}
			desc.Format = a_colourFormat;
			hr = g.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&g.texOverlay));
			if (FAILED(hr)) {
				LogError("creating the overlay texture", hr);
				return false;
			}
			g.layerW = a_w;
			g.layerH = a_h;

			D3D12_SHADER_RESOURCE_VIEW_DESC view{};
			view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			view.Texture2D.MipLevels = 1;
			D3D12_CPU_DESCRIPTOR_HANDLE h = g.srvHeap->GetCPUDescriptorHandleForHeapStart();
			view.Format = DXGI_FORMAT_R32_FLOAT;
			h.ptr += SIZE_T(1) * g.srvInc;
			g.device->CreateShaderResourceView(g.texDepth.Get(), &view, h);
			view.Format = a_colourFormat;
			h.ptr += g.srvInc;
			g.device->CreateShaderResourceView(g.texOverlay.Get(), &view, h);
			return true;
		}

		bool EnsureUpload(UINT a_slot, UINT64 a_size)
		{
			if (g.upload[a_slot] && g.uploadSize[a_slot] >= a_size) {
				return true;
			}
			g.upload[a_slot].Reset();
			const auto heap = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
			const auto desc = BufferDesc(a_size);
			const HRESULT hr = g.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
				IID_PPV_ARGS(&g.upload[a_slot]));
			if (FAILED(hr)) {
				LogError("creating an upload buffer", hr);
				return false;
			}
			g.uploadSize[a_slot] = a_size;
			return true;
		}

		D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* a_resource, D3D12_RESOURCE_STATES a_before, D3D12_RESOURCE_STATES a_after)
		{
			D3D12_RESOURCE_BARRIER b{};
			b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			b.Transition.pResource = a_resource;
			b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			b.Transition.StateBefore = a_before;
			b.Transition.StateAfter = a_after;
			return b;
		}

		void Draw(IDXGISwapChain* a_swapChain)
		{
			auto& link = Link::Get();
			if (!link.IsOpen()) {
				return;
			}

			const auto now = std::chrono::steady_clock::now();

			// Is there a HUD to draw, or only the test square?
			const bool mcAlive = link.McPid() != 0 && (::GetTickCount64() - link.McHeartbeatMs()) < 3000;
			const std::uint64_t published = mcAlive ? link.OverlayFramesPublished() : 0;
			if (published != g.lastFrameCount) {
				g.lastFrameCount = published;
				g.lastNewFrame = now;
			}
			const bool hudFresh = mcAlive && g.haveFrame && std::chrono::duration<double>(now - g.lastNewFrame).count() < kStaleSeconds;
			const bool inGame = link.InGame();
			if (inGame && !g_wasInGame) {
				g_inGameSince = now;
			}
			g_wasInGame = inGame;
			// The test square: Minecraft is linked but no HUD is arriving, or a save loaded less than 30 s ago.
			const bool badge = !hudFresh && inGame && (mcAlive || std::chrono::duration<double>(now - g_inGameSince).count() < kBadgeSeconds);
			const bool newFrame = mcAlive && link.AcquireOverlayFrame();
			if (!hudFresh && !newFrame && !badge) {
				return;  // nothing to draw
			}
			++g_drawCalls;

			if (g.failed) {
				return;
			}

			// The back buffer.
			ComPtr<IDXGISwapChain3> sc3;
			if (FAILED(a_swapChain->QueryInterface(IID_PPV_ARGS(&sc3)))) {
				return;
			}
			ComPtr<ID3D12Resource> backBuffer;
			if (FAILED(sc3->GetBuffer(sc3->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&backBuffer)))) {
				return;
			}
			const D3D12_RESOURCE_DESC bb = backBuffer->GetDesc();
			const DXGI_FORMAT rtvFormat = Typed(bb.Format);
			if (!g.loggedFormat) {
				g.loggedFormat = true;
				g_sdk->logger->InfoF(g_handle, "overlay: back buffer is %u x %u, format %d", static_cast<unsigned>(bb.Width), bb.Height, static_cast<int>(bb.Format));
				if (rtvFormat == DXGI_FORMAT_R16G16B16A16_FLOAT || rtvFormat == DXGI_FORMAT_R10G10B10A2_UNORM) {
					g_sdk->logger->Info(g_handle, "overlay: the game looks to be in HDR; the HUD's colours may look off there");
				}
			}

			if (!g.ready) {
				if (!InitDevice(a_swapChain)) {
					g.failed = true;
					return;
				}
				g.ready = true;
				g_sdk->logger->Info(g_handle, "overlay: D3D12 resources ready");
			}
			if (!EnsurePipeline(rtvFormat)) {
				g.failed = true;
				return;
			}

			// Wait until the GPU is done with this frame's allocator and upload buffer.
			const UINT slot = g.frame % kFrames;
			if (g.fence->GetCompletedValue() < g.frameFence[slot]) {
				g.fence->SetEventOnCompletion(g.frameFence[slot], g.fenceEvent);
				::WaitForSingleObject(g.fenceEvent, 1000);
			}
			if (FAILED(g.allocator[slot]->Reset()) || FAILED(g.list->Reset(g.allocator[slot].Get(), nullptr))) {
				return;
			}

			// A new frame from Minecraft: copy it up to the GPU. A plain frame is one picture; a layered one has three (the world's
			// colour, the world's depth, the overlay), all of the same size.
			bool uploaded = false;
			if (newFrame) {
				const auto* hdr = link.OverlayFrontHeader();
				const UINT w = hdr->width;
				const UINT h = hdr->height;
				const bool layered = (hdr->flags & proto::kOverlayLayered) != 0;
				if (w > 0 && h > 0 && w <= proto::kMaxOverlayW && h <= proto::kMaxOverlayH) {
					const DXGI_FORMAT texFormat = IsSrgb(bb.Format) ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
					const UINT rowBytes = w * 4;
					const UINT rowPitch = (rowBytes + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
					// Each layer's rows go in a region of the upload buffer that starts on the copy alignment.
					const UINT64 region = (UINT64(rowPitch) * h + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) & ~UINT64(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
					const UINT layers = layered ? 3 : 1;
					if (EnsureTexture(w, h, texFormat) && (!layered || EnsureLayerTextures(w, h, texFormat)) && EnsureUpload(slot, region * layers)) {
						void* mapped = nullptr;
						const D3D12_RANGE noRead{ 0, 0 };
						if (SUCCEEDED(g.upload[slot]->Map(0, &noRead, &mapped))) {
							ID3D12Resource* targets[3] = { g.texture.Get(), g.texDepth.Get(), g.texOverlay.Get() };
							const DXGI_FORMAT formats[3] = { texFormat, DXGI_FORMAT_R32_FLOAT, texFormat };
							const std::uint8_t* pixels = link.OverlayFrontPixels();
							for (UINT layer = 0; layer < layers; ++layer) {
								const std::uint8_t* src = pixels + UINT64(layer) * proto::kOverlayLayerBytes;
								auto* dst = static_cast<std::uint8_t*>(mapped) + UINT64(layer) * region;
								for (UINT y = 0; y < h; ++y) {
									std::memcpy(dst + UINT64(y) * rowPitch, src + UINT64(y) * rowBytes, rowBytes);
								}
							}
							g.upload[slot]->Unmap(0, nullptr);

							for (UINT layer = 0; layer < layers; ++layer) {
								D3D12_TEXTURE_COPY_LOCATION to{};
								to.pResource = targets[layer];
								to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
								to.SubresourceIndex = 0;
								D3D12_TEXTURE_COPY_LOCATION from{};
								from.pResource = g.upload[slot].Get();
								from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
								from.PlacedFootprint.Offset = UINT64(layer) * region;
								from.PlacedFootprint.Footprint.Format = formats[layer];
								from.PlacedFootprint.Footprint.Width = w;
								from.PlacedFootprint.Footprint.Height = h;
								from.PlacedFootprint.Footprint.Depth = 1;
								from.PlacedFootprint.Footprint.RowPitch = rowPitch;

								auto toCopy = Transition(targets[layer], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
								g.list->ResourceBarrier(1, &toCopy);
								g.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
								auto toRead = Transition(targets[layer], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
								g.list->ResourceBarrier(1, &toRead);
							}

							g.flipY = (hdr->flags & proto::kOverlayBottomUp) != 0;
							g.layered = layered;
							g.mcA = hdr->mcA;
							g.mcB = hdr->mcB;
							g.zeroToOne = (hdr->flags & proto::kOverlayZeroToOne) != 0;
							g.haveFrame = true;
							uploaded = true;
							++g_framesUploaded;
							const double age = link.CameraAgeMs(hdr->cameraFrame);
							if (age >= 0.0) {
								g_ageSum += age;
								g_ageMax = std::max(g_ageMax, age);
								++g_ageCount;
							}
							if (!g.loggedFirstFrame) {
								g.loggedFirstFrame = true;
								g_sdk->logger->InfoF(g_handle, "overlay: first HUD frame received (%u x %u, %s)", w, h, layered ? "layered: world colour, world depth, overlay" : "plain");
							}
							// The picture is stretched over the whole screen, so it has to be the same shape as the screen
							// for Minecraft's blocks to line up with Night City.
							const float hudAspect = float(w) / float(h);
							const float screenAspect = float(bb.Width) / float(bb.Height);
							if (std::fabs(hudAspect - screenAspect) > 0.02f * screenAspect && (g.warnedW != w || g.warnedH != h)) {
								g.warnedW = w;
								g.warnedH = h;
								g_sdk->logger->WarnF(g_handle, "overlay: Minecraft's window is %u x %u (shape %.3f) but the game screen is %u x %u (shape %.3f); blocks will not line up. Resize Minecraft's window to the same shape",
									w, h, hudAspect, static_cast<unsigned>(bb.Width), bb.Height, screenAspect);
							}
						}
					}
				}
			}
			const bool drawHud = (hudFresh || uploaded) && g.haveFrame && g.texture && (!g.layered || (g.texDepth && g.texOverlay));

			// A layered frame is hidden behind the game's world using the capture's copy of the game's depth, when there is one.
			depth::GameDepth gameDepth;
			bool useGameDepth = false;
			if (drawHud && g.layered && depth::GetGameDepth(gameDepth)) {
				if (g.boundGameDepth != gameDepth.resource || g.boundGameW != gameDepth.width || g.boundGameH != gameDepth.height || g.boundGameFormat != gameDepth.srvFormat) {
					WaitIdle();  // the descriptor may be in use by a frame still on the GPU
					D3D12_SHADER_RESOURCE_VIEW_DESC view{};
					view.Format = gameDepth.srvFormat;
					view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
					view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
					view.Texture2D.MipLevels = 1;
					view.Texture2D.PlaneSlice = 0;
					D3D12_CPU_DESCRIPTOR_HANDLE h = g.srvHeap->GetCPUDescriptorHandleForHeapStart();
					h.ptr += SIZE_T(3) * g.srvInc;
					g.device->CreateShaderResourceView(gameDepth.resource, &view, h);
					g.boundGameDepth = gameDepth.resource;
					g.boundGameW = gameDepth.width;
					g.boundGameH = gameDepth.height;
					g.boundGameFormat = gameDepth.srvFormat;
					g_sdk->logger->InfoF(g_handle, "overlay: hiding blocks behind the game's world, using its depth (%u x %u)", gameDepth.width, gameDepth.height);
				}
				useGameDepth = true;
				auto toRead = Transition(gameDepth.resource, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
				g.list->ResourceBarrier(1, &toRead);
			}

			// Draw onto the back buffer.
			auto toTarget = Transition(backBuffer.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
			g.list->ResourceBarrier(1, &toTarget);

			D3D12_CPU_DESCRIPTOR_HANDLE rtv = g.rtvHeap->GetCPUDescriptorHandleForHeapStart();
			rtv.ptr += SIZE_T(slot) * g.rtvSize;
			D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
			rtvDesc.Format = rtvFormat;
			rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
			g.device->CreateRenderTargetView(backBuffer.Get(), &rtvDesc, rtv);

			g.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
			ID3D12DescriptorHeap* heaps[] = { g.srvHeap.Get() };
			g.list->SetDescriptorHeaps(1, heaps);
			g.list->SetGraphicsRootSignature(g.rootSignature.Get());
			g.list->SetPipelineState(g.pso.Get());
			g.list->SetGraphicsRootDescriptorTable(0, g.srvHeap->GetGPUDescriptorHandleForHeapStart());
			g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

			D3D12_VIEWPORT viewport{ 0.0f, 0.0f, float(bb.Width), float(bb.Height), 0.0f, 1.0f };
			g.list->RSSetViewports(1, &viewport);

			if (drawHud) {
				++g_hudDraws;
				D3D12_RECT scissor{ 0, 0, LONG(bb.Width), LONG(bb.Height) };
				g.list->RSSetScissorRects(1, &scissor);
				++(g.layered ? g_layeredDraws : g_plainDraws);
				if (useGameDepth) {
					++g_depthDraws;
				}
				float constants[16] = { g.flipY ? 1.0f : 0.0f, 0.0f, 0.0f, g.layered ? 1.0f : 0.0f, 0.0f, 0.0f, g.zeroToOne ? 1.0f : 0.0f, useGameDepth ? 1.0f : 0.0f,
					g.mcA, g.mcB, 0.02f /* the game's near plane: its depth is 0.02 / distance */, 0.06f, 0.004f, float(g_debugView), 0.0f, 0.0f };
				if (input::CursorVisible()) {
					// The cursor lives in the HUD's pixels; the HUD is stretched over the screen.
					float cx = 0.0f;
					float cy = 0.0f;
					input::CursorPosition(cx, cy);
					constants[2] = 1.0f;
					constants[4] = g.texW ? cx * float(bb.Width) / float(g.texW) : cx;
					constants[5] = g.texH ? cy * float(bb.Height) / float(g.texH) : cy;
				}
				g.list->SetGraphicsRoot32BitConstants(1, 16, constants, 0);
				g.list->DrawInstanced(3, 1, 0, 0);
			} else if (badge) {
				++g_badgeDraws;
				D3D12_RECT scissor{ 8, 8, 72, 72 };  // the test square
				g.list->RSSetScissorRects(1, &scissor);
				const float constants[16] = { 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
				g.list->SetGraphicsRoot32BitConstants(1, 16, constants, 0);
				g.list->DrawInstanced(3, 1, 0, 0);
			}

			if (useGameDepth) {
				// The capture's next copy into this texture assumes it is waiting in the copy-destination state.
				auto toCopyDest = Transition(gameDepth.resource, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
				g.list->ResourceBarrier(1, &toCopyDest);
			}
			auto toPresent = Transition(backBuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
			g.list->ResourceBarrier(1, &toPresent);

			if (FAILED(g.list->Close())) {
				return;
			}
			ID3D12CommandList* lists[] = { g.list.Get() };
			g.queue->ExecuteCommandLists(1, lists);
			g.queue->Signal(g.fence.Get(), ++g.fenceValue);
			g.frameFence[slot] = g.fenceValue;
			++g.frame;
		}

		bool g_inHook = false;

		void LogStatus()
		{
			const auto now = std::chrono::steady_clock::now();
			if (now - g_lastStatus < std::chrono::seconds(5)) {
				return;
			}
			g_lastStatus = now;
			auto& link = Link::Get();
			const bool mcAlive = link.IsOpen() && link.McPid() != 0 && (::GetTickCount64() - link.McHeartbeatMs()) < 3000;
			g_sdk->logger->InfoF(g_handle,
				"overlay: status: %llu presents, in game=%d, Minecraft linked=%d, frames sent by Minecraft so far=%llu, frames uploaded=%llu, HUD draws=%llu, test-square draws=%llu, GPU ready=%d, failed=%d",
				static_cast<unsigned long long>(g_presents), link.InGame() ? 1 : 0, mcAlive ? 1 : 0,
				static_cast<unsigned long long>(mcAlive ? link.OverlayFramesPublished() : 0), static_cast<unsigned long long>(g_framesUploaded),
				static_cast<unsigned long long>(g_hudDraws), static_cast<unsigned long long>(g_badgeDraws), g.ready ? 1 : 0, g.failed ? 1 : 0);
			g_sdk->logger->InfoF(g_handle, "overlay: draws so far: %llu plain, %llu layered, %llu of those hidden against the game's depth",
				static_cast<unsigned long long>(g_plainDraws), static_cast<unsigned long long>(g_layeredDraws), static_cast<unsigned long long>(g_depthDraws));
			if (g_ageCount > 0) {
				g_sdk->logger->InfoF(g_handle, "overlay: delay: the picture Minecraft sends was drawn through a camera published %.0f ms earlier on average (up to %.0f ms), over %llu frames; the game presented about %.0f frames a second, Minecraft sent about %.0f",
					g_ageSum / double(g_ageCount), g_ageMax, static_cast<unsigned long long>(g_ageCount), double(g_presents) / 5.0, double(g_ageCount) / 5.0);
			}
			g_ageSum = g_ageMax = 0.0;
			g_ageCount = 0;
			g_presents = 0;
		}

		HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* a_swapChain, UINT a_sync, UINT a_flags)
		{
			++g_presents;
			LogStatus();
			cybercraft::depth::Report();
			if (!g_inHook && a_swapChain == g_gameSwapChain && (a_flags & DXGI_PRESENT_TEST) == 0) {
				g_inHook = true;
				Draw(a_swapChain);
				g_inHook = false;
			}
			return g_origPresent(a_swapChain, a_sync, a_flags);
		}

		HRESULT STDMETHODCALLTYPE HookPresent1(IDXGISwapChain1* a_swapChain, UINT a_sync, UINT a_flags, const DXGI_PRESENT_PARAMETERS* a_params)
		{
			++g_presents;
			LogStatus();
			if (!g_inHook && static_cast<IDXGISwapChain*>(a_swapChain) == g_gameSwapChain && (a_flags & DXGI_PRESENT_TEST) == 0) {
				g_inHook = true;
				Draw(a_swapChain);
				g_inHook = false;
			}
			return g_origPresent1(a_swapChain, a_sync, a_flags, a_params);
		}

		// The first swapchain the game has in use, or null. (Plain data only, so a bad read can be caught.)
		IDXGISwapChain* FindGameSwapChain()
		{
			auto* data = RED4ext::GpuApi::GetDeviceData();
			if (!data) {
				return nullptr;
			}
			auto& chains = data->swapChains;
			for (std::size_t i = 0; i < 32; ++i) {
				auto& entry = chains.resources[i];
				if (entry.refCount >= 0 && entry.instance.swapChain.Get() != nullptr) {
					g_window = entry.instance.windowHandle;
					return entry.instance.swapChain.Get();
				}
			}
			return nullptr;
		}

		void Patch(UINT a_index, void* a_hook, void** a_original)
		{
			void** slot = &g_vtable[a_index];
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

	void Install()
	{
		if (g_installed || g_triedInstall) {
			return;
		}

		// The swapchain may not exist yet when the game first starts running: look again every couple of
		// seconds, and give up after a minute or so.
		static int calls = 0;
		static int attempts = 0;
		if (calls++ % 120 != 0) {
			return;
		}
		if (++attempts > 30) {
			g_triedInstall = true;
			g_sdk->logger->Warn(g_handle, "overlay: gave up looking for the game's swapchain; the HUD overlay is off");
			return;
		}

		IDXGISwapChain* swapChain = FindGameSwapChain();
		if (!swapChain) {
			return;
		}
		ComPtr<IDXGISwapChain3> check;
		if (FAILED(swapChain->QueryInterface(IID_PPV_ARGS(&check)))) {
			g_triedInstall = true;
			g_sdk->logger->Warn(g_handle, "overlay: the game's swapchain doesn't answer as an IDXGISwapChain3; the HUD overlay is off");
			return;
		}
		g_triedInstall = true;

		g_gameSwapChain = swapChain;
		g_vtable = *reinterpret_cast<void***>(swapChain);
		Patch(kPresentIndex, reinterpret_cast<void*>(&HookPresent), reinterpret_cast<void**>(&g_origPresent));
		Patch(kPresent1Index, reinterpret_cast<void*>(&HookPresent1), reinterpret_cast<void**>(&g_origPresent1));
		g_installTime = std::chrono::steady_clock::now();
		g_installed = g_origPresent != nullptr && g_origPresent1 != nullptr;
		g_sdk->logger->InfoF(g_handle, "overlay: hooked the game's swapchain %p (Present %p, Present1 %p)", static_cast<void*>(swapChain),
			reinterpret_cast<void*>(g_origPresent), reinterpret_cast<void*>(g_origPresent1));
	}

	void* GameWindow()
	{
		return g_installed ? g_window : nullptr;
	}

	void SetDebugView(int a_view)
	{
		a_view = std::clamp(a_view, 0, 3);
		if (a_view != g_debugView) {
			g_debugView = a_view;
			g_sdk->logger->InfoF(g_handle, "overlay: depth debug view %d", a_view);
		}
	}

	void Uninstall()
	{
		if (!g_installed || !g_vtable) {
			return;
		}
		Patch(kPresentIndex, reinterpret_cast<void*>(g_origPresent), reinterpret_cast<void**>(&g_origPresent));
		Patch(kPresent1Index, reinterpret_cast<void*>(g_origPresent1), reinterpret_cast<void**>(&g_origPresent1));
		g_installed = false;
	}
}
