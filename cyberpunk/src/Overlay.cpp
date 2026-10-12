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
#include "Log.hpp"
#include "UiProbe.hpp"
#include "InScene.hpp"
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
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>

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
		std::recursive_mutex g_gpuMutex;           // Draw (at Present) and DrawWorldIntoScene (from the game's queue submission) share the textures and descriptors
		float g_sceneGain = 1.5f;                  // the blocks' brightness in the game's HDR scene (1 = a Minecraft white is 1.0 in the scene's units)
		float g_sceneAo = 1.0f;                    // ambient occlusion where blocks meet the game's world: strength 0 (off) to 1
		float g_terrainAo = 0.6f;                  // the same on the game's own surfaces next to blocks (the shadow a block makes on the road): strength 0 (off) to 1
		float g_sceneAoRadius = 0.8f;              // metres
		bool g_sceneAoView = false;                // show only the occlusion term
		float g_glowStart = 0.7f;                  // how bright a pixel has to be (after undoing Minecraft's face shading) to count as light-emitting: 0.3 to 0.99
		float g_fogGlowExp = 0.0f;                 // how much of the fog the glowing pixels of a block feel: the transmittance to this power (1: all of it, 0: none)
		float g_sceneGlow = 15.0f;                 // how much the brightest pixels are boosted in the scene: up to (1 + this) times
		const char* g_sceneWhy = "";                // why the last attempt to draw the blocks into the scene drew nothing
		float g_sceneDelayMs = 0.0f;               // how far behind the newest published camera the blocks are aimed when drawn into the scene
		std::atomic<std::uint64_t> g_sceneDrawn{ 0 };  // how many times the blocks were drawn into the scene
		std::uint64_t g_sceneSeen = 0;
		float g_uiScaleX = 1.0f;  // the game's interface layer is stretched by this much across, and by g_uiScaleY down, about the middle of the screen (and then shifted) before it is laid over the blocks
		float g_uiScaleY = 1.0f;
		float g_uiShiftX = 0.0f;  // pixels
		float g_uiShiftY = 0.0f;
		int g_fogView = 0;                          // 0 off, 1 show a fog volume's colour on the blocks, 2 its transmittance (a debug view)
		int g_fogIndex = 0;                         // which of the fog volumes found; negative: the one the compute shaders write twice a frame (the integrated volume)
		float g_fogNear = 0.5f;                     // metres: the distance of the volume's first slice (a guess to be tuned by eye)
		float g_fogFar = 200.0f;                    // metres: the distance of its last slice
		bool g_fogExp = true;                       // the slices are spread exponentially with distance (otherwise evenly)
		std::vector<FogVolume> g_fogVolumes;        // the fog volumes as of the scene being complete, sorted by address (set by the in-scene hook)
		int g_uiMode = 0;  // the game's interface over the blocks: 0 off, 1 the layer is premultiplied alpha, 2 it is straight alpha, 3 show the layer alone
		bool g_warpEnabled = true;
		float g_warpDelayMs = 10.0f;
		std::uint64_t g_warpDraws = 0;
		std::uint64_t g_framesUploaded = 0;
		std::uint64_t g_layeredDraws = 0;
		std::uint64_t g_plainDraws = 0;
		std::uint64_t g_depthDraws = 0;
		double g_ageSum = 0.0;
		double g_ageMax = 0.0;
		std::uint64_t g_ageCount = 0;
		bool g_wasInGame = false;
		std::chrono::steady_clock::time_point g_inGameSince{};

		const char* kShaderSource =
R"HLSL(
cbuffer P : register(b0)
{
	float flipY; float mode; float cursorOn; float layered;
	float2 cursor; float zeroToOne; float haveGameDepth;
	float mcA; float mcB; float gameNear; float biasAbs;
	float biasRel; float debugView; float uiMode; float haveUi;
	float4 warpR0; float4 warpR1; float4 warpR2;   // rows of the matrix from Minecraft's camera space to the current camera space
	float4 warpT;                                  // xyz: where Minecraft's camera is, in the current camera space; w: 1 = re-aim
	float4 tans;                                   // tan(half field of view): current x, y; Minecraft's x, y
	float4 uiXform;                                // the interface layer: x: stretch across and w: stretch down, both about the middle of the screen; y, z: shift in pixels
	float4 sceneParams;                            // x: 1 = draw into the game's HDR scene (linear, scaled by y, brightest pixels boosted by up to w); z: 1 = leave the blocks out (they were drawn into the scene)
	float4 shadeX; float4 shadeY; float4 shadeZ;   // rows of the matrix from Minecraft's camera space to Minecraft's world (xyz), w of shadeX: 1 = valid: used to tell which way a face points
	float4 aoParams;                               // ambient occlusion where blocks meet the game's world: x: the blocks' occlusion strength (0 = off; negative = show only the occlusion term, white for none and dark for a lot), y: radius in metres, z: the occlusion strength on the game's own surfaces next to blocks, w: tan(half the vertical field of view)
	float3 fogParams;                              // the game's volumetric fog on the blocks: x: 0 off, 1 its colour shown instead of the blocks, 2 its transmittance shown, 3 applied to the blocks (block * transmittance + fog light); y: the volume's near distance in metres; z: its far distance (negative: the slices are spread evenly, positive: exponentially)
};
Texture2D tex : register(t0);          // the whole frame (plain mode), or the world's colour (layered)
Texture2D mcDepthTex : register(t1);   // layered: the world's depth, as Minecraft wrote it
Texture2D overlayTex : register(t2);   // layered: the hand, hotbar and screens
Texture2D gameDepthTex : register(t3); // the game's own depth (a copy), when there is one
Texture2D uiTex : register(t4);        // the game's interface layer (a copy), when there is one: its sRGB-encoded values as they are
Texture2D sceneTex : register(t8);      // the game's HDR scene, while its brightness is being measured
Texture2D smallTex : register(t9);      // the scene's log brightness in a grid of 16 x 9 cells
Texture2D adaptPrevTex : register(t10); // last frame's result: r: the smoothed log brightness, g: the gain multiplier, b: the reference log brightness
Texture2D adaptTex : register(t11);     // this frame's
Texture2D gbufTex0 : register(t12);     // the G-buffer layers as they were copied before the lighting (for the viewer)
Texture2D gbufTex1 : register(t13);
Texture2D gbufTex2 : register(t14);
Texture2D gbufTex3 : register(t15);
Texture3D emitTex : register(t7);       // the light emission level (0 to 15) of every block around the player, one byte each: which blocks are light sources
Texture3D fogTex : register(t6);       // one of the game's volumetric fog textures (the screen divided by 8 across, 128 slices deep), when one is being looked at
Texture2D blockDepthTex : register(t5); // how far the nearest block is at each place on the screen, as seen from the current camera (0: none); made by the scene's first pass
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
// A place in Minecraft's picture, in Minecraft's camera space (x right, y up, z forward), from the picture's own depth.
float3 McViewPos(float2 texUv)
{
	uint mw, mh;
	mcDepthTex.GetDimensions(mw, mh);
	uint2 px = clamp(uint2(texUv * float2(mw, mh)), uint2(0, 0), uint2(mw - 1, mh - 1));
	float s = mcDepthTex.Load(int3(px, 0)).r;
	float ndcz = zeroToOne > 0.5 ? s : s * 2.0 - 1.0;
	float den = ndcz + mcA;
	float zk = abs(den) > 1e-9 ? mcB / den : 1e9;
	float2 tuv = flipY > 0.5 ? float2(texUv.x, 1.0 - texUv.y) : texUv;
	float2 nm = float2(tuv.x * 2.0 - 1.0, 1.0 - tuv.y * 2.0);
	return float3(nm * tans.zw, 1.0) * zk;
}
// Which way the surface at this place in Minecraft's picture faces, in Minecraft's camera space, from the slope of the picture's depth (zero if it can't tell).
float3 McNormalWithin(float2 texUv, float maxDistance)
{
	uint mw, mh;
	mcDepthTex.GetDimensions(mw, mh);
	float2 d = 1.0 / float2(mw, mh);
	float3 pc = McViewPos(texUv);
	if (pc.z <= 0.0 || pc.z > maxDistance) { return float3(0, 0, 0); }
	float3 pr = McViewPos(texUv + float2(d.x, 0));
	float3 pl = McViewPos(texUv - float2(d.x, 0));
	float3 pu = McViewPos(texUv + float2(0, d.y));
	float3 pd = McViewPos(texUv - float2(0, d.y));
	// The smaller step along each axis, so that the edge of a block doesn't make a normal from two different surfaces.
	float3 dx = abs(pr.z - pc.z) < abs(pc.z - pl.z) ? pr - pc : pc - pl;
	float3 dy = abs(pu.z - pc.z) < abs(pc.z - pd.z) ? pu - pc : pc - pd;
	float3 n = cross(dx, dy);
	float len = length(n);
	if (len < 1e-12) { return float3(0, 0, 0); }
	n /= len;
	if (dot(n, pc) > 0.0) { n = -n; }   // facing the camera
	return n;
}
float3 McNormal(float2 texUv)
{
	return McNormalWithin(texUv, 48.0);
}
// How bright Minecraft made the face at this place: 1 for a top, 0.8 north and south, 0.6 east and west, 0.5 a bottom (1 if it can't tell). The way the face points
// comes from McNormal and the camera's axes in Minecraft's world.
float McShade(float2 texUv)
{
	if (shadeX.w < 0.5) { return 1.0; }
	float3 n = McNormal(texUv);
	if (dot(n, n) < 0.5) { return 1.0; }
	float3 nw = float3(dot(shadeX.xyz, n), dot(shadeY.xyz, n), dot(shadeZ.xyz, n));
	if (abs(nw.y) > 0.7) { return nw.y > 0.0 ? 1.0 : 0.5; }
	return abs(nw.z) > abs(nw.x) ? 0.8 : 0.6;
}
// How much light the block this pixel shows gives out, from the grid Minecraft publishes: its light level / 15, or -1 if there is no grid or the pixel is outside it. Where the block is: the
// pixel's place in Minecraft's camera space (from the picture's depth), moved a little way into the surface (against its normal, which faces the camera) so that it is inside the block,
// then turned into Minecraft's world with the camera's axes and measured from the grid's corner (uiXform.xyz is the camera's place relative to that corner).
float EmissionAt(float2 texUv)
{
	if (uiXform.w < 0.5 || shadeX.w < 0.5) { return -1.0; }
	float3 Pc = McViewPos(texUv);
	if (Pc.z <= 0.0 || Pc.z > 400.0) { return -1.0; }
	// The surface direction is worked out as far as the grid goes, and the step into the block grows with distance (the depth is less exact far away).
	float3 n = McNormalWithin(texUv, 400.0);
	float push = min(0.12 + 0.0005 * Pc.z, 0.4);
	float3 Pi = dot(n, n) > 0.5 ? Pc - n * push : Pc + normalize(Pc) * (push * 0.5);
	float3 Pw = uiXform.xyz + float3(dot(shadeX.xyz, Pi), dot(shadeY.xyz, Pi), dot(shadeZ.xyz, Pi));
	int3 v = int3(floor(Pw));
	uint ew, eh, ed;
	emitTex.GetDimensions(ew, eh, ed);
	if (any(v < 0) || v.x >= int(ew) || v.y >= int(eh) || v.z >= int(ed)) { return -1.0; }
	return round(emitTex.Load(int4(v, 0)).r * 255.0) / 15.0;
}

// Ambient occlusion where a block meets the game's world. Points are taken in 3D on the side of the surface at P that faces N (spread over a hemisphere, turned a little at every
// pixel), each is projected onto the screen, and it counts as hidden if the game's depth there is in front of it (the point is inside, or behind, the game's geometry) and the
// game's surface is near P in depth. Taking the points in 3D rather than around the pixel on the screen matters: a road seen at a shallow angle is only a few pixels tall on the
)HLSL"
R"HLSL(// screen however far it is in the world, so a disk of screen pixels would miss it at a distance. About 0 to 0.5: half the points are inside the ground at the foot of a wall.
float SceneAO(float2 pix, float3 P, float3 N)
{
	uint gw, gh;
	gameDepthTex.GetDimensions(gw, gh);
	float2 size = float2(gw, gh);
	float tanY = aoParams.w;
	float2 tanXY = float2(tanY * size.x / size.y, tanY);
	float radius = aoParams.y;
	float noise = frac(52.9829189 * frac(dot(pix, float2(0.06711056, 0.00583715))));
	float3 up = abs(N.y) < 0.9 ? float3(0, 1, 0) : float3(1, 0, 0);
	float3 T = normalize(cross(up, N));
	float3 B = cross(N, T);
	float bias = 0.02 + 0.004 * P.z;
	float occ = 0.0;
	[unroll] for (int k = 0; k < 16; ++k) {
		float u = (float(k) + 0.5) / 16.0;
		float phi = (float(k) + noise) * 2.39996323;     // the golden angle: an even spread that is turned a little at every pixel
		float sinT = sqrt(u);
		float cosT = sqrt(1.0 - u);
		float3 dir = T * (cos(phi) * sinT) + B * (sin(phi) * sinT) + N * cosT;
		float sc = radius * (0.25 + 0.75 * frac(float(k) * 0.61803399 + noise));
		float3 S = P + dir * sc;
		if (S.z < 0.05) { continue; }
		float2 ndcS = S.xy / (S.z * tanXY);
		float2 sp = float2((ndcS.x * 0.5 + 0.5) * size.x, (0.5 - ndcS.y * 0.5) * size.y);
		if (sp.x < 0.0 || sp.y < 0.0 || sp.x >= size.x || sp.y >= size.y) { continue; }
		float sg = gameDepthTex.Load(int3(uint2(sp), 0)).r;
		if (sg <= 1e-8) { continue; }                    // sky
		float zs = gameNear / sg;
		if (zs < S.z - bias) {
			occ += saturate(radius / max(abs(P.z - zs), 1e-3));   // something far in front of the block doesn't count
		}
	}
	return occ / 16.0;
}
// The game's depth at a pixel, as a distance (huge for the sky).
float GameZ(int2 p, int2 size)
{
	float sg = gameDepthTex.Load(int3(clamp(p, int2(0, 0), size - 1), 0)).r;
	return sg > 1e-8 ? gameNear / sg : 1e9;
}
// Re-aiming, part one: is the point on the current camera's line of sight at distance t behind the surface that Minecraft's (older) picture shows there? The line is
// centre + t * direction in Minecraft's camera space (t is also the distance along the current camera's view). nm is where that point is in Minecraft's picture (-1 to 1), and gap how far
// behind that picture's surface it is (a huge number if the point is off the picture or behind the camera).
bool PointBehind(float3 centre, float3 direction, float t, uint mw, uint mh, out float2 nm, out float gap)
{
	float3 X = centre + t * direction;
	nm = float2(2.0, 2.0);
	gap = 1e9;
	if (X.z <= 0.01) { return false; }
	nm = X.xy / X.z / tans.zw;
	if (abs(nm.x) > 1.0 || abs(nm.y) > 1.0) { return false; }
	float2 tuv = float2(nm.x * 0.5 + 0.5, 0.5 - nm.y * 0.5);
	float2 tex2 = flipY > 0.5 ? float2(tuv.x, 1.0 - tuv.y) : tuv;
	float s = mcDepthTex.Load(int3(clamp(uint2(tex2 * float2(mw, mh)), uint2(0, 0), uint2(mw - 1, mh - 1)), 0)).r;
	float ndcz = zeroToOne > 0.5 ? s : s * 2.0 - 1.0;
	float den = ndcz + mcA;
	float zk = abs(den) > 1e-9 ? mcB / den : 1e9;
	if (zk <= 0.0) { zk = 1e9; }
	gap = X.z - zk;
	return gap >= 0.0;
}
// Re-aiming, part two: for this place on the current camera's screen, where in Minecraft's older picture is the nearest block along the line of sight? Walks the line from near to far
// in steps of equal pixels in the old picture (equal steps of one over the distance, because the picture's parallax is proportional to that) looking for where it goes from in front of
// a surface to behind it, refines that crossing by bisection, and only then checks that the point found is not more than a block's thickness behind the surface (otherwise the line
// went behind the block's edge: keep looking). The check must come after the refining: the steps are further apart in metres the further away they are, so a thickness test on the steps
// themselves misses far blocks altogether, which showed as bands that flickered with the speed. The fixed point iteration this replaced started at the pixel's own place and, where the
// camera had moved, landed beside a block's edge (on empty space, whose depth says "far away"): a strip of the block along that edge vanished, as wide as the shift.
bool FindSource(float2 ndc, uint mw, uint mh, out float2 nm, out float zHit)
{
	float3 dc = float3(ndc * tans.xy, 1.0);
	float3 direction = warpR0.xyz * dc.x + warpR1.xyz * dc.y + warpR2.xyz * dc.z;               // the line of sight, in Minecraft's camera space
	float3 centre = -(warpR0.xyz * warpT.x + warpR1.xyz * warpT.y + warpR2.xyz * warpT.z);     // where the current camera is, in Minecraft's camera space
	nm = float2(2.0, 2.0);
	zHit = 1e9;
	if (direction.z <= 0.01) { return false; }
	float moved = length(centre.xy) + 0.5 * abs(centre.z);
	if (moved < 0.002) {
		// The camera hasn't moved (only turned): the same direction in the old picture, whatever the depth.
		nm = direction.xy / direction.z / tans.zw;
		if (abs(nm.x) > 1.0 || abs(nm.y) > 1.0) { return false; }
		float2 tuv0 = float2(nm.x * 0.5 + 0.5, 0.5 - nm.y * 0.5);
		float2 tex0 = flipY > 0.5 ? float2(tuv0.x, 1.0 - tuv0.y) : tuv0;
		float s0 = mcDepthTex.Load(int3(clamp(uint2(tex0 * float2(mw, mh)), uint2(0, 0), uint2(mw - 1, mh - 1)), 0)).r;
		float nz = zeroToOne > 0.5 ? s0 : s0 * 2.0 - 1.0;
		float dn = nz + mcA;
		zHit = abs(dn) > 1e-9 ? mcB / dn : 1e9;
		if (zHit <= 0.0) { zHit = 1e9; }
		if (zHit < 1e8) {
			// The distance along the current camera's view of the point Minecraft's picture shows here (its own distance is along Minecraft's axis: they differ when the camera has turned).
			float3 pm = float3(nm * tans.zw, 1.0) * zHit;
			float3 pc = float3(dot(warpR0.xyz, pm), dot(warpR1.xyz, pm), dot(warpR2.xyz, pm)) + warpT.xyz;
			zHit = pc.z > 0.0 ? pc.z : 1e9;
		}
		return true;
	}
	float focal = 0.5 * float(mh) / tans.w;                   // pixels of Minecraft's picture per unit of tangent
	float uNear = 1.0 / 0.8;                                   // nearest block looked for: 0.8 m
	float uFar = 1.0 / 250.0;                                  // furthest: 250 m
	float steps = clamp(ceil(focal * moved * (uNear - uFar) / 4.0), 6.0, 64.0);   // about four pixels a step, as many as the movement needs
	float du = (uNear - uFar) / steps;
	float tPrev = 1.0 / uNear;
	bool prevBehind = false;
	[loop] for (int k = 0; k <= 64; ++k) {
		if (float(k) > steps) { break; }
		float t = 1.0 / max(uNear - du * float(k), 1e-4);
		float2 pn;
		float gap;
		bool behind = PointBehind(centre, direction, t, mw, mh, pn, gap);
		if (behind && !prevBehind) {
			// The line has just gone from in front of a surface to behind one: find where, between the last step and this one.
			float lo = tPrev;
			float hi = t;
			if (k > 0) {
				[loop] for (int b = 0; b < 18; ++b) {
					if (hi - lo < 0.01 + 0.0005 * hi) { break; }   // about a centimetre close, and a twentieth of a percent far away
					float mid = 0.5 * (lo + hi);
					float2 pm2;
					float g2;
					if (PointBehind(centre, direction, mid, mw, mh, pm2, g2)) { hi = mid; } else { lo = mid; }
				}
			}
			float2 nmHit;
			float gapHit;
			PointBehind(centre, direction, hi, mw, mh, nmHit, gapHit);
			if (gapHit < 1.5) {
				nm = nmHit;
				zHit = hi;
				return true;
			}
		}
		prevBehind = behind;
		tPrev = t;
	}
	return false;
}
// One texel of a G-buffer layer copy (layer 1 to 4), by the position on the screen.
float4 GbufSample(uint layer, float2 uv)
{
	uint w = 1, h = 1;
	float4 v = float4(0.0, 0.0, 0.0, 1.0);
	if (layer == 1) { gbufTex0.GetDimensions(w, h); v = gbufTex0.Load(int3(min(uint2(uv * float2(w, h)), uint2(max(w, 1) - 1, max(h, 1) - 1)), 0)); }
	else if (layer == 2) { gbufTex1.GetDimensions(w, h); v = gbufTex1.Load(int3(min(uint2(uv * float2(w, h)), uint2(max(w, 1) - 1, max(h, 1) - 1)), 0)); }
	else if (layer == 3) { gbufTex2.GetDimensions(w, h); v = gbufTex2.Load(int3(min(uint2(uv * float2(w, h)), uint2(max(w, 1) - 1, max(h, 1) - 1)), 0)); }
)HLSL"
R"HLSL(	else if (layer == 4) { gbufTex3.GetDimensions(w, h); v = gbufTex3.Load(int3(min(uint2(uv * float2(w, h)), uint2(max(w, 1) - 1, max(h, 1) - 1)), 0)); }
	return v;
}
float4 PS(VSOut i) : SV_Target
{
	if (sceneParams.x < 0.5 && fogParams.x > 0.5) {
		// The G-buffer viewer (only the Present path comes here: its sceneParams.x is 0): fogParams.x is the layer 1 to 4, y the channel (0 all three colours, 1 r, 2 g, 3 b, 4 a), z the scale.
		uint layer = uint(fogParams.x + 0.5);
		uint ch = uint(fogParams.y + 0.5);
		float4 v = GbufSample(layer, i.uv);
		float scale = max(fogParams.z, 0.0);
		float3 c;
		if (layer == 4) {
			// Motion vectors (two floats): no motion is grey.
			float2 m = v.rg * scale * 8.0;
			float one = ch == 2 ? m.y : m.x;
			c = ch == 0 ? float3(0.5 + m.x, 0.5 + m.y, 0.5) : float3(0.5 + one, 0.5 + one, 0.5 + one);
		} else {
			c = ch == 0 ? v.rgb : ch == 1 ? v.rrr : ch == 2 ? v.ggg : ch == 3 ? v.bbb : v.aaa;
			c *= scale;
		}
		return float4(saturate(c), 1.0);
	}
	if (mode > 0.5) {
		return float4(0.0, 0.5, 0.6, 0.6);   // the test square: cyan at 60%, premultiplied
	}
	float2 uv = i.uv;
	if (flipY > 0.5) { uv.y = 1.0 - uv.y; }
	float4 c;
	if (layered > 0.5) {
		float4 w;
		float zm = 1e9;
		bool hidden = false;
		float2 mcUv = uv;   // where in Minecraft's picture this pixel's block was taken from
		uint mw, mh;
		mcDepthTex.GetDimensions(mw, mh);
		if (warpT.w > 0.5) {
			// Re-aim: for this place on the screen, where in Minecraft's (older) picture is the nearest block along the line of sight (see FindSource)?
			float2 ndc = float2(i.uv.x * 2.0 - 1.0, 1.0 - i.uv.y * 2.0);
			float2 nm;
			float zHit;
			bool ok = FindSource(ndc, mw, mh, nm, zHit);
			float2 tuv = float2(nm.x * 0.5 + 0.5, 0.5 - nm.y * 0.5);
			float2 tex2 = flipY > 0.5 ? float2(tuv.x, 1.0 - tuv.y) : tuv;
			w = ok ? tex.Sample(samp, tex2) : float4(0, 0, 0, 0);
			mcUv = tex2;
			zm = ok ? zHit : 1e9;              // how far the block is along the current camera's view
		} else {
			w = tex.Sample(samp, uv);       // the world, premultiplied alpha
			if (w.a > 0.0) {
				// How far is this block pixel from the camera? Minecraft's depth is the matrix's: ndc = -A + B / distance.
				float s = mcDepthTex.Load(int3(min(uint2(uv * float2(mw, mh)), uint2(mw - 1, mh - 1)), 0)).r;
				float ndc = zeroToOne > 0.5 ? s : s * 2.0 - 1.0;
				float den = ndc + mcA;
				zm = abs(den) > 1e-9 ? mcB / den : 1e9;
				if (zm <= 0.0) { zm = 1e9; }
			}
		}
		if (sceneParams.x > 1.5 && sceneParams.x < 2.5) {
			// The scene's first pass: how far the nearest block is here, as seen from the current camera (0 for none), whether or not it is hidden behind the game's world.
			return float4((w.a > 0.01 && zm < 1e6) ? zm : 0.0, 0.0, 0.0, 0.0);
		}
		float zg = 1e9;
		if (haveGameDepth > 0.5) {
			// The game's world at this place on the screen: its depth is gameNear / distance (reversed, no far plane).
			uint gw, gh;
			gameDepthTex.GetDimensions(gw, gh);
			float sg = gameDepthTex.Load(int3(min(uint2(i.uv * float2(gw, gh)), uint2(gw - 1, gh - 1)), 0)).r;
			zg = sg > 1e-8 ? gameNear / sg : 1e9;
			// Hidden if it is behind what the game drew there (with a little room for rounding, and for the game's jittered depth).
			hidden = w.a > 0.0 && zm > zg + biasAbs + biasRel * zg;
			if (hidden) {
				// Where a block meets the ground at a shallow angle the ground's depth changes quickly from pixel to pixel (0.3 m a pixel at 20 m), and its depth is jittered by up to half a pixel
				// (the game's temporal anti-aliasing), while ours comes from a re-aimed picture: so along the line where they meet, pixels flipped between hidden and shown from frame to frame. Don't
				// hide a block pixel by less than a pixel's worth of the ground's slope (a jump in depth, like the edge of a building, doesn't count as slope).
				int2 gsize = int2(gw, gh);
				int2 gp = int2(min(uint2(i.uv * float2(gw, gh)), uint2(gw - 1, gh - 1)));
				float zMax = max(max(GameZ(gp + int2(1, 0), gsize), GameZ(gp - int2(1, 0), gsize)), max(GameZ(gp + int2(0, 1), gsize), GameZ(gp - int2(0, 1), gsize)));
				float slope = max(min(zMax - zg, 0.1 * zg + 0.3), 0.0);
				hidden = zm > zg + biasAbs + biasRel * zg + slope;
			}
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
		if (sceneParams.x > 0.5) {
			// How much the city's brightness (measured and smoothed by the passes before) scales the blocks' ordinary light; 1 if there is no measurement.
			float adaptM = adaptTex.Load(int3(0, 0, 0)).g;
			if (adaptM <= 0.0) { adaptM = 1.0; }
			float fogT = 1.0;
			float3 fogC = float3(0.0, 0.0, 0.0);
			bool plainBlocks = fogParams.x > 4.5;   // mode 5: flat grey blocks with only the game's fog on them
			if (fogParams.x > 0.5) {
				// The game's volumetric fog where this block is: shown instead of the block (to find out which volume is the fog the scene uses, and how distance maps to its slices),
				// or applied to it (the light the fog adds, and how much of the block shows through).
				if (fogParams.x > 6.5) {
					// The gain multiplier from the city's brightness: grey for 1, darker below, white at 4.
					float g = saturate(adaptM * 0.25);
					return float4(g, g, g, 1.0) * w.a;
				}
				if (fogParams.x > 5.5) {
					// The light level of the block, white for 15 and black for none, magenta where there is no grid: to check that the right pixels are found to be light sources.
					float e = EmissionAt(mcUv);
					return (e < 0.0 ? float4(1.0, 0.0, 1.0, 1.0) : float4(e, e, e, 1.0)) * w.a;
				}
				if (fogParams.x > 3.5 && fogParams.x < 4.5) {
					// Distance bands: each block painted with colours that repeat every metre (red), four metres (green) and twenty-five (blue): the blocks' own distance, to check it is right.
					return float4(frac(zm), frac(zm * 0.25), frac(zm * 0.04), 1.0) * w.a;
				}
				uint fw, fh, fd;
				fogTex.GetDimensions(fw, fh, fd);
				float zq = max(zm, 0.01);
				float nearD = max(fogParams.y, 0.01);
				float farD = max(abs(fogParams.z), nearD + 0.1);
				float t = fogParams.z > 0.0 ? log(zq / nearD) / log(farD / nearD) : (zq - nearD) / (farD - nearD);
				float2 uvv = i.pos.xy / (float2(fw, fh) * 8.0);
				float4 f = fogTex.SampleLevel(samp, float3(uvv, saturate(t)), 0);
				if (fogParams.x < 2.5) {
					return (fogParams.x < 1.5 ? float4(f.rgb, 1.0) : float4(f.aaa, 1.0)) * w.a;
				}
				fogT = saturate(f.a);
				fogC = f.rgb;
			}
			// Into the game's HDR scene: Minecraft's colours are gamma encoded and premultiplied; the scene is linear.
			float3 s = w.a > 1e-4 ? w.rgb / w.a : float3(0, 0, 0);
			// Minecraft's whites are only 1.0, while the city's lights are many times that: the brightest pixels (lit white, glowstone, torch flames) are boosted, the way a texture pack
			// with brighter light sources would, so that they glow when the game's bloom gets them.
			// Minecraft dims faces by direction (a top is 100%, north and south 80%, east and west 60%, a bottom 50%), so a lit side face is never as bright as a top: undo that
			// first, or only the tops would glow.
			// Which pixels are light sources: Minecraft says (the light level of the block, from the grid it publishes). Without that grid, guess from the brightness.
			float emit = EmissionAt(mcUv);
			float hot = emit;
			if (emit < 0.0) {
				float peak = max(s.r, max(s.g, s.b)) / McShade(mcUv);
				// Outside the grid (further than about 32 blocks from the player) only the very brightest pixels count, so that far sunlit pale blocks don't glow.
)HLSL"
R"HLSL(				float start = uiXform.w > 0.5 ? max(aoParams.z, 0.9) : aoParams.z;   // aoParams.z is the glow threshold in the blocks' pass (the terrain's occlusion strength in its own)
				hot = smoothstep(start, 1.0, saturate(peak));
			}
			// Ordinary light and the light sources' own light are separate: a block's colour times the gain (so that ordinary blocks can sit at the brightness of the street, where bloom ignores them),
			// and, for a light source, its colour times the glow, however low the gain is (the game's own emissives are ten to twenty times a white surface, which is what makes them bloom).
			float3 lin0 = pow(saturate(s), 2.2);
			float3 baseLin = lin0 * sceneParams.y * adaptM;
			float3 glowLin = lin0 * (sceneParams.w * hot * hot);
			if (abs(aoParams.x) > 0.001 && haveGameDepth > 0.5 && zm < 1e6 && w.a > 0.01) {
				float3 Nm = McNormal(mcUv);
				if (dot(Nm, Nm) > 0.5) {
					// The normal in the current camera's space (the picture was drawn through an older camera), and where this pixel is in that space.
					float3 Nc = warpT.w > 0.5 ? float3(dot(warpR0.xyz, Nm), dot(warpR1.xyz, Nm), dot(warpR2.xyz, Nm)) : Nm;
					uint gw2, gh2;
					gameDepthTex.GetDimensions(gw2, gh2);
					float2 ndcP = float2(i.uv.x * 2.0 - 1.0, 1.0 - i.uv.y * 2.0);
					float3 P = float3(ndcP * float2(aoParams.w * float(gw2) / float(gh2), aoParams.w) * zm, zm);
					float ao = 1.0 - saturate(SceneAO(i.pos.xy, P, Nc) * 1.6 * abs(aoParams.x));
					if (aoParams.x < 0.0) { return float4(ao, ao, ao, 1.0) * w.a; }   // the occlusion alone: white for none, dark for a lot
					baseLin *= ao;
					glowLin *= ao;
				}
			}
			if (plainBlocks) {
				baseLin = float3(0.4, 0.4, 0.4);   // no texture, no Minecraft shading, no occlusion, no glow: the fog alone
				glowLin = float3(0.0, 0.0, 0.0);
			}
			// The game's fog: how much of the block shows through, plus the light the fog adds. The extra light of glowing pixels is held back less than that of ordinary ones: Cyberpunk's own
			// emissives are tens of times brighter than a white surface, which is why they still glow (and bloom) after the fog has taken most of their light, while ours start only a few
			// times brighter. So the glow's share is the transmittance to a power below one (sceneParams.z: 1 is the physical amount, 0 no fog on the glow at all).
			float glowT = exp2(sceneParams.z * log2(max(fogT, 1e-4)));
			float3 lin = baseLin * fogT + glowLin * glowT + fogC;
			return float4(lin * w.a, w.a);
		}
		if (sceneParams.z > 0.5) { w = float4(0, 0, 0, 0); }
		if (uiMode > 0.5 && haveUi > 0.5) {
			uint uw, uh;
			uiTex.GetDimensions(uw, uh);
			float2 usize = float2(uw, uh);
			float2 uq = (i.pos.xy - usize * 0.5) / float2(uiXform.x, uiXform.w) + usize * 0.5 - uiXform.yz;   // where in the layer this pixel is, after the stretch and shift
			float4 u = (all(uq >= 0.0) && all(uq < usize)) ? uiTex.SampleLevel(samp, uq / usize, 0) : float4(0, 0, 0, 0);
			if (uiMode > 3.5) {
				return float4(float3(1.0, 0.0, 1.0) * u.a * 0.6, u.a * 0.6);   // a magenta shadow of the layer over everything, to line it up with the real interface
			}
			if (uiMode > 2.5) {
				return float4(u.rgb + float3(0.35, 0.0, 0.35) * (1.0 - u.a), 1.0);   // the layer alone; where it is transparent, magenta
			}
			if (w.a > 0.0) {
				// The game's interface over the blocks (the picture underneath already has it on, so only pixels with blocks need this).
				float3 ur = uiMode > 1.5 ? u.rgb * u.a : u.rgb;
				w = float4(ur + w.rgb * (1.0 - u.a), u.a + w.a * (1.0 - u.a));
			}
		}
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

// The game's own surface at a pixel, in the current camera's space.
float3 GameViewPos(float2 pix, float2 size, float2 tanXY)
{
	float sg = gameDepthTex.Load(int3(uint2(clamp(pix, float2(0, 0), size - 1.0)), 0)).r;
	float z = sg > 1e-8 ? gameNear / sg : 1e9;
	float2 ndc = float2(pix.x / size.x * 2.0 - 1.0, 1.0 - pix.y / size.y * 2.0);
	return float3(ndc * tanXY * z, z);
}
// Ambient occlusion on the game's world where Minecraft's blocks are next to it (the shadow a block makes on the road at its foot): the same hemisphere of 3D points as
// SceneAO, but around the game's surface at this pixel, tested against the blocks' depth (see blockDepthTex): a point counts as hidden if a block is in front of it.
// The result is multiplied onto the scene (aoParams.z is the strength).
float4 PSTerrain(VSOut i) : SV_Target
{
	uint gw, gh;
	gameDepthTex.GetDimensions(gw, gh);
	float2 size = float2(gw, gh);
	float tanY = aoParams.w;
	float2 tanXY = float2(tanY * size.x / size.y, tanY);
	float2 pix = i.pos.xy;
	float3 P = GameViewPos(pix, size, tanXY);
	if (P.z > 150.0) { return float4(1, 1, 1, 1); }
	float3 pr = GameViewPos(pix + float2(1, 0), size, tanXY);
	float3 pl = GameViewPos(pix - float2(1, 0), size, tanXY);
	float3 pu = GameViewPos(pix + float2(0, 1), size, tanXY);
	float3 pd = GameViewPos(pix - float2(0, 1), size, tanXY);
	float3 dx = abs(pr.z - P.z) < abs(P.z - pl.z) ? pr - P : P - pl;
	float3 dy = abs(pu.z - P.z) < abs(P.z - pd.z) ? pu - P : P - pd;
	float3 N = cross(dx, dy);
	float len = length(N);
	if (len < 1e-12) { return float4(1, 1, 1, 1); }
	N /= len;
	if (dot(N, P) > 0.0) { N = -N; }
	float radius = aoParams.y;
	float noise = frac(52.9829189 * frac(dot(pix, float2(0.06711056, 0.00583715))));
	float3 up = abs(N.y) < 0.9 ? float3(0, 1, 0) : float3(1, 0, 0);
	float3 T = normalize(cross(up, N));
	float3 B = cross(N, T);
	float bias = 0.02 + 0.004 * P.z;
	float occ = 0.0;
	[unroll] for (int k = 0; k < 16; ++k) {
		float u = (float(k) + 0.5) / 16.0;
		float phi = (float(k) + noise) * 2.39996323;
		float sinT = sqrt(u);
		float cosT = sqrt(1.0 - u);
		float3 dir = T * (cos(phi) * sinT) + B * (sin(phi) * sinT) + N * cosT;
		float sc = radius * (0.25 + 0.75 * frac(float(k) * 0.61803399 + noise));
		float3 S = P + dir * sc;
		if (S.z < 0.05) { continue; }
		float2 ndcS = S.xy / (S.z * tanXY);
		float2 sp = float2((ndcS.x * 0.5 + 0.5) * size.x, (0.5 - ndcS.y * 0.5) * size.y);
		if (sp.x < 0.0 || sp.y < 0.0 || sp.x >= size.x || sp.y >= size.y) { continue; }
		float zb = blockDepthTex.Load(int3(uint2(sp), 0)).r;
		if (zb <= 0.0) { continue; }                     // no block there
		// Only the blocks' front surface is known, so a block is taken to be a metre thick: the point is inside it if it is behind the front surface, but by less than that. (Without the limit
		// every point anywhere behind a block's silhouette counted, which darkened the floor at the back and sides of a block and left the front looking weak.)
		float into = S.z - zb;
		if (into > bias && into < 1.0) {
			occ += saturate(radius / max(abs(P.z - zb), 1e-3));   // a block far in front of this surface doesn't count
		}
	}
	float ao = 1.0 - saturate((occ / 16.0) * 1.6 * abs(aoParams.z));
	return float4(ao, ao, ao, 1.0);
}
// The engine-lit blocks (the G-buffer pass): the same re-aiming as the scene pass, but the block is written into the engine's own G-buffer instead of being coloured and lit here. Layer 1 is the
// albedo, layer 2 the world-space normal stored as n / max(|n|) * 0.5 + 0.5, layer 3 metalness, roughness, material class (85: the ordinary one) and emissive, layer 4 the motion vector; the depth
// is the engine's reverse-Z (0.02 / distance). sceneParams.y is the roughness and .z the exponent applied to the colour (1: as Minecraft has it). Pixels with no block are not written.
struct GbufOut
{
)HLSL"
R"HLSL(	float4 albedo : SV_Target0;
	float4 normal : SV_Target1;
	float4 material : SV_Target2;
	float2 velocity : SV_Target3;
	float depth : SV_Depth;
};
GbufOut PSGbufBlocks(VSOut i)
{
	GbufOut o;
	o.albedo = float4(0.0, 0.0, 0.0, 0.0);
	o.normal = float4(0.5, 0.5, 1.0, 0.0);
	o.material = float4(0.0, 0.0, 0.0, 0.0);
	o.velocity = float2(0.0, 0.0);
	o.depth = 0.0;
	float2 uv = i.uv;
	if (flipY > 0.5) { uv.y = 1.0 - uv.y; }
	float4 w;
	float zm = 1e9;
	float2 mcUv = uv;
	uint mw, mh;
	mcDepthTex.GetDimensions(mw, mh);
	if (warpT.w > 0.5) {
		float2 ndc = float2(i.uv.x * 2.0 - 1.0, 1.0 - i.uv.y * 2.0);
		float2 nm;
		float zHit;
		bool ok = FindSource(ndc, mw, mh, nm, zHit);
		float2 tuv = float2(nm.x * 0.5 + 0.5, 0.5 - nm.y * 0.5);
		float2 tex2 = flipY > 0.5 ? float2(tuv.x, 1.0 - tuv.y) : tuv;
		w = ok ? tex.Sample(samp, tex2) : float4(0, 0, 0, 0);
		mcUv = tex2;
		zm = ok ? zHit : 1e9;
	} else {
		w = tex.Sample(samp, uv);
		if (w.a > 0.0) {
			float s = mcDepthTex.Load(int3(min(uint2(uv * float2(mw, mh)), uint2(mw - 1, mh - 1)), 0)).r;
			float ndc = zeroToOne > 0.5 ? s : s * 2.0 - 1.0;
			float den = ndc + mcA;
			zm = abs(den) > 1e-9 ? mcB / den : 1e9;
			if (zm <= 0.0) { zm = 1e9; }
		}
	}
	if (w.a < 0.5 || zm > 1e6) { discard; }
	// The colour without Minecraft's face shading (the engine lights it itself).
	float3 c = saturate(w.rgb / max(w.a, 0.001) / max(McShade(mcUv), 0.5));
	o.albedo = float4(pow(c, max(sceneParams.z, 0.1)), 0.0);
	// The face's normal: from the picture's depth, turned into Minecraft's world, snapped to the nearest axis (the blocks are cubes), then into the game's world (x, y, z) = (x, -z, y) of Minecraft's.
	float3 nc = McNormalWithin(mcUv, 400.0);
	float3 snapped = float3(0.0, 1.0, 0.0);
	if (shadeX.w > 0.5 && dot(nc, nc) > 0.5) {
		float3 nw = float3(dot(shadeX.xyz, nc), dot(shadeY.xyz, nc), dot(shadeZ.xyz, nc));
		float3 a3 = abs(nw);
		snapped = (a3.x >= a3.y && a3.x >= a3.z) ? float3(nw.x >= 0.0 ? 1.0 : -1.0, 0.0, 0.0) : (a3.y >= a3.z) ? float3(0.0, nw.y >= 0.0 ? 1.0 : -1.0, 0.0) : float3(0.0, 0.0, nw.z >= 0.0 ? 1.0 : -1.0);
	}
	float3 ng = float3(snapped.x, -snapped.z, snapped.y);
	o.normal = float4(ng * 0.5 + 0.5, 0.0);
	// A light-emitting block (Minecraft's light level of the block this pixel shows, from the grid it publishes) writes its level / 15, times a scale, as the emissive value: layer 3's alpha is what the
	// engine takes as self-illumination (it read 0.57 on some lit road paint), so the engine's own bloom, exposure and fog then treat it as a light source. No grid or an unknown pixel: none.
	float emit = EmissionAt(mcUv);
	float glow = emit > 0.0 ? saturate(emit * sceneParams.x) : 0.0;
	o.material = float4(0.0, saturate(sceneParams.y), 85.0 / 255.0, glow);
	float2 ndcP = float2(i.uv.x * 2.0 - 1.0, 1.0 - i.uv.y * 2.0);
	// An exact depth for a face that lies on a block boundary (every cube's does, at a whole number in Minecraft's world): the search's depth is good to about a centimetre and jitters from
	// pixel to pixel when the camera moves, which the engine's shadow lookup turns into dark diagonal lines on the faces the sun skims (shadow acne). So: where the point the search found is within
	// 3 cm of a whole number along the face's axis, intersect this pixel's own line of sight with that plane. (A slab's top, a torch and the like are not on a boundary: they keep the search's depth.)
	// uiXform.xyz is the camera's place relative to the emission grid's corner (a whole number of blocks: its fractional part is the camera's place within its block); sceneParams.w: 1 = on.
	if (sceneParams.w > 0.5 && warpT.w > 0.5 && shadeX.w > 0.5 && dot(nc, nc) > 0.5) {
		float3 dc = float3(ndcP * tans.xy, 1.0);
		float3 dir = warpR0.xyz * dc.x + warpR1.xyz * dc.y + warpR2.xyz * dc.z;
		float3 ctr = -(warpR0.xyz * warpT.x + warpR1.xyz * warpT.y + warpR2.xyz * warpT.z);
		float3 dirW = float3(dot(shadeX.xyz, dir), dot(shadeY.xyz, dir), dot(shadeZ.xyz, dir));
		float3 ctrW = float3(dot(shadeX.xyz, ctr), dot(shadeY.xyz, ctr), dot(shadeZ.xyz, ctr));
		float3 hitW = ctrW + zm * dirW;
		float3 camFrac = frac(uiXform.xyz);
		int axis = abs(snapped.x) > 0.5 ? 0 : (abs(snapped.y) > 0.5 ? 1 : 2);
		float coord = camFrac[axis] + hitW[axis];
		float plane = round(coord);
		if (abs(coord - plane) < 0.03 && abs(dirW[axis]) > 1e-4) {
			float t = (plane - camFrac[axis] - ctrW[axis]) / dirW[axis];
			if (abs(t - zm) < 0.05) { zm = t; }
		}
	}
	// The motion vector a static surface here would have: where this point was in the previous frame's camera, minus where it is now, in -1 to 1 with y down (what the engine stores: the log
	// showed its vectors to be exactly this, in those units). The previous camera's place relative to the current one comes as a matrix and a vector in constants this pass has no other use for:
	// aoParams.xyz, aoParams.w with fogParams.xy, and fogParams.z with cursor, are its three rows; biasAbs, biasRel and debugView its translation; uiMode the scale and haveUi 1 = valid.
	if (haveUi > 0.5 && uiMode > 0.0 && warpT.w > 0.5) {
		float3x3 Rp = float3x3(aoParams.x, aoParams.y, aoParams.z, aoParams.w, fogParams.x, fogParams.y, fogParams.z, cursor.x, cursor.y);
		float3 Pc = float3(ndcP.x * tans.x * zm, ndcP.y * tans.y * zm, zm);
		float3 Pp = mul(Rp, Pc) + float3(biasAbs, biasRel, debugView);
		if (Pp.z > 0.01) {
			float2 ndcPrev = float2(Pp.x / (Pp.z * tans.x), Pp.y / (Pp.z * tans.y));
			float2 d = ndcPrev - ndcP;
			o.velocity = float2(d.x, -d.y) * uiMode;
		}
	}
	o.depth = saturate(0.02 / zm);
	return o;
}

// Measuring the city's brightness, pass one: this pixel of a 16 x 9 target is the average log brightness of its share of the scene, from 8 x 8 samples.
float4 PSMeasure1(VSOut i) : SV_Target
{
	uint sw, sh;
	sceneTex.GetDimensions(sw, sh);
	float2 size = float2(sw, sh);
	float2 cell = size / float2(16.0, 9.0);
	float2 origin = floor(i.pos.xy) * cell;
	float acc = 0.0;
	[unroll] for (int y = 0; y < 8; ++y) {
		[unroll] for (int x = 0; x < 8; ++x) {
			float2 p = origin + (float2(x, y) + 0.5) / 8.0 * cell;
			float3 c = sceneTex.Load(int3(min(uint2(p), uint2(sw - 1, sh - 1)), 0)).rgb;
			float lum = dot(c, float3(0.2126, 0.7152, 0.0722));
			acc += log(clamp(lum, 1e-4, 8.0));
		}
	}
	return float4(acc / 64.0, 0.0, 0.0, 1.0);
}
// Pass two (a 1 x 1 target): the average of the 144 cells, smoothed over time with last frame's, and what that means for the blocks' gain. Its settings come in the same constants as
// everything else: sceneParams: x 1 on the first frame, y the seconds since the last frame, z the time the smoothing takes, w 1 to take the brightness now as the reference;
// fogParams: x the power, y and z the smallest and largest multiplier; aoParams: x the reference brightness to start from, z the strength (0 to 1).
float4 PSMeasure2(VSOut i) : SV_Target
{
	float acc = 0.0;
	[unroll] for (int y = 0; y < 9; ++y) {
		[unroll] for (int x = 0; x < 16; ++x) {
			acc += smallTex.Load(int3(x, y, 0)).r;
		}
	}
	float cur = acc / 144.0;
	float4 prev = adaptPrevTex.Load(int3(0, 0, 0));
	bool first = sceneParams.x > 0.5;
	float k = 1.0 - exp(-sceneParams.y / max(sceneParams.z, 0.05));
	float smoothed = first ? cur : lerp(prev.r, cur, k);
	float refLog = first ? log(max(aoParams.x, 1e-4)) : prev.b;
	if (sceneParams.w > 0.5) { refLog = smoothed; }
	float m = clamp(exp((smoothed - refLog) * fogParams.x), fogParams.y, fogParams.z);
	m = lerp(1.0, m, saturate(aoParams.z));
	return float4(smoothed, m, refLog, 1.0);
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
			ComPtr<ID3D12PipelineState> psoScene;  // the same shaders, drawing into the game's HDR scene (R16G16B16A16_FLOAT)
			DXGI_FORMAT psoFormat = DXGI_FORMAT_UNKNOWN;
			ComPtr<ID3DBlob> vs, ps;
			ComPtr<ID3D12DescriptorHeap> rtvHeap;
			ComPtr<ID3D12DescriptorHeap> sceneRtvHeap;  // one render target view: for the block depth
			ComPtr<ID3DBlob> psTerrain;
			ComPtr<ID3DBlob> psMeasure1, psMeasure2;
			ComPtr<ID3D12PipelineState> psoMeasure1, psoMeasure2;
			ComPtr<ID3D12Resource> blockDepth;           // the nearest block's distance at each place on the screen (R32_FLOAT, screen sized), made by the scene's first pass
			UINT blockDepthW = 0, blockDepthH = 0;
			ComPtr<ID3D12PipelineState> psoDepth, psoTerrain, psoTerrainView;
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
			std::uint64_t cameraFrame = 0;  // the camera the newest frame was drawn through (Link's frame counter)
			float mcA = 0.0f, mcB = 0.0f;
			bool zeroToOne = true;
			UINT srvInc = 0;
			// The game's depth copy, as last bound to descriptor 3.
			ID3D12Resource* boundGameDepth = nullptr;
			UINT boundGameW = 0, boundGameH = 0;
			ID3D12Resource* boundUi = nullptr;
			ID3D12Resource* boundFog = nullptr;  // the 3D fog texture the seventh descriptor points at
			UINT boundUiW = 0, boundUiH = 0;
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
			srv.NumDescriptors = 16;
			srv.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
			hr = g.device->CreateDescriptorHeap(&srv, IID_PPV_ARGS(&g.srvHeap));
			if (FAILED(hr)) {
				LogError("creating the texture heap", hr);
				return false;
			}
			g.srvInc = g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			{
				// All eight descriptors must be valid for the shader, even the ones a frame doesn't use: make them null ones.
				D3D12_SHADER_RESOURCE_VIEW_DESC nullView{};
				nullView.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
				nullView.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
				nullView.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
				nullView.Texture2D.MipLevels = 1;
				for (UINT i = 0; i < 6; ++i) {
					D3D12_CPU_DESCRIPTOR_HANDLE h = g.srvHeap->GetCPUDescriptorHandleForHeapStart();
					h.ptr += SIZE_T(i) * g.srvInc;
					g.device->CreateShaderResourceView(nullptr, &nullView, h);
				}
				{
					// The seventh and eighth are for 3D textures.
					D3D12_SHADER_RESOURCE_VIEW_DESC null3d{};
					null3d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
					null3d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
					null3d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
					null3d.Texture3D.MostDetailedMip = 0;
					null3d.Texture3D.MipLevels = 1;
					for (UINT i = 6; i < 8; ++i) {
						D3D12_CPU_DESCRIPTOR_HANDLE h = g.srvHeap->GetCPUDescriptorHandleForHeapStart();
						h.ptr += SIZE_T(i) * g.srvInc;
						g.device->CreateShaderResourceView(nullptr, &null3d, h);
					}
					for (UINT i = 8; i < 16; ++i) {
						D3D12_CPU_DESCRIPTOR_HANDLE h = g.srvHeap->GetCPUDescriptorHandleForHeapStart();
						h.ptr += SIZE_T(i) * g.srvInc;
						g.device->CreateShaderResourceView(nullptr, &nullView, h);   // the measuring passes' textures, then the G-buffer copies
					}
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
			errors.Reset();
			hr = ::D3DCompile(kShaderSource, std::strlen(kShaderSource), nullptr, nullptr, nullptr, "PSTerrain", "ps_5_0", 0, 0, &g.psTerrain, &errors);
			if (FAILED(hr)) {
				LogError("compiling the pixel shader for the terrain", hr);
				if (errors) {
					g_sdk->logger->Error(g_handle, static_cast<const char*>(errors->GetBufferPointer()));
				}
				return false;
			}
			for (int pass = 1; pass <= 2; ++pass) {
				errors.Reset();
				hr = ::D3DCompile(kShaderSource, std::strlen(kShaderSource), nullptr, nullptr, nullptr, pass == 1 ? "PSMeasure1" : "PSMeasure2", "ps_5_0", 0, 0, pass == 1 ? &g.psMeasure1 : &g.psMeasure2, &errors);
				if (FAILED(hr)) {
					LogError("compiling the pixel shader that measures the city's brightness", hr);
					if (errors) {
						g_sdk->logger->Error(g_handle, static_cast<const char*>(errors->GetBufferPointer()));
					}
					return false;
				}
			}

			D3D12_DESCRIPTOR_RANGE range{};
			range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
			range.NumDescriptors = 16;
			range.BaseShaderRegister = 0;
			range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

			D3D12_ROOT_PARAMETER params[2]{};
			params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			params[0].DescriptorTable.NumDescriptorRanges = 1;
			params[0].DescriptorTable.pDescriptorRanges = &range;
			params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
			params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
			params[1].Constants.ShaderRegister = 0;
			params[1].Constants.Num32BitValues = 63;
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

		// The same shaders, drawing into the game's HDR scene texture: premultiplied alpha blending onto R16G16B16A16_FLOAT.
		bool EnsureScenePipeline()
		{
			if (g.psoScene) {
				return true;
			}
			D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
			desc.pRootSignature = g.rootSignature.Get();
			desc.VS = { g.vs->GetBufferPointer(), g.vs->GetBufferSize() };
			desc.PS = { g.ps->GetBufferPointer(), g.ps->GetBufferSize() };
			auto& blend = desc.BlendState.RenderTarget[0];
			blend.BlendEnable = TRUE;
			blend.SrcBlend = D3D12_BLEND_ONE;
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
			desc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
			desc.SampleDesc.Count = 1;
			HRESULT hr = g.device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&g.psoScene));
			if (FAILED(hr)) {
				LogError("creating the pipeline for the game's HDR scene", hr);
				return false;
			}
			// The block depth pass: plain float output, no blending.
			{
				D3D12_GRAPHICS_PIPELINE_STATE_DESC d = desc;
				d.BlendState.RenderTarget[0] = D3D12_RENDER_TARGET_BLEND_DESC{};
				d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
				d.RTVFormats[0] = DXGI_FORMAT_R32_FLOAT;
				hr = g.device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&g.psoDepth));
				if (FAILED(hr)) {
					LogError("creating the pipeline for the block depth", hr);
					return false;
				}
			}
			// The terrain's ambient occlusion: multiplied onto the scene (the colour already there times the result); the scene's alpha is left as it is.
			{
				D3D12_GRAPHICS_PIPELINE_STATE_DESC d = desc;
				d.PS = { g.psTerrain->GetBufferPointer(), g.psTerrain->GetBufferSize() };
				auto& b = d.BlendState.RenderTarget[0];
				b = D3D12_RENDER_TARGET_BLEND_DESC{};
				b.BlendEnable = TRUE;
				b.SrcBlend = D3D12_BLEND_DEST_COLOR;
				b.DestBlend = D3D12_BLEND_ZERO;
				b.BlendOp = D3D12_BLEND_OP_ADD;
				b.SrcBlendAlpha = D3D12_BLEND_ZERO;
				b.DestBlendAlpha = D3D12_BLEND_ONE;
				b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
				b.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
				hr = g.device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&g.psoTerrain));
				if (FAILED(hr)) {
					LogError("creating the pipeline for the terrain's occlusion", hr);
					return false;
				}
				// The same, written over the scene instead (for /ccdebug sceneaoview): the occlusion alone, white for none.
				b = D3D12_RENDER_TARGET_BLEND_DESC{};
				b.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
				hr = g.device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&g.psoTerrainView));
				if (FAILED(hr)) {
					LogError("creating the pipeline for the terrain's occlusion view", hr);
					return false;
				}
			}
			// The two passes that measure the city's brightness: plain output, no blending.
			for (int pass = 1; pass <= 2; ++pass) {
				D3D12_GRAPHICS_PIPELINE_STATE_DESC d = desc;
				d.PS = pass == 1 ? D3D12_SHADER_BYTECODE{ g.psMeasure1->GetBufferPointer(), g.psMeasure1->GetBufferSize() } : D3D12_SHADER_BYTECODE{ g.psMeasure2->GetBufferPointer(), g.psMeasure2->GetBufferSize() };
				d.BlendState.RenderTarget[0] = D3D12_RENDER_TARGET_BLEND_DESC{};
				d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
				d.RTVFormats[0] = pass == 1 ? DXGI_FORMAT_R16_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT;
				hr = g.device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(pass == 1 ? &g.psoMeasure1 : &g.psoMeasure2));
				if (FAILED(hr)) {
					LogError("creating the pipeline that measures the city's brightness", hr);
					return false;
				}
			}
			return true;
		}

		D3D12_HEAP_PROPERTIES HeapProps(D3D12_HEAP_TYPE a_type);  // defined below
		D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* a_resource, D3D12_RESOURCE_STATES a_before, D3D12_RESOURCE_STATES a_after);  // defined below

		// ---- which blocks are light sources: a small 3D texture (one byte per block) filled from what Minecraft publishes ----------------------------------------
		struct EmissionState
		{
			ComPtr<ID3D12Resource> texture;
			ComPtr<ID3D12Resource> upload[kFrames];
			std::uint8_t* mapped[kFrames] = {};
			D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
			bool ready = false;
			bool failed = false;
			bool valid = false;  // the texture holds a grid
			std::uint32_t generation = 0;
			std::int32_t originX = 0, originY = 0, originZ = 0;
			std::vector<std::uint8_t> bytes;
		};
		EmissionState g_emit;

		bool EnsureEmissionTexture()
		{
			if (g_emit.ready) {
				return true;
			}
			if (g_emit.failed) {
				return false;
			}
			const D3D12_HEAP_PROPERTIES heap = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
			D3D12_RESOURCE_DESC desc{};
			desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
			desc.Width = proto::kEmissionSizeX;
			desc.Height = proto::kEmissionSizeY;
			desc.DepthOrArraySize = static_cast<UINT16>(proto::kEmissionSizeZ);
			desc.MipLevels = 1;
			desc.Format = DXGI_FORMAT_R8_UNORM;
			desc.SampleDesc.Count = 1;
			desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
			HRESULT hr = g.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&g_emit.texture));
			if (FAILED(hr)) {
				g_emit.failed = true;
				LogError("creating the light emission texture", hr);
				return false;
			}
			UINT64 total = 0;
			g.device->GetCopyableFootprints(&desc, 0, 1, 0, &g_emit.footprint, nullptr, nullptr, &total);
			const D3D12_HEAP_PROPERTIES uploadHeap = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
			D3D12_RESOURCE_DESC bufferDesc{};
			bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			bufferDesc.Width = total;
			bufferDesc.Height = 1;
			bufferDesc.DepthOrArraySize = 1;
			bufferDesc.MipLevels = 1;
			bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
			bufferDesc.SampleDesc.Count = 1;
			bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			for (UINT i = 0; i < kFrames; ++i) {
				hr = g.device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&g_emit.upload[i]));
				void* mapped = nullptr;
				if (FAILED(hr) || FAILED(g_emit.upload[i]->Map(0, nullptr, &mapped))) {
					g_emit.failed = true;
					LogError("creating the light emission upload buffer", FAILED(hr) ? hr : E_FAIL);
					return false;
				}
				g_emit.mapped[i] = static_cast<std::uint8_t*>(mapped);
			}
			D3D12_SHADER_RESOURCE_VIEW_DESC view{};
			view.Format = DXGI_FORMAT_R8_UNORM;
			view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
			view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			view.Texture3D.MostDetailedMip = 0;
			view.Texture3D.MipLevels = 1;
			D3D12_CPU_DESCRIPTOR_HANDLE h = g.srvHeap->GetCPUDescriptorHandleForHeapStart();
			h.ptr += SIZE_T(7) * g.srvInc;
			g.device->CreateShaderResourceView(g_emit.texture.Get(), &view, h);
			g_emit.ready = true;
			g_sdk->logger->Info(g_handle, "overlay: the light emission texture is ready (which blocks are light sources)");
			return true;
		}

		// If Minecraft has published a new grid of block light levels, copy it into the texture (recorded into this frame's list, after the list's reset).
		void UpdateEmission(Link& a_link, UINT a_slot)
		{
			Link::EmissionSnapshot info;
			if (!a_link.ReadEmission(info, g_emit.generation, nullptr)) {
				return;
			}
			const std::uint32_t sx = proto::kEmissionSizeX, sy = proto::kEmissionSizeY, sz = proto::kEmissionSizeZ;
			if ((info.flags & proto::kEmissionValid) == 0 || info.sizeX != sx || info.sizeY != sy || info.sizeZ != sz) {
				g_emit.valid = false;
				return;
			}
			if (g_emit.valid && info.generation == g_emit.generation) {
				return;
			}
			if (!EnsureEmissionTexture()) {
				return;
			}
			if (!a_link.ReadEmission(info, g_emit.valid ? g_emit.generation : info.generation + 1, &g_emit.bytes) || g_emit.bytes.size() != std::size_t(sx) * sy * sz) {
				return;
			}
			const UINT rowPitch = g_emit.footprint.Footprint.RowPitch;
			std::uint8_t* dst = g_emit.mapped[a_slot];
			for (std::uint32_t z = 0; z < sz; ++z) {
				for (std::uint32_t y = 0; y < sy; ++y) {
					std::memcpy(dst + (std::size_t(z) * sy + y) * rowPitch, g_emit.bytes.data() + (std::size_t(z) * sy + y) * sx, sx);
				}
			}
			auto toCopy = Transition(g_emit.texture.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
			g.list->ResourceBarrier(1, &toCopy);
			D3D12_TEXTURE_COPY_LOCATION dstLoc{};
			dstLoc.pResource = g_emit.texture.Get();
			dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			dstLoc.SubresourceIndex = 0;
			D3D12_TEXTURE_COPY_LOCATION srcLoc{};
			srcLoc.pResource = g_emit.upload[a_slot].Get();
			srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
			srcLoc.PlacedFootprint = g_emit.footprint;
			g.list->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
			auto toRead = Transition(g_emit.texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			g.list->ResourceBarrier(1, &toRead);
			g_emit.generation = info.generation;
			g_emit.originX = info.originX;
			g_emit.originY = info.originY;
			g_emit.originZ = info.originZ;
			if (!g_emit.valid) {
				g_sdk->logger->InfoF(g_handle, "overlay: Minecraft's light emission grid arrived: %u x %u x %u blocks from (%d, %d, %d)", sx, sy, sz, info.originX, info.originY, info.originZ);
			}
			g_emit.valid = true;
		}

		// ---- the city's brightness, measured to scale the blocks' gain ---------------------------------------------------------------------------------------------
		// Two small passes before our drawing, while the game's scene is still readable: a 16 x 9 texture of the scene's log brightness, then a single pixel that averages it, smooths it
		// over time with last frame's, and turns it into the gain multiplier the blocks use (see PSMeasure2). The single pixel (r: smoothed log brightness, g: multiplier, b: reference)
		// is copied to a second texture, which the next frame's pass reads as its history.
		struct AdaptState
		{
			ComPtr<ID3D12Resource> cells, cur, prev;  // (not called small: that is a macro in the Windows headers)
			ComPtr<ID3D12DescriptorHeap> rtvHeap;  // two views: small, cur
			UINT rtvInc = 0;
			ID3D12Resource* boundScene = nullptr;  // the scene texture the view in slot 8 is of
			bool ready = false;
			bool failed = false;
			bool reset = true;                     // the next measurement starts the history again
			bool calibrate = false;                // the next measurement takes the brightness it finds as the reference
			std::chrono::steady_clock::time_point last{};
		};
		AdaptState g_adapt;
		float g_adaptStrength = 0.0f;              // 0: the gain is what /ccdebug scenegain says; 1: it follows the city's brightness fully
		float g_adaptPower = 0.0f;                 // the multiplier is (brightness now / reference) to this power (negative: a lower gain when the city is brighter)
		float g_adaptTau = 1.5f;                   // seconds the smoothing takes
		constexpr float kAdaptStartReference = 0.12f;  // the brightness (linear) taken as the reference before one is set with /ccdebug adaptref

		bool EnsureAdapt()
		{
			if (g_adapt.ready) {
				return true;
			}
			if (g_adapt.failed) {
				return false;
			}
			const D3D12_HEAP_PROPERTIES heap = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
			auto make = [&](UINT a_w, UINT a_h, DXGI_FORMAT a_format, bool a_target, ComPtr<ID3D12Resource>& a_out) {
				D3D12_RESOURCE_DESC d{};
				d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
				d.Width = a_w;
				d.Height = a_h;
				d.DepthOrArraySize = 1;
				d.MipLevels = 1;
				d.Format = a_format;
				d.SampleDesc.Count = 1;
				d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
				d.Flags = a_target ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET : D3D12_RESOURCE_FLAG_NONE;
				D3D12_CLEAR_VALUE clear{};
				clear.Format = a_format;
				return SUCCEEDED(g.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, a_target ? &clear : nullptr, IID_PPV_ARGS(&a_out)));
			};
			if (!make(16, 9, DXGI_FORMAT_R16_FLOAT, true, g_adapt.cells) || !make(1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, true, g_adapt.cur) || !make(1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, false, g_adapt.prev)) {
				g_adapt.failed = true;
				g_sdk->logger->Error(g_handle, "overlay: could not make the textures that measure the city's brightness");
				return false;
			}
			D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
			rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
			rtvDesc.NumDescriptors = 2;
			if (FAILED(g.device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&g_adapt.rtvHeap)))) {
				g_adapt.failed = true;
				return false;
			}
			g_adapt.rtvInc = g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
			D3D12_RENDER_TARGET_VIEW_DESC rv{};
			rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
			rv.Format = DXGI_FORMAT_R16_FLOAT;
			D3D12_CPU_DESCRIPTOR_HANDLE h = g_adapt.rtvHeap->GetCPUDescriptorHandleForHeapStart();
			g.device->CreateRenderTargetView(g_adapt.cells.Get(), &rv, h);
			rv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
			h.ptr += g_adapt.rtvInc;
			g.device->CreateRenderTargetView(g_adapt.cur.Get(), &rv, h);
			auto srv = [&](UINT a_slot, ID3D12Resource* a_resource, DXGI_FORMAT a_format) {
				D3D12_SHADER_RESOURCE_VIEW_DESC v{};
				v.Format = a_format;
				v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
				v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
				v.Texture2D.MipLevels = 1;
				D3D12_CPU_DESCRIPTOR_HANDLE c = g.srvHeap->GetCPUDescriptorHandleForHeapStart();
				c.ptr += SIZE_T(a_slot) * g.srvInc;
				g.device->CreateShaderResourceView(a_resource, &v, c);
			};
			srv(9, g_adapt.cells.Get(), DXGI_FORMAT_R16_FLOAT);
			srv(10, g_adapt.prev.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
			srv(11, g_adapt.cur.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
			g_adapt.ready = true;
			g_adapt.reset = true;
			g_sdk->logger->Info(g_handle, "overlay: the textures that measure the city's brightness are ready");
			return true;
		}

		// ---- the G-buffer viewer and reader ----------------------------------------------------------------------------------------------------------------
		// The G-buffer layers are copied into textures of ours just before the engine's lighting pass reads them (the queue hook inserts a list for that: see InScene.cpp), so they can be
		// shown on the screen as raw colour at Present (/ccdebug gbufview) and single texels logged (/ccdebug gbufread). Nothing about how the game draws is changed.
		constexpr int kGbufLayers = 4;
		constexpr int kGbufPoints = 5;
		constexpr UINT64 kGbufStride = 512;  // a placed footprint's alignment
		struct GbufState
		{
			ComPtr<ID3D12Resource> tex[kGbufLayers];  // copies: waiting in the copy-destination state between uses
			D3D12_RESOURCE_DESC desc[kGbufLayers] = {};
			bool copied[kGbufLayers] = {};
			ComPtr<ID3D12Resource> readback;
			std::uint8_t* readbackData = nullptr;
			int readCountdown = 0;                    // Presents until the GPU has surely finished the copy of the texels
			UINT points[kGbufPoints][2] = {};
			DXGI_FORMAT formats[kGbufLayers] = {};
			int channel = 0;
			float scale = 1.0f;
			bool havePose = false;                    // the camera as it is and as it was a frame before, when a read was recorded (to predict the motion vectors)
			Link::CameraPose poseCur, posePrev;
			double dt = 0.0;
			bool predictPending = false;              // the read has been logged: the prediction follows (it needs the camera's axes, defined further down)
			UINT screenW = 0, screenH = 0;
		};
		GbufState g_gbuf;
		double g_frameDtSec = 1.0 / 60.0;              // how long a frame takes, smoothed (from the Present hook)
		std::atomic<std::int64_t> g_gbufReadNotBeforeMs{ 0 };  // a read asked for with a delay waits until then (steady clock, in milliseconds)
		std::int64_t SteadyMs()
		{
			return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
		}
		std::atomic<int> g_gbufViewLayer{ 0 };       // 0 off, 1 to 4 the layer shown over the whole screen
		std::atomic<bool> g_gbufReadArmed{ false };  // the next snapshot also copies the texels at five points of the screen

		bool EnsureGbufTexture(int a_layer, ID3D12Resource* a_source)
		{
			const D3D12_RESOURCE_DESC src = a_source->GetDesc();
			if (g_gbuf.tex[a_layer]) {
				const D3D12_RESOURCE_DESC& cur = g_gbuf.desc[a_layer];
				if (cur.Width == src.Width && cur.Height == src.Height && cur.Format == src.Format) {
					return true;
				}
				WaitIdle();  // the old copy may be in use by a frame on the GPU
				g_gbuf.tex[a_layer].Reset();
				g_gbuf.copied[a_layer] = false;
			}
			D3D12_RESOURCE_DESC d = src;
			d.Alignment = 0;
			d.Flags = D3D12_RESOURCE_FLAG_NONE;
			const D3D12_HEAP_PROPERTIES heap = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
			if (FAILED(g.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_gbuf.tex[a_layer])))) {
				g_sdk->logger->ErrorF(g_handle, "overlay: could not make the copy of G-buffer layer %d", a_layer + 1);
				return false;
			}
			g_gbuf.desc[a_layer] = d;
			g_gbuf.formats[a_layer] = d.Format;
			D3D12_SHADER_RESOURCE_VIEW_DESC v{};
			v.Format = d.Format;
			v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			v.Texture2D.MipLevels = 1;
			D3D12_CPU_DESCRIPTOR_HANDLE c = g.srvHeap->GetCPUDescriptorHandleForHeapStart();
			c.ptr += SIZE_T(12 + a_layer) * g.srvInc;
			g.device->CreateShaderResourceView(g_gbuf.tex[a_layer].Get(), &v, c);
			g_sdk->logger->InfoF(g_handle, "overlay: G-buffer layer %d is %llu x %u, format %d", a_layer + 1, static_cast<unsigned long long>(d.Width), d.Height, static_cast<int>(d.Format));
			return true;
		}

		bool EnsureGbufReadback()
		{
			if (g_gbuf.readback && g_gbuf.readbackData) {
				return true;
			}
			const D3D12_HEAP_PROPERTIES heap = HeapProps(D3D12_HEAP_TYPE_READBACK);
			D3D12_RESOURCE_DESC d{};
			d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			d.Width = 32 * kGbufStride;
			d.Height = 1;
			d.DepthOrArraySize = 1;
			d.MipLevels = 1;
			d.Format = DXGI_FORMAT_UNKNOWN;
			d.SampleDesc.Count = 1;
			d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			if (FAILED(g.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_gbuf.readback)))) {
				return false;
			}
			void* mapped = nullptr;
			if (FAILED(g_gbuf.readback->Map(0, nullptr, &mapped)) || !mapped) {
				g_gbuf.readback.Reset();
				return false;
			}
			g_gbuf.readbackData = static_cast<std::uint8_t*>(mapped);
			return true;
		}

		float HalfToFloat(std::uint16_t h)
		{
			const std::uint32_t sign = (h >> 15) & 1, exponent = (h >> 10) & 0x1F, mantissa = h & 0x3FF;
			float value;
			if (exponent == 0) {
				value = std::ldexp(static_cast<float>(mantissa), -24);
			} else if (exponent == 31) {
				value = mantissa ? NAN : INFINITY;
			} else {
				value = std::ldexp(static_cast<float>(mantissa | 0x400), static_cast<int>(exponent) - 25);
			}
			return sign ? -value : value;
		}

		void DescribeTexel(const std::uint8_t* a_data, DXGI_FORMAT a_format, char* a_out, std::size_t a_size)
		{
			if (a_format == DXGI_FORMAT_R10G10B10A2_UNORM) {
				std::uint32_t v;
				std::memcpy(&v, a_data, 4);
				const std::uint32_t r = v & 1023, gg = (v >> 10) & 1023, b = (v >> 20) & 1023, aa = v >> 30;
				std::snprintf(a_out, a_size, "R10G10B10A2 r=%u g=%u b=%u a=%u  (%.3f %.3f %.3f)", r, gg, b, aa, r / 1023.0f, gg / 1023.0f, b / 1023.0f);
			} else if (a_format == DXGI_FORMAT_R8G8B8A8_UNORM) {
				std::snprintf(a_out, a_size, "R8G8B8A8 r=%u g=%u b=%u a=%u  (%.3f %.3f %.3f %.3f)", a_data[0], a_data[1], a_data[2], a_data[3], a_data[0] / 255.0f, a_data[1] / 255.0f, a_data[2] / 255.0f, a_data[3] / 255.0f);
			} else if (a_format == DXGI_FORMAT_R16G16_FLOAT) {
				std::uint16_t h[2];
				std::memcpy(h, a_data, 4);
				std::snprintf(a_out, a_size, "R16G16_FLOAT x=%.5f y=%.5f", HalfToFloat(h[0]), HalfToFloat(h[1]));
			} else {
				std::snprintf(a_out, a_size, "format %d bytes %02X %02X %02X %02X", static_cast<int>(a_format), a_data[0], a_data[1], a_data[2], a_data[3]);
			}
		}

		// A moment after the snapshot that asked for texels was recorded, the GPU has finished with it: write what it read.
		void PollGbufRead()
		{
			if (g_gbuf.readCountdown <= 0 || --g_gbuf.readCountdown > 0 || !g_gbuf.readbackData) {
				return;
			}
			const char* names[kGbufPoints] = { "the centre of the screen", "160 pixels left of the centre", "160 pixels right of the centre", "160 pixels above the centre", "160 pixels below the centre" };
			g_sdk->logger->Info(g_handle, "overlay: G-BUFFER READ: raw values at five points (layers 1 to 4 are the G-buffer layers in the order the engine writes them, the depth is the main depth texture's, distance = 0.02 / depth)");
			for (int p = 0; p < kGbufPoints; ++p) {
				g_sdk->logger->InfoF(g_handle, "overlay: G-BUFFER READ: %s (pixel %u, %u):", names[p], g_gbuf.points[p][0], g_gbuf.points[p][1]);
				for (int L = 0; L < kGbufLayers; ++L) {
					if (!g_gbuf.tex[L]) {
						continue;
					}
					char text[160];
					DescribeTexel(g_gbuf.readbackData + (UINT64(L) * kGbufPoints + p) * kGbufStride, g_gbuf.formats[L], text, sizeof(text));
					g_sdk->logger->InfoF(g_handle, "overlay: G-BUFFER READ:     layer %d: %s", L + 1, text);
				}
				float depthValue = 0.0f;
				std::memcpy(&depthValue, g_gbuf.readbackData + (20 + UINT64(p)) * kGbufStride, 4);
				const unsigned stencil = g_gbuf.readbackData[(25 + UINT64(p)) * kGbufStride];
				if (depthValue > 1e-9f) {
					g_sdk->logger->InfoF(g_handle, "overlay: G-BUFFER READ:     depth %.7f (distance %.3f m), stencil %u (0x%02X)", depthValue, 0.02f / depthValue, stencil, stencil);
				} else {
					g_sdk->logger->InfoF(g_handle, "overlay: G-BUFFER READ:     depth %.7f (nothing: the sky), stencil %u (0x%02X)", depthValue, stencil, stencil);
				}
			}
			g_gbuf.predictPending = true;
		}

		// ---- the engine-lit blocks ---------------------------------------------------------------------------------------------------------------------------
		// The blocks are written into the G-buffer (the four layers and the main depth with its stencil) at a moment between the engine's geometry and its decals, so that the decals, the ambient
		// occlusion, the reflections and the lighting all see them (see InScene.cpp for where). A pixel of the pass is a block pixel found by the same re-aiming as the scene pass.
		struct GbufBlocksGpu
		{
			ComPtr<ID3DBlob> ps;
			ComPtr<ID3D12PipelineState> pso;
			ComPtr<ID3D12DescriptorHeap> rtvHeap;  // the four layers
			ComPtr<ID3D12DescriptorHeap> dsvHeap;  // the main depth
			UINT rtvInc = 0;
			bool failed = false;
		};
		GbufBlocksGpu g_gbb;
		std::atomic<bool> g_gbufBlocksOn{ true };    // the default: the blocks are written into the G-buffer (and drawn into the scene if that does not happen in a frame)
		std::atomic<std::uint64_t> g_gbufBlocksDrawn{ 0 };
		bool g_gbufExact = true;    // the exact depth for faces on a block boundary (see PSGbufBlocks)
		float g_gbufEmit = 1.0f;    // the emissive value written for a light-emitting block is its light level / 15 times this (0: none)
		float g_gbufVelScale = 0.0f;  // the motion vector written: 0 none, 1 the camera's own motion as the engine stores it (previous minus current, -1 to 1, y down)
		float g_gbufRough = 0.55f;  // the roughness written for every block (the street reads 0.5 to 0.6, a wall 0.43)
		float g_gbufGamma = 1.0f;   // the exponent applied to Minecraft's colours before they are written as albedo (1: as they are; 2.2 if the engine keeps its albedo linear)

		bool EnsureGbufBlocksPipeline()
		{
			if (g_gbb.pso) {
				return true;
			}
			if (g_gbb.failed) {
				return false;
			}
			ComPtr<ID3DBlob> errors;
			HRESULT hr = ::D3DCompile(kShaderSource, std::strlen(kShaderSource), nullptr, nullptr, nullptr, "PSGbufBlocks", "ps_5_0", 0, 0, &g_gbb.ps, &errors);
			if (FAILED(hr)) {
				g_gbb.failed = true;
				LogError("compiling the pixel shader that writes the blocks into the G-buffer", hr);
				if (errors) {
					g_sdk->logger->Error(g_handle, static_cast<const char*>(errors->GetBufferPointer()));
				}
				return false;
			}
			D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
			d.pRootSignature = g.rootSignature.Get();
			d.VS = { g.vs->GetBufferPointer(), g.vs->GetBufferSize() };
			d.PS = { g_gbb.ps->GetBufferPointer(), g_gbb.ps->GetBufferSize() };
			for (int k = 0; k < 4; ++k) {
				d.BlendState.RenderTarget[k].BlendEnable = FALSE;
				d.BlendState.RenderTarget[k].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
			}
			d.SampleMask = UINT_MAX;
			d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
			d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
			d.RasterizerState.DepthClipEnable = TRUE;
			// The engine's depth is reverse-Z (0.02 / distance: larger is nearer), so a block wins where its depth is at least the game's. The stencil is written as 0, like the buildings.
			d.DepthStencilState.DepthEnable = TRUE;
			d.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
			d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
			d.DepthStencilState.StencilEnable = TRUE;
			d.DepthStencilState.StencilReadMask = 0xFF;
			d.DepthStencilState.StencilWriteMask = 0xFF;
			D3D12_DEPTH_STENCILOP_DESC op{};
			op.StencilFailOp = D3D12_STENCIL_OP_KEEP;
			op.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
			op.StencilPassOp = D3D12_STENCIL_OP_REPLACE;
			op.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
			d.DepthStencilState.FrontFace = op;
			d.DepthStencilState.BackFace = op;
			d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			d.NumRenderTargets = 4;
			d.RTVFormats[0] = DXGI_FORMAT_R10G10B10A2_UNORM;
			d.RTVFormats[1] = DXGI_FORMAT_R10G10B10A2_UNORM;
			d.RTVFormats[2] = DXGI_FORMAT_R8G8B8A8_UNORM;
			d.RTVFormats[3] = DXGI_FORMAT_R16G16_FLOAT;
			d.DSVFormat = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
			d.SampleDesc.Count = 1;
			hr = g.device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&g_gbb.pso));
			if (FAILED(hr)) {
				g_gbb.failed = true;
				LogError("creating the pipeline that writes the blocks into the G-buffer", hr);
				return false;
			}
			D3D12_DESCRIPTOR_HEAP_DESC rtv{};
			rtv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
			rtv.NumDescriptors = 4;
			D3D12_DESCRIPTOR_HEAP_DESC dsv{};
			dsv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
			dsv.NumDescriptors = 1;
			if (FAILED(g.device->CreateDescriptorHeap(&rtv, IID_PPV_ARGS(&g_gbb.rtvHeap))) || FAILED(g.device->CreateDescriptorHeap(&dsv, IID_PPV_ARGS(&g_gbb.dsvHeap)))) {
				g_gbb.failed = true;
				g_gbb.pso.Reset();
				return false;
			}
			g_gbb.rtvInc = g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
			g_sdk->logger->Info(g_handle, "overlay: the pipeline that writes the blocks into the G-buffer is ready");
			return true;
		}

		// The block depth texture (screen sized) and its views, made again if the screen's size changes.
		bool EnsureBlockDepth(UINT a_w, UINT a_h)
		{
			if (g.blockDepth && g.blockDepthW == a_w && g.blockDepthH == a_h) {
				return true;
			}
			WaitIdle();
			g.blockDepth.Reset();
			D3D12_HEAP_PROPERTIES heap{};
			heap.Type = D3D12_HEAP_TYPE_DEFAULT;
			heap.CreationNodeMask = 1;
			heap.VisibleNodeMask = 1;
			D3D12_RESOURCE_DESC desc{};
			desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			desc.Width = a_w;
			desc.Height = a_h;
			desc.DepthOrArraySize = 1;
			desc.MipLevels = 1;
			desc.Format = DXGI_FORMAT_R32_FLOAT;
			desc.SampleDesc.Count = 1;
			desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
			desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
			D3D12_CLEAR_VALUE clear{};
			clear.Format = DXGI_FORMAT_R32_FLOAT;
			HRESULT hr = g.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear, IID_PPV_ARGS(&g.blockDepth));
			if (FAILED(hr)) {
				LogError("creating the block depth texture", hr);
				return false;
			}
			if (!g.sceneRtvHeap) {
				D3D12_DESCRIPTOR_HEAP_DESC rtv{};
				rtv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
				rtv.NumDescriptors = 1;
				hr = g.device->CreateDescriptorHeap(&rtv, IID_PPV_ARGS(&g.sceneRtvHeap));
				if (FAILED(hr)) {
					LogError("creating the block depth render target heap", hr);
					return false;
				}
			}
			D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
			rtvDesc.Format = DXGI_FORMAT_R32_FLOAT;
			rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
			g.device->CreateRenderTargetView(g.blockDepth.Get(), &rtvDesc, g.sceneRtvHeap->GetCPUDescriptorHandleForHeapStart());
			D3D12_SHADER_RESOURCE_VIEW_DESC view{};
			view.Format = DXGI_FORMAT_R32_FLOAT;
			view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			view.Texture2D.MipLevels = 1;
			D3D12_CPU_DESCRIPTOR_HANDLE h = g.srvHeap->GetCPUDescriptorHandleForHeapStart();
			h.ptr += SIZE_T(5) * g.srvInc;
			g.device->CreateShaderResourceView(g.blockDepth.Get(), &view, h);
			g.blockDepthW = a_w;
			g.blockDepthH = a_h;
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

		// A camera's axes in Minecraft's world: right, up and forward, from its yaw, pitch and roll (degrees; yaw 0 faces south, positive pitch looks down).
		struct Axes
		{
			double v[3][3];  // v[0] right, v[1] up, v[2] forward
		};

		Axes AxesOf(const Link::CameraPose& a_pose)
		{
			constexpr double kRad = 3.14159265358979323846 / 180.0;
			const double yaw = a_pose.yaw * kRad;
			const double pitch = a_pose.pitch * kRad;
			const double roll = a_pose.roll * kRad;
			const double sy = std::sin(yaw), cy = std::cos(yaw), sp = std::sin(pitch), cp = std::cos(pitch);
			const double f[3] = { -sy * cp, -sp, cy * cp };
			const double r0[3] = { -cy, 0.0, -sy };
			const double u0[3] = { r0[1] * f[2] - r0[2] * f[1], r0[2] * f[0] - r0[0] * f[2], r0[0] * f[1] - r0[1] * f[0] };
			Axes axes{};
			for (int i = 0; i < 3; ++i) {
				axes.v[0][i] = r0[i] * std::cos(roll) + u0[i] * std::sin(roll);
				axes.v[1][i] = u0[i] * std::cos(roll) - r0[i] * std::sin(roll);
				axes.v[2][i] = f[i];
			}
			return axes;
		}

		double Dot3(const double* a, const double* b)
		{
			return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
		}

		// Fills the shader's warp constants (20 floats) for re-aiming a picture drawn through a_mc at the camera a_now. Returns false if the
		// two cameras are too far apart for that to make sense (a teleport, a cut): the picture is then drawn as it is.
		bool BuildWarp(const Link::CameraPose& a_mc, const Link::CameraPose& a_now, float* a_out)
		{
			const double dx = a_mc.x - a_now.x, dy = a_mc.y - a_now.y, dz = a_mc.z - a_now.z;
			if (dx * dx + dy * dy + dz * dz > 16.0) {
				return false;
			}
			const Axes m = AxesOf(a_mc);
			const Axes c = AxesOf(a_now);
			if (Dot3(m.v[2], c.v[2]) < 0.9) {
				return false;  // looking more than about 25 degrees apart
			}
			const double delta[3] = { dx, dy, dz };
			constexpr double kRad = 3.14159265358979323846 / 180.0;
			for (int i = 0; i < 3; ++i) {
				// Row i of the matrix from Minecraft's camera space to the current camera space: dot(current axis i, Minecraft axis j).
				for (int j = 0; j < 3; ++j) {
					a_out[i * 4 + j] = static_cast<float>(Dot3(c.v[i], m.v[j]));
				}
				a_out[i * 4 + 3] = 0.0f;
				// Where Minecraft's camera is, in the current camera's space.
				a_out[12 + i] = static_cast<float>(Dot3(c.v[i], delta));
			}
			a_out[15] = 1.0f;
			const double tanHalfC = std::tan(a_now.vfov * 0.5 * kRad);
			const double tanHalfM = std::tan(a_mc.vfov * 0.5 * kRad);
			a_out[16] = static_cast<float>(tanHalfC * a_now.aspect);
			a_out[17] = static_cast<float>(tanHalfC);
			a_out[18] = static_cast<float>(tanHalfM * a_mc.aspect);
			a_out[19] = static_cast<float>(tanHalfM);
			return true;
		}

		// A pose a fraction of the way from a towards b (angles the short way round).
		Link::CameraPose LerpPose(const Link::CameraPose& a_a, const Link::CameraPose& a_b, double a_t)
		{
			Link::CameraPose r = a_a;
			r.x = a_a.x + (a_b.x - a_a.x) * a_t;
			r.y = a_a.y + (a_b.y - a_a.y) * a_t;
			r.z = a_a.z + (a_b.z - a_a.z) * a_t;
			auto angle = [a_t](float a_from, float a_to) {
				const float d = std::fmod(a_to - a_from + 540.0f, 360.0f) - 180.0f;
				return a_from + d * static_cast<float>(a_t);
			};
			r.yaw = angle(a_a.yaw, a_b.yaw);
			r.pitch = angle(a_a.pitch, a_b.pitch);
			r.roll = angle(a_a.roll, a_b.roll);
			return r;
		}

		void NoteFrameTime()
		{
			static auto last = std::chrono::steady_clock::time_point{};
			const auto t = std::chrono::steady_clock::now();
			if (last.time_since_epoch().count() != 0) {
				const double dt = std::chrono::duration<double>(t - last).count();
				if (dt > 0.001 && dt < 0.2) {
					g_frameDtSec += (dt - g_frameDtSec) * 0.05;
				}
			}
			last = t;
		}

		// After a read has been logged: for each point of it, the motion vector that the camera's own motion since the frame before would give, next to what the engine wrote, so that the engine's units
		// and sign can be read off the ratio (for static geometry the vector is only the camera's motion). The point's place and depth come from the read; the camera now and a frame ago from the plugin's history.
		void LogGbufPrediction()
		{
			if (!g_gbuf.predictPending || !g_gbuf.readbackData) {
				return;
			}
			g_gbuf.predictPending = false;
			if (!g_gbuf.havePose || g_gbuf.screenW == 0 || g_gbuf.screenH == 0) {
				g_sdk->logger->Info(g_handle, "overlay: G-BUFFER READ: no camera history, so no predicted motion vectors");
				return;
			}
			const Link::CameraPose& cur = g_gbuf.poseCur;
			const Link::CameraPose& prev = g_gbuf.posePrev;
			const Axes ac = AxesOf(cur);
			const Axes ap = AxesOf(prev);
			const double cx = cur.x - prev.x, cy = cur.y - prev.y, cz = cur.z - prev.z;
			const double moved = std::sqrt(cx * cx + cy * cy + cz * cz);
			g_sdk->logger->InfoF(g_handle, "overlay: G-BUFFER READ: the camera moved %.2f cm since the frame before (a frame takes %.2f ms: %.2f m/s). For each point: the engine's motion vector, then the one predicted from that movement as a fraction of the screen (0 to 1 across, y down) and as -1 to 1 (y up), then engine / predicted",
				moved * 100.0, g_gbuf.dt * 1000.0, g_gbuf.dt > 0.0 ? moved / g_gbuf.dt : 0.0);
			constexpr double kRad = 3.14159265358979323846 / 180.0;
			const double tanY = std::tan(cur.vfov * 0.5 * kRad);
			const double tanX = tanY * (double(g_gbuf.screenW) / double(g_gbuf.screenH));
			for (int p = 0; p < kGbufPoints; ++p) {
				float depthValue = 0.0f;
				std::memcpy(&depthValue, g_gbuf.readbackData + (20 + UINT64(p)) * kGbufStride, 4);
				if (depthValue <= 1e-9f) {
					continue;  // the sky
				}
				const double z = 0.02 / depthValue;
				const double nx = ((g_gbuf.points[p][0] + 0.5) / g_gbuf.screenW) * 2.0 - 1.0;
				const double ny = 1.0 - ((g_gbuf.points[p][1] + 0.5) / g_gbuf.screenH) * 2.0;
				const double xc = nx * tanX * z, yc = ny * tanY * z;
				// the point's offset from the camera now, in the world's axes, and from the camera a frame ago
				const double qx = ac.v[0][0] * xc + ac.v[1][0] * yc + ac.v[2][0] * z + cx;
				const double qy = ac.v[0][1] * xc + ac.v[1][1] * yc + ac.v[2][1] * z + cy;
				const double qz = ac.v[0][2] * xc + ac.v[1][2] * yc + ac.v[2][2] * z + cz;
				const double xp = ap.v[0][0] * qx + ap.v[0][1] * qy + ap.v[0][2] * qz;
				const double yp = ap.v[1][0] * qx + ap.v[1][1] * qy + ap.v[1][2] * qz;
				const double zp = ap.v[2][0] * qx + ap.v[2][1] * qy + ap.v[2][2] * qz;
				if (zp <= 0.01) {
					continue;
				}
				const double dndcX = xp / (zp * tanX) - nx;  // where it was minus where it is, in -1 to 1 (y up)
				const double dndcY = yp / (zp * tanY) - ny;
				const double du = dndcX * 0.5, dv = -dndcY * 0.5;  // the same as a fraction of the screen, y down
				std::uint16_t half[2];
				std::memcpy(half, g_gbuf.readbackData + (3 * UINT64(kGbufPoints) + p) * kGbufStride, 4);
				const double ex = HalfToFloat(half[0]), ey = HalfToFloat(half[1]);
				char ratio[160] = "(too small to compare)";
				if (std::fabs(du) > 2e-6 && std::fabs(dv) > 2e-6) {
					std::snprintf(ratio, sizeof(ratio), "x %.3f, y %.3f against the screen fractions (y down), y %.3f against -1 to 1 (y up)", ex / du, ey / dv, ey / dndcY);
				} else if (std::fabs(du) > 2e-6) {
					std::snprintf(ratio, sizeof(ratio), "x %.3f against the screen fraction", ex / du);
				} else if (std::fabs(dv) > 2e-6) {
					std::snprintf(ratio, sizeof(ratio), "y %.3f against the screen fraction (y down), %.3f against -1 to 1 (y up)", ey / dv, ey / dndcY);
				}
				g_sdk->logger->InfoF(g_handle, "overlay: G-BUFFER READ:     point %d (%.2f m away): engine (%.6f, %.6f); predicted (%.6f, %.6f) as screen fractions, (%.6f, %.6f) as -1 to 1; ratio %s", p, z, ex, ey, du, dv, dndcX, dndcY, ratio);
			}
		}

		void Draw(IDXGISwapChain* a_swapChain)
		{
			std::lock_guard<std::recursive_mutex> lock(g_gpuMutex);
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
			UpdateEmission(link, slot);  // Minecraft's grid of which blocks give light

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
							g.cameraFrame = hdr->cameraFrame;
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
								if (layered) {
									inscene::EnableByDefault();  // the blocks are drawn into the game's own scene unless the player has said otherwise
								}
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

			// The game's interface layer, copied each frame by the depth hook: drawn back over the blocks, so the interface is in front of them.
			depth::SetScreenSize(bb.Width, bb.Height);
			inscene::SetScreenSize(bb.Width, bb.Height);
			depth::SetUiWanted(g_uiMode > 0);
			depth::GameUi gameUi;
			bool useUi = false;
			if (drawHud && g.layered && g_uiMode > 0) {
				if (depth::GetGameUi(gameUi)) {
					if (g.boundUi != gameUi.resource || g.boundUiW != gameUi.width || g.boundUiH != gameUi.height) {
						WaitIdle();  // the descriptor may be in use by a frame still on the GPU
						D3D12_SHADER_RESOURCE_VIEW_DESC view{};
						view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;  // the layer's values as they are stored (sRGB-encoded), blended the way Minecraft's are
						view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
						view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
						view.Texture2D.MipLevels = 1;
						D3D12_CPU_DESCRIPTOR_HANDLE h = g.srvHeap->GetCPUDescriptorHandleForHeapStart();
						h.ptr += SIZE_T(4) * g.srvInc;
						g.device->CreateShaderResourceView(gameUi.resource, &view, h);
						g.boundUi = gameUi.resource;
						g.boundUiW = gameUi.width;
						g.boundUiH = gameUi.height;
						g_sdk->logger->InfoF(g_handle, "overlay: drawing the game's interface layer (%u x %u) over the blocks (mode %d)", gameUi.width, gameUi.height, g_uiMode);
					}
					useUi = true;
					auto toReadUi = Transition(gameUi.resource, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
					g.list->ResourceBarrier(1, &toReadUi);
				} else {
					static auto lastComplaint = std::chrono::steady_clock::time_point{};
					const auto t = std::chrono::steady_clock::now();
					if (t - lastComplaint > std::chrono::seconds(8)) {
						lastComplaint = t;
						g_sdk->logger->Info(g_handle, "overlay: the game's interface layer has not been captured (yet): it needs the depth capture on (blocks drawn) and the HUD showing");
					}
				}
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
				float constants[63] = { g.flipY ? 1.0f : 0.0f, 0.0f, 0.0f, g.layered ? 1.0f : 0.0f, 0.0f, 0.0f, g.zeroToOne ? 1.0f : 0.0f, useGameDepth ? 1.0f : 0.0f,
					g.mcA, g.mcB, 0.02f /* the game's near plane: its depth is 0.02 / distance */, 0.06f, 0.004f, float(g_debugView), float(g_uiMode), useUi ? 1.0f : 0.0f };
				// Re-aim the blocks (the world layer) at the game's camera as it is now, so the time the picture took to arrive isn't seen.
				if (g.layered && g_warpEnabled) {
					Link::CameraPose drawn;
					Link::CameraPose target;
					const auto aim = now - std::chrono::microseconds(static_cast<long long>(g_warpDelayMs * 1000.0f));
					if (link.CameraPoseForFrame(g.cameraFrame, drawn) && link.CameraPoseAt(aim, target) && BuildWarp(drawn, target, &constants[16])) {
						++g_warpDraws;
					} else {
						constants[31] = 0.0f;
					}
				}
				{
					// If the blocks were drawn into the game's scene this frame, only the overlay (hand, hotbar, screens) is drawn here.
					const std::uint64_t drawn = g_sceneDrawn.load();
					constants[42] = (g.layered && drawn != g_sceneSeen) ? 1.0f : 0.0f;
					g_sceneSeen = drawn;
				}
				constants[36] = g_uiScaleX;
				constants[37] = g_uiShiftX;
				constants[38] = g_uiShiftY;
				constants[39] = g_uiScaleY;
				if (input::CursorVisible()) {
					// The cursor lives in the HUD's pixels; the HUD is stretched over the screen.
					float cx = 0.0f;
					float cy = 0.0f;
					input::CursorPosition(cx, cy);
					constants[2] = 1.0f;
					constants[4] = g.texW ? cx * float(bb.Width) / float(g.texW) : cx;
					constants[5] = g.texH ? cy * float(bb.Height) / float(g.texH) : cy;
				}
				g.list->SetGraphicsRoot32BitConstants(1, 63, constants, 0);
				g.list->DrawInstanced(3, 1, 0, 0);
			} else if (badge) {
				++g_badgeDraws;
				D3D12_RECT scissor{ 8, 8, 72, 72 };  // the test square
				g.list->RSSetScissorRects(1, &scissor);
				float constants[63] = {};
				constants[1] = 1.0f;
				g.list->SetGraphicsRoot32BitConstants(1, 63, constants, 0);
				g.list->DrawInstanced(3, 1, 0, 0);
			}

			// The G-buffer viewer: one layer, copied before the lighting, over the whole screen as raw colour (see /ccdebug gbufview). The copies wait in the copy-destination state.
			PollGbufRead();
			LogGbufPrediction();
			{
				const int layer = g_gbufViewLayer.load();
				if (layer >= 1 && layer <= kGbufLayers && g_gbuf.tex[layer - 1] && g_gbuf.copied[layer - 1]) {
					auto toRead = Transition(g_gbuf.tex[layer - 1].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
					g.list->ResourceBarrier(1, &toRead);
					D3D12_RECT scissor{ 0, 0, LONG(bb.Width), LONG(bb.Height) };
					g.list->RSSetScissorRects(1, &scissor);
					float constants[63] = {};
					constants[60] = static_cast<float>(layer);        // fogParams.x
					constants[61] = static_cast<float>(g_gbuf.channel);  // .y
					constants[62] = g_gbuf.scale;                      // .z
					g.list->SetGraphicsRoot32BitConstants(1, 63, constants, 0);
					g.list->DrawInstanced(3, 1, 0, 0);
					auto toDest = Transition(g_gbuf.tex[layer - 1].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
					g.list->ResourceBarrier(1, &toDest);
				}
			}

			if (useUi) {
				auto uiBack = Transition(gameUi.resource, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
				g.list->ResourceBarrier(1, &uiBack);
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
			const bool verbose = log::Verbose();
			if (verbose) g_sdk->logger->InfoF(g_handle,
				"overlay: status: %llu presents, in game=%d, Minecraft linked=%d, frames sent by Minecraft so far=%llu, frames uploaded=%llu, HUD draws=%llu, test-square draws=%llu, GPU ready=%d, failed=%d",
				static_cast<unsigned long long>(g_presents), link.InGame() ? 1 : 0, mcAlive ? 1 : 0,
				static_cast<unsigned long long>(mcAlive ? link.OverlayFramesPublished() : 0), static_cast<unsigned long long>(g_framesUploaded),
				static_cast<unsigned long long>(g_hudDraws), static_cast<unsigned long long>(g_badgeDraws), g.ready ? 1 : 0, g.failed ? 1 : 0);
			if (verbose) g_sdk->logger->InfoF(g_handle, "overlay: draws so far: %llu plain, %llu layered, %llu of those hidden against the game's depth, %llu re-aimed at the current camera (aiming %.0f ms behind the newest)",
				static_cast<unsigned long long>(g_plainDraws), static_cast<unsigned long long>(g_layeredDraws), static_cast<unsigned long long>(g_depthDraws),
				static_cast<unsigned long long>(g_warpDraws), g_warpDelayMs);
			if (g_ageCount > 0 && verbose) {
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
			NoteFrameTime();
			LogStatus();
			cybercraft::depth::Report();
			if (a_swapChain == g_gameSwapChain) {
				cybercraft::uiprobe::OnPresent(a_swapChain);
				cybercraft::inscene::OnPresent();
			}
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
			NoteFrameTime();
			LogStatus();
			if (static_cast<IDXGISwapChain*>(a_swapChain) == g_gameSwapChain) {
				cybercraft::uiprobe::OnPresent(static_cast<IDXGISwapChain*>(a_swapChain));
				cybercraft::inscene::OnPresent();
			}
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

	void SetWarp(bool a_enabled, float a_delayMs)
	{
		g_warpEnabled = a_enabled;
		g_warpDelayMs = std::clamp(a_delayMs, 0.0f, 150.0f);
	}

	bool GbufWanted()
	{
		return g_gbufViewLayer.load() > 0 || (g_gbufReadArmed.load() && SteadyMs() >= g_gbufReadNotBeforeMs.load());
	}

	bool GbufBlocksOn()
	{
		return g_gbufBlocksOn.load();
	}

	std::uint64_t GbufBlocksDrawn()
	{
		return g_gbufBlocksDrawn.load();
	}

	void SetGbufEmit(float a_percent)
	{
		std::lock_guard<std::recursive_mutex> lock(g_gpuMutex);
		g_gbufEmit = std::clamp(a_percent, 0.0f, 400.0f) / 100.0f;
		g_sdk->logger->InfoF(g_handle, "overlay: emissive value written for light-emitting blocks: their light level / 15 times %.2f (0 writes none)", g_gbufEmit);
	}

	void SetGbufExact(bool a_on)
	{
		std::lock_guard<std::recursive_mutex> lock(g_gpuMutex);
		g_gbufExact = a_on;
		g_sdk->logger->InfoF(g_handle, "overlay: exact depth for block faces on a block boundary: %s", a_on ? "on" : "off (the search's depth, good to about a centimetre)");
	}

	void SetGbufVelocity(float a_percent)
	{
		std::lock_guard<std::recursive_mutex> lock(g_gpuMutex);
		g_gbufVelScale = std::clamp(a_percent, 0.0f, 400.0f) / 100.0f;
		g_sdk->logger->InfoF(g_handle, "overlay: motion vector written for the blocks: %s", g_gbufVelScale > 0.0f ? "the camera's own motion as the engine stores it, times the scale" : "none (zero)");
		if (g_gbufVelScale > 0.0f) {
			g_sdk->logger->InfoF(g_handle, "overlay: motion vector scale %.2f (1 is the engine's unit as read from the log: previous minus current, -1 to 1, y down)", g_gbufVelScale);
		}
	}

	void SetGbufBlocks(bool a_on, float a_roughnessPercent, float a_gammaPercent)
	{
		std::lock_guard<std::recursive_mutex> lock(g_gpuMutex);
		g_gbufRough = std::clamp(a_roughnessPercent, 1.0f, 100.0f) / 100.0f;
		g_gbufGamma = std::clamp(a_gammaPercent, 20.0f, 400.0f) / 100.0f;
		g_gbufBlocksOn.store(a_on);
		g_sdk->logger->InfoF(g_handle, "overlay: blocks written into the engine's G-buffer (lit by the engine): %s (roughness %.2f, colour exponent %.2f: 1 keeps Minecraft's colours as they are, 2.2 makes them linear)", a_on ? "on" : "off", g_gbufRough, g_gbufGamma);
		if (a_on) {
			g_sdk->logger->Info(g_handle, "overlay: while this is on the blocks are not drawn into the scene or at Present, only into the G-buffer (needs the scene hook running and the G-buffer found: see the 'found the G-buffer' line)");
		}
	}

	void SetGbufView(int a_layer, int a_channel, float a_scalePercent)
	{
		std::lock_guard<std::recursive_mutex> lock(g_gpuMutex);
		g_gbuf.channel = std::clamp(a_channel, 0, 4);
		g_gbuf.scale = std::clamp(a_scalePercent, 1.0f, 100000.0f) / 100.0f;
		g_gbufViewLayer.store(std::clamp(a_layer, 0, kGbufLayers));
		if (a_layer <= 0) {
			g_sdk->logger->Info(g_handle, "overlay: G-buffer viewer off");
		} else {
			const char* channels[] = { "all three colours", "channel r", "channel g", "channel b", "channel a" };
			g_sdk->logger->InfoF(g_handle, "overlay: G-buffer viewer: layer %d, %s, scale %.2f (the layers are copied when the engine's lighting is about to read them)", a_layer, channels[g_gbuf.channel], g_gbuf.scale);
		}
	}

	void RequestGbufRead(float a_delaySeconds)
	{
		const float delay = std::clamp(a_delaySeconds, 0.0f, 60.0f);
		g_gbufReadNotBeforeMs.store(SteadyMs() + static_cast<std::int64_t>(delay * 1000.0f));
		g_gbufReadArmed.store(true);
		if (delay > 0.0f) {
			g_sdk->logger->InfoF(g_handle, "overlay: G-buffer reader: in %.0f seconds the raw values at five points of the screen will be written to the log", delay);
		} else {
			g_sdk->logger->Info(g_handle, "overlay: G-buffer reader: the next frame's raw values at five points of the screen will be written to the log");
		}
	}

	// Writes the blocks into the G-buffer: the four layers (in the states a_layers give) and the main depth (in a_depthState) as render targets and depth-stencil for one full-screen pass, then
	// puts them back exactly as they were. False if it could not (nothing is recorded then).
	bool RecordGbufBlocks(ID3D12GraphicsCommandList* a_list, const GbufSource* a_layers, std::uint32_t a_depthState, bool a_haveDepth)
	{
		std::lock_guard<std::recursive_mutex> lock(g_gpuMutex);
		if (!g_gbufBlocksOn.load() || !a_list || !a_layers || !a_haveDepth) {
			return false;
		}
		for (int L = 0; L < kGbufLayers; ++L) {
			if (!a_layers[L].res) {
				return false;
			}
		}
		if (!g.ready || g.failed || !g.srvHeap || !g.rootSignature || !g.device || !g.haveFrame || !g.texture || !g.texDepth || !g.layered) {
			return false;
		}
		ID3D12Resource* depthResource = depth::CandidateResource();
		if (!depthResource) {
			return false;
		}
		const DXGI_FORMAT want[kGbufLayers] = { DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R16G16_FLOAT };
		const D3D12_RESOURCE_DESC d0 = a_layers[0].res->GetDesc();
		for (int L = 0; L < kGbufLayers; ++L) {
			if (a_layers[L].res->GetDesc().Format != want[L]) {
				static bool noted = false;
				if (!noted) {
					noted = true;
					g_sdk->logger->InfoF(g_handle, "overlay: the G-buffer layer %d is not in the format the blocks are written in (%d instead of %d): not writing the blocks", L + 1, static_cast<int>(a_layers[L].res->GetDesc().Format), static_cast<int>(want[L]));
				}
				return false;
			}
		}
		if (!EnsureGbufBlocksPipeline()) {
			return false;
		}

		// The same constants as the scene pass: the re-aiming from the camera the picture was drawn through to the camera now, and Minecraft's camera axes (for the face's direction).
		float constants[63] = { g.flipY ? 1.0f : 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, g.zeroToOne ? 1.0f : 0.0f, 0.0f, g.mcA, g.mcB, 0.02f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
		auto& link = Link::Get();
		bool warped = false;
		Link::CameraPose drawn;
		Link::CameraPose target;
		const bool haveDrawn = link.CameraPoseForFrame(g.cameraFrame, drawn);
		const auto aim = std::chrono::steady_clock::now() - std::chrono::microseconds(static_cast<long long>(g_sceneDelayMs * 1000.0f));
		if (g_warpEnabled) {
			warped = haveDrawn && link.CameraPoseAt(aim, target) && BuildWarp(drawn, target, &constants[16]);
		}
		if (haveDrawn) {
			const Axes m = AxesOf(drawn);
			for (int row = 0; row < 3; ++row) {
				for (int col = 0; col < 3; ++col) {
					constants[44 + row * 4 + col] = static_cast<float>(m.v[col][row]);
				}
			}
			constants[47] = 1.0f;
			// The camera's place relative to the emission grid's corner (a whole number of blocks, so its fractional part is the camera's place within its block, which the exact depth uses), and whether
			// there is a grid (for the light sources): as the scene pass passes it. Without a grid, the camera's own place.
			const bool grid = g_emit.valid && g_emit.ready;
			constants[36] = static_cast<float>(drawn.x - (grid ? g_emit.originX : 0));
			constants[37] = static_cast<float>(drawn.y - (grid ? g_emit.originY : 0));
			constants[38] = static_cast<float>(drawn.z - (grid ? g_emit.originZ : 0));
			constants[39] = grid ? 1.0f : 0.0f;
		}
		if (!warped) {
			constants[31] = 0.0f;
		}
		constants[40] = g_gbufEmit;    // sceneParams.x: the scale of the emissive value written for light-emitting blocks
		constants[41] = g_gbufRough;   // .y
		constants[42] = g_gbufGamma;   // .z
		constants[43] = (g_gbufExact && haveDrawn) ? 1.0f : 0.0f;  // .w: the exact depth on block boundaries
		// The motion vector: where the previous camera is relative to the current one, as a matrix and a vector (the slots are ones this pass has no other use for: see PSGbufBlocks). The previous camera
		// is a third of the way back to the camera three frames ago, not the one a frame ago: a single frame's pose came out about half as far again as the engine's own on one read in two.
		if (g_gbufVelScale > 0.0f && warped) {
			Link::CameraPose past;
			if (link.CameraPoseAt(aim - std::chrono::microseconds(static_cast<long long>(g_frameDtSec * 3.0e6)), past)) {
				const Link::CameraPose prev = LerpPose(target, past, 1.0 / 3.0);
				const Axes ac = AxesOf(target);
				const Axes ap = AxesOf(prev);
				double r[3][3];
				for (int i = 0; i < 3; ++i) {
					for (int j = 0; j < 3; ++j) {
						r[i][j] = ap.v[i][0] * ac.v[j][0] + ap.v[i][1] * ac.v[j][1] + ap.v[i][2] * ac.v[j][2];
					}
				}
				const double cx = target.x - prev.x, cy = target.y - prev.y, cz = target.z - prev.z;
				constants[56] = static_cast<float>(r[0][0]);
				constants[57] = static_cast<float>(r[0][1]);
				constants[58] = static_cast<float>(r[0][2]);
				constants[59] = static_cast<float>(r[1][0]);
				constants[60] = static_cast<float>(r[1][1]);
				constants[61] = static_cast<float>(r[1][2]);
				constants[62] = static_cast<float>(r[2][0]);
				constants[4] = static_cast<float>(r[2][1]);
				constants[5] = static_cast<float>(r[2][2]);
				constants[11] = static_cast<float>(ap.v[0][0] * cx + ap.v[0][1] * cy + ap.v[0][2] * cz);
				constants[12] = static_cast<float>(ap.v[1][0] * cx + ap.v[1][1] * cy + ap.v[1][2] * cz);
				constants[13] = static_cast<float>(ap.v[2][0] * cx + ap.v[2][1] * cy + ap.v[2][2] * cz);
				constants[14] = g_gbufVelScale;
				constants[15] = 1.0f;
			}
		}

		// Views of the four layers and of the main depth.
		D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_gbb.rtvHeap->GetCPUDescriptorHandleForHeapStart();
		const D3D12_CPU_DESCRIPTOR_HANDLE rtvStart = rtv;
		for (int L = 0; L < kGbufLayers; ++L) {
			D3D12_RENDER_TARGET_VIEW_DESC rv{};
			rv.Format = want[L];
			rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
			g.device->CreateRenderTargetView(a_layers[L].res, &rv, rtv);
			rtv.ptr += g_gbb.rtvInc;
		}
		D3D12_DEPTH_STENCIL_VIEW_DESC dv{};
		dv.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
		dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
		const D3D12_CPU_DESCRIPTOR_HANDLE dsv = g_gbb.dsvHeap->GetCPUDescriptorHandleForHeapStart();
		g.device->CreateDepthStencilView(depthResource, &dv, dsv);

		// The layers and the depth in the states a pass writing them needs (at the moment this is meant for they already are: nothing to move), and back afterwards.
		D3D12_RESOURCE_BARRIER pre[kGbufLayers];
		D3D12_RESOURCE_BARRIER post[kGbufLayers];
		UINT moved = 0;
		for (int L = 0; L < kGbufLayers; ++L) {
			const auto state = static_cast<D3D12_RESOURCE_STATES>(a_layers[L].state);
			if (state != D3D12_RESOURCE_STATE_RENDER_TARGET) {
				pre[moved] = Transition(a_layers[L].res, state, D3D12_RESOURCE_STATE_RENDER_TARGET);
				post[moved] = Transition(a_layers[L].res, D3D12_RESOURCE_STATE_RENDER_TARGET, state);
				++moved;
			}
		}
		if (moved) {
			a_list->ResourceBarrier(moved, pre);
		}
		const bool moveDepth = a_depthState != static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_DEPTH_WRITE);
		if (moveDepth) {
			depth::RecordCandidateBarrier(a_list, a_depthState, static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_DEPTH_WRITE));
		}

		ID3D12DescriptorHeap* heaps[] = { g.srvHeap.Get() };
		a_list->SetDescriptorHeaps(1, heaps);
		a_list->SetGraphicsRootSignature(g.rootSignature.Get());
		a_list->SetGraphicsRootDescriptorTable(0, g.srvHeap->GetGPUDescriptorHandleForHeapStart());
		a_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		a_list->OMSetRenderTargets(kGbufLayers, &rtvStart, TRUE, &dsv);
		D3D12_VIEWPORT viewport{ 0.0f, 0.0f, float(d0.Width), float(d0.Height), 0.0f, 1.0f };
		a_list->RSSetViewports(1, &viewport);
		D3D12_RECT scissor{ 0, 0, LONG(d0.Width), LONG(d0.Height) };
		a_list->RSSetScissorRects(1, &scissor);
		a_list->SetPipelineState(g_gbb.pso.Get());
		a_list->OMSetStencilRef(0);
		a_list->SetGraphicsRoot32BitConstants(1, 63, constants, 0);
		a_list->DrawInstanced(3, 1, 0, 0);

		if (moveDepth) {
			depth::RecordCandidateBarrier(a_list, static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_DEPTH_WRITE), a_depthState);
		}
		if (moved) {
			a_list->ResourceBarrier(moved, post);
		}
		++g_gbufBlocksDrawn;
		++g_sceneDrawn;  // so that the Present-time draw leaves the blocks out this frame
		return true;
	}

	bool RecordGbufSnapshot(ID3D12GraphicsCommandList* a_list, const GbufSource* a_layers, std::uint32_t a_depthState, bool a_haveDepth)
	{
		std::lock_guard<std::recursive_mutex> lock(g_gpuMutex);
		if (!a_list || !a_layers || !g.ready || g.failed || !g.device || !g.srvHeap) {
			return false;
		}
		const bool readDue = g_gbufReadArmed.load() && SteadyMs() >= g_gbufReadNotBeforeMs.load();
		const bool wantRead = readDue && EnsureGbufReadback();
		if (!wantRead && g_gbufViewLayer.load() <= 0) {
			return false;  // nothing to look at or read: no copies
		}
		bool any = false;
		for (int L = 0; L < kGbufLayers; ++L) {
			ID3D12Resource* src = a_layers[L].res;
			if (!src || !EnsureGbufTexture(L, src)) {
				continue;
			}
			if (L == 0 && wantRead) {
				const D3D12_RESOURCE_DESC d = src->GetDesc();
				g_gbuf.screenW = static_cast<UINT>(d.Width);
				g_gbuf.screenH = d.Height;
				const UINT cx = static_cast<UINT>(d.Width / 2), cy = d.Height / 2;
				const UINT pts[kGbufPoints][2] = { { cx, cy }, { cx - 160, cy }, { cx + 160, cy }, { cx, cy - 160 }, { cx, cy + 160 } };
				std::memcpy(g_gbuf.points, pts, sizeof(pts));
			}
			const auto state = static_cast<D3D12_RESOURCE_STATES>(a_layers[L].state);
			const bool lift = (state & D3D12_RESOURCE_STATE_COPY_SOURCE) == 0;
			if (lift) {
				auto toSource = Transition(src, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
				a_list->ResourceBarrier(1, &toSource);
			}
			a_list->CopyResource(g_gbuf.tex[L].Get(), src);
			if (wantRead) {
				for (int p = 0; p < kGbufPoints; ++p) {
					D3D12_TEXTURE_COPY_LOCATION dst{};
					dst.pResource = g_gbuf.readback.Get();
					dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
					dst.PlacedFootprint.Offset = (UINT64(L) * kGbufPoints + p) * kGbufStride;
					dst.PlacedFootprint.Footprint.Format = g_gbuf.formats[L];
					dst.PlacedFootprint.Footprint.Width = 1;
					dst.PlacedFootprint.Footprint.Height = 1;
					dst.PlacedFootprint.Footprint.Depth = 1;
					dst.PlacedFootprint.Footprint.RowPitch = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
					D3D12_TEXTURE_COPY_LOCATION from{};
					from.pResource = src;
					from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
					from.SubresourceIndex = 0;
					D3D12_BOX box{ g_gbuf.points[p][0], g_gbuf.points[p][1], 0, g_gbuf.points[p][0] + 1, g_gbuf.points[p][1] + 1, 1 };
					a_list->CopyTextureRegion(&dst, 0, 0, 0, &from, &box);
				}
			}
			if (lift) {
				auto back = Transition(src, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
				a_list->ResourceBarrier(1, &back);
			}
			g_gbuf.copied[L] = true;
			any = true;
		}
		if (wantRead) {
			if (a_haveDepth) {
				depth::RecordTexelReadback(a_list, a_depthState, g_gbuf.readback.Get(), 20 * kGbufStride, 25 * kGbufStride, kGbufStride, g_gbuf.points, kGbufPoints);
			}
			g_gbuf.readCountdown = 8;
			g_gbufReadArmed.store(false);
			// The camera now and a frame ago, for the predicted motion vectors.
			const auto now = std::chrono::steady_clock::now();
			auto& link = Link::Get();
			g_gbuf.dt = g_frameDtSec;
			g_gbuf.havePose = link.CameraPoseAt(now, g_gbuf.poseCur) && link.CameraPoseAt(now - std::chrono::microseconds(static_cast<long long>(g_frameDtSec * 1.0e6)), g_gbuf.posePrev);
		}
		return any;
	}

	bool MeasureScene(ID3D12GraphicsCommandList* a_list, ID3D12Resource* a_scene, std::uint32_t a_sceneState)
	{
		std::lock_guard<std::recursive_mutex> lock(g_gpuMutex);
		if (!a_list || !a_scene || !g.ready || g.failed || !g.srvHeap || !g.rootSignature || !g.device) {
			return false;
		}
		if (g_adaptStrength <= 0.0f && !g_adapt.ready) {
			return false;  // never used: no cost
		}
		if (!EnsureScenePipeline() || !EnsureAdapt() || !g.psoMeasure1 || !g.psoMeasure2) {
			return false;
		}
		if (g_adapt.boundScene != a_scene) {
			if (g_adapt.boundScene) {
				WaitIdle();  // the descriptor may be in use by a frame still on the GPU
			}
			D3D12_SHADER_RESOURCE_VIEW_DESC v{};
			v.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
			v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			v.Texture2D.MipLevels = 1;
			D3D12_CPU_DESCRIPTOR_HANDLE c = g.srvHeap->GetCPUDescriptorHandleForHeapStart();
			c.ptr += SIZE_T(8) * g.srvInc;
			g.device->CreateShaderResourceView(a_scene, &v, c);
			g_adapt.boundScene = a_scene;
			g_adapt.reset = true;
		}
		const auto now = std::chrono::steady_clock::now();
		float dt = g_adapt.last.time_since_epoch().count() == 0 ? 0.016f : std::chrono::duration<float>(now - g_adapt.last).count();
		dt = std::clamp(dt, 0.001f, 0.5f);
		g_adapt.last = now;

		// The scene is readable as it is if its state has the pixel shader resource bit (the state our list is inserted at has it: 0x8C0); if not, lift it for the passes.
		const auto sceneState = static_cast<D3D12_RESOURCE_STATES>(a_sceneState);
		const bool lift = (sceneState & D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) == 0;
		if (lift) {
			auto toRead = Transition(a_scene, sceneState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			a_list->ResourceBarrier(1, &toRead);
		}

		ID3D12DescriptorHeap* heaps[] = { g.srvHeap.Get() };
		a_list->SetDescriptorHeaps(1, heaps);
		a_list->SetGraphicsRootSignature(g.rootSignature.Get());
		a_list->SetGraphicsRootDescriptorTable(0, g.srvHeap->GetGPUDescriptorHandleForHeapStart());
		a_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		float constants[63] = {};
		D3D12_CPU_DESCRIPTOR_HANDLE smallRtv = g_adapt.rtvHeap->GetCPUDescriptorHandleForHeapStart();
		D3D12_CPU_DESCRIPTOR_HANDLE curRtv = smallRtv;
		curRtv.ptr += g_adapt.rtvInc;

		// Pass one: the scene's log brightness in 16 x 9 cells.
		auto smallToTarget = Transition(g_adapt.cells.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
		a_list->ResourceBarrier(1, &smallToTarget);
		a_list->OMSetRenderTargets(1, &smallRtv, FALSE, nullptr);
		D3D12_VIEWPORT viewport1{ 0.0f, 0.0f, 16.0f, 9.0f, 0.0f, 1.0f };
		D3D12_RECT scissor1{ 0, 0, 16, 9 };
		a_list->RSSetViewports(1, &viewport1);
		a_list->RSSetScissorRects(1, &scissor1);
		a_list->SetPipelineState(g.psoMeasure1.Get());
		a_list->SetGraphicsRoot32BitConstants(1, 63, constants, 0);
		a_list->DrawInstanced(3, 1, 0, 0);
		auto smallToRead = Transition(g_adapt.cells.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		a_list->ResourceBarrier(1, &smallToRead);

		// Pass two: the average, smoothed with last frame's, as the gain multiplier.
		auto curToTarget = Transition(g_adapt.cur.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
		a_list->ResourceBarrier(1, &curToTarget);
		a_list->OMSetRenderTargets(1, &curRtv, FALSE, nullptr);
		D3D12_VIEWPORT viewport2{ 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f };
		D3D12_RECT scissor2{ 0, 0, 1, 1 };
		a_list->RSSetViewports(1, &viewport2);
		a_list->RSSetScissorRects(1, &scissor2);
		a_list->SetPipelineState(g.psoMeasure2.Get());
		constants[40] = g_adapt.reset ? 1.0f : 0.0f;       // sceneParams.x: no history yet
		constants[41] = dt;                                 // sceneParams.y
		constants[42] = g_adaptTau;                         // sceneParams.z
		constants[43] = g_adapt.calibrate ? 1.0f : 0.0f;    // sceneParams.w: take the brightness now as the reference
		constants[56] = kAdaptStartReference;               // aoParams.x
		constants[58] = g_adaptStrength;                    // aoParams.z
		constants[60] = g_adaptPower;                       // fogParams.x
		constants[61] = 0.25f;                              // fogParams.y: the smallest multiplier
		constants[62] = 4.0f;                               // fogParams.z: the largest
		a_list->SetGraphicsRoot32BitConstants(1, 63, constants, 0);
		a_list->DrawInstanced(3, 1, 0, 0);

		// The result is this frame's (read by the blocks, so it ends in the readable state), and a copy of it is next frame's history.
		auto curToCopy = Transition(g_adapt.cur.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
		auto prevToCopy = Transition(g_adapt.prev.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
		D3D12_RESOURCE_BARRIER toCopy[] = { curToCopy, prevToCopy };
		a_list->ResourceBarrier(2, toCopy);
		a_list->CopyResource(g_adapt.prev.Get(), g_adapt.cur.Get());
		auto curToRead = Transition(g_adapt.cur.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		auto prevToRead = Transition(g_adapt.prev.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		D3D12_RESOURCE_BARRIER toRead[] = { curToRead, prevToRead };
		a_list->ResourceBarrier(2, toRead);

		if (lift) {
			auto back = Transition(a_scene, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, sceneState);
			a_list->ResourceBarrier(1, &back);
		}
		g_adapt.reset = false;
		g_adapt.calibrate = false;
		return true;
	}

	void SetAdapt(float a_strengthPercent, float a_powerPercent)
	{
		std::lock_guard<std::recursive_mutex> lock(g_gpuMutex);
		g_adaptStrength = std::clamp(a_strengthPercent, 0.0f, 100.0f) / 100.0f;
		g_adaptPower = std::clamp(a_powerPercent, -150.0f, 150.0f) / 100.0f;
		g_sdk->logger->InfoF(g_handle, "overlay: the blocks' gain follows the city's brightness at %.0f%%, to the power %.2f (the multiplier is (brightness now / reference) to that power, kept between 0.25 and 4)", g_adaptStrength * 100.0f, g_adaptPower);
	}

	void CalibrateAdapt()
	{
		std::lock_guard<std::recursive_mutex> lock(g_gpuMutex);
		g_adapt.calibrate = true;
		g_sdk->logger->Info(g_handle, "overlay: the city's brightness now is taken as the reference the gain is right for");
	}

	bool DrawWorldIntoScene(ID3D12GraphicsCommandList* a_list, D3D12_CPU_DESCRIPTOR_HANDLE a_rtv, UINT a_width, UINT a_height)
	{
		std::lock_guard<std::recursive_mutex> lock(g_gpuMutex);
		if (!g.ready || g.failed || !g.srvHeap || !g.rootSignature || !g.device) {
			g_sceneWhy = "the overlay is not ready (or failed)";
			return false;
		}
		if (!g.haveFrame || !g.texture || !g.texDepth) {
			g_sceneWhy = "no Minecraft frame has arrived yet";
			return false;
		}
		if (!g.layered) {
			g_sceneWhy = "the last Minecraft frame was not a layered one (a menu or a plain frame)";
			return false;
		}
		depth::GameDepth gameDepth;
		if (!depth::GetGameDepth(gameDepth)) {
			g_sceneWhy = "there is no copy of the game's depth (the depth capture is off or restarting)";
			return false;  // without the game's depth the blocks can't be hidden behind the world: draw nothing
		}
		if (!EnsureScenePipeline()) {
			g_sceneWhy = "the pipeline for the scene could not be made";
			return false;
		}
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
		}
		auto toRead = Transition(gameDepth.resource, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		a_list->ResourceBarrier(1, &toRead);

		ID3D12DescriptorHeap* heaps[] = { g.srvHeap.Get() };
		a_list->SetDescriptorHeaps(1, heaps);
		a_list->SetGraphicsRootSignature(g.rootSignature.Get());
		a_list->SetGraphicsRootDescriptorTable(0, g.srvHeap->GetGPUDescriptorHandleForHeapStart());
		a_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		D3D12_VIEWPORT viewport{ 0.0f, 0.0f, float(a_width), float(a_height), 0.0f, 1.0f };
		a_list->RSSetViewports(1, &viewport);
		D3D12_RECT scissor{ 0, 0, LONG(a_width), LONG(a_height) };
		a_list->RSSetScissorRects(1, &scissor);

		float constants[63] = { g.flipY ? 1.0f : 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, g.zeroToOne ? 1.0f : 0.0f, 1.0f,
			g.mcA, g.mcB, 0.02f /* the game's near plane */, 0.06f, 0.004f, 0.0f, 0.0f, 0.0f };
		auto& link = Link::Get();
		bool warped = false;
		Link::CameraPose drawn;
		Link::CameraPose target;
		bool haveTarget = false;
		const bool haveDrawn = link.CameraPoseForFrame(g.cameraFrame, drawn);
		if (g_warpEnabled) {
			const auto aim = std::chrono::steady_clock::now() - std::chrono::microseconds(static_cast<long long>(g_sceneDelayMs * 1000.0f));
			haveTarget = link.CameraPoseAt(aim, target);
			warped = haveDrawn && haveTarget && BuildWarp(drawn, target, &constants[16]);
		}
		if ((g_sceneAo > 0.0f || g_sceneAoView || g_terrainAo > 0.0f) && (haveDrawn || haveTarget)) {
			// Ambient occlusion needs the camera's field of view.
			constexpr double kRad = 3.14159265358979323846 / 180.0;
			const double tanHalfY = std::tan((haveTarget ? target.vfov : drawn.vfov) * 0.5 * kRad);
			if (tanHalfY > 0.01) {
				constants[56] = g_sceneAoView ? -std::max(g_sceneAo, 0.01f) : g_sceneAo;   // the blocks' own occlusion: strength (negative: show only the occlusion)
				constants[57] = g_sceneAoRadius;
				constants[58] = 0.0f;                                                       // (set below for the terrain's occlusion: its strength)
				constants[59] = static_cast<float>(tanHalfY);
			}
		}
		if (haveDrawn) {
			// Minecraft's camera axes in Minecraft's world, as the rows of the matrix from camera space to world space: used to tell which way a face points.
			const Axes m = AxesOf(drawn);
			for (int row = 0; row < 3; ++row) {
				for (int col = 0; col < 3; ++col) {
					constants[44 + row * 4 + col] = static_cast<float>(m.v[col][row]);
				}
			}
			constants[47] = 1.0f;  // valid
			if (g_emit.valid && g_emit.ready) {
				// Where Minecraft's camera is relative to the corner of the light emission grid (the pose is in Minecraft's world); [39] says there is a grid.
				constants[36] = static_cast<float>(drawn.x - g_emit.originX);
				constants[37] = static_cast<float>(drawn.y - g_emit.originY);
				constants[38] = static_cast<float>(drawn.z - g_emit.originZ);
				constants[39] = 1.0f;
			}
		}
		if (!warped) {
			constants[31] = 0.0f;
		}
		constants[41] = g_sceneGain;
		constants[42] = g_fogGlowExp;  // (at Present this slot says whether the blocks are left out; in the scene's passes it is free)
		constants[43] = g_sceneGlow;
		// One of the game's fog volumes, for the debug view: bound to the seventh descriptor, and moved to a readable state for the blocks' pass if it isn't in one.
		ID3D12Resource* fogResource = nullptr;
		D3D12_RESOURCE_STATES fogState = D3D12_RESOURCE_STATE_COMMON;
		bool fogMoved = false;
		if (g_fogView > 0) {
			std::size_t pick = g_fogIndex < 0 ? 0 : static_cast<std::size_t>(g_fogIndex);
			if (g_fogIndex < 0) {
				// By behaviour, not by address (the addresses change from one run to the next): the volume that is readable when the scene is complete and was written by compute the most.
				std::uint32_t most = 0;
				for (std::size_t i = 0; i < g_fogVolumes.size(); ++i) {
					if ((g_fogVolumes[i].state & D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) != 0 && g_fogVolumes[i].uavWrites > most) {
						most = g_fogVolumes[i].uavWrites;
						pick = i;
					}
				}
			}
			if (pick < g_fogVolumes.size()) {
				fogResource = g_fogVolumes[pick].resource;
				fogState = static_cast<D3D12_RESOURCE_STATES>(g_fogVolumes[pick].state);
				if (g.boundFog != fogResource) {
					WaitIdle();  // the descriptor may be in use by a frame still on the GPU
					D3D12_SHADER_RESOURCE_VIEW_DESC view{};
					view.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
					view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
					view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
					view.Texture3D.MostDetailedMip = 0;
					view.Texture3D.MipLevels = 1;
					D3D12_CPU_DESCRIPTOR_HANDLE h = g.srvHeap->GetCPUDescriptorHandleForHeapStart();
					h.ptr += SIZE_T(6) * g.srvInc;
					g.device->CreateShaderResourceView(fogResource, &view, h);
					g.boundFog = fogResource;
					g_sdk->logger->InfoF(g_handle, "overlay: using fog volume %u of %u (%p)%s", static_cast<unsigned>(pick), static_cast<unsigned>(g_fogVolumes.size()), static_cast<void*>(fogResource), g_fogIndex < 0 ? ", picked as the one written most often by compute" : "");
				}
				if ((fogState & D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) == 0) {
					D3D12_RESOURCE_BARRIER b{};
					b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
					b.Transition.pResource = fogResource;
					b.Transition.Subresource = 0;
					b.Transition.StateBefore = fogState;
					b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
					a_list->ResourceBarrier(1, &b);
					fogMoved = true;
				}
				constants[60] = static_cast<float>(g_fogView);
				constants[61] = g_fogNear;
				constants[62] = g_fogExp ? g_fogFar : -g_fogFar;
			} else {
				static auto lastNote = std::chrono::steady_clock::time_point{};
				const auto t = std::chrono::steady_clock::now();
				if (t - lastNote > std::chrono::seconds(8)) {
					lastNote = t;
					g_sdk->logger->InfoF(g_handle, "overlay: there is no fog volume %d (the game has %u of them at this point of the frame)", g_fogIndex, static_cast<unsigned>(g_fogVolumes.size()));
				}
			}
		}
		const bool haveCamera = haveDrawn || haveTarget;
		// The terrain's occlusion needs the camera's field of view (set above with the block occlusion's); its strength rides in aoParams.z.
		if (haveCamera && (g_terrainAo > 0.0f || g_sceneAoView) && constants[59] > 0.0f) {
			if (EnsureBlockDepth(a_width, a_height)) {
				constants[58] = std::max(g_terrainAo, g_sceneAoView ? 0.01f : 0.0f);
				// Pass 1: how far the nearest block is at each place on the screen, from the current camera (0 where there is none).
				auto toTarget = Transition(g.blockDepth.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
				a_list->ResourceBarrier(1, &toTarget);
				D3D12_CPU_DESCRIPTOR_HANDLE depthRtv = g.sceneRtvHeap->GetCPUDescriptorHandleForHeapStart();
				a_list->OMSetRenderTargets(1, &depthRtv, FALSE, nullptr);
				const float none[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
				a_list->ClearRenderTargetView(depthRtv, none, 0, nullptr);
				a_list->SetPipelineState(g.psoDepth.Get());
				constants[40] = 2.0f;
				a_list->SetGraphicsRoot32BitConstants(1, 63, constants, 0);
				a_list->DrawInstanced(3, 1, 0, 0);
				auto toRead = Transition(g.blockDepth.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
				a_list->ResourceBarrier(1, &toRead);
				// Pass 2: the game's own surfaces darkened where blocks are close to them (multiplied onto the scene).
				a_list->OMSetRenderTargets(1, &a_rtv, FALSE, nullptr);
				a_list->SetPipelineState(g_sceneAoView ? g.psoTerrainView.Get() : g.psoTerrain.Get());
				constants[40] = 3.0f;
				a_list->SetGraphicsRoot32BitConstants(1, 63, constants, 0);
				a_list->DrawInstanced(3, 1, 0, 0);
			}
		}
		if (g_fogView >= 6) {
			constants[60] = float(g_fogView);  // the light level and gain views need no fog volume
		}
		// The blocks themselves, over all of that.
		a_list->OMSetRenderTargets(1, &a_rtv, FALSE, nullptr);
		a_list->SetPipelineState(g.psoScene.Get());
		constants[40] = 1.0f;          // into the scene
		constants[58] = g_glowStart;   // (the terrain's occlusion strength was in this slot for its pass)
		a_list->SetGraphicsRoot32BitConstants(1, 63, constants, 0);
		a_list->DrawInstanced(3, 1, 0, 0);

		if (fogMoved) {
			D3D12_RESOURCE_BARRIER b{};
			b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			b.Transition.pResource = fogResource;
			b.Transition.Subresource = 0;
			b.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			b.Transition.StateAfter = fogState;
			a_list->ResourceBarrier(1, &b);
		}
		auto back = Transition(gameDepth.resource, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
		a_list->ResourceBarrier(1, &back);
		++g_sceneDrawn;
		return true;
	}

	const char* SceneBlockedReason()
	{
		return g_sceneWhy;
	}

	void SetFogVolumes(const std::vector<FogVolume>& a_volumes)
	{
		std::lock_guard<std::recursive_mutex> lock(g_gpuMutex);
		g_fogVolumes = a_volumes;
	}

	void SetFogView(int a_mode, int a_index)
	{
		g_fogView = std::clamp(a_mode, 0, 7);
		g_fogIndex = a_index;
		g_sdk->logger->InfoF(g_handle, "overlay: fog on the blocks: %s (volume %s)", g_fogView == 0 ? "off" : g_fogView == 1 ? "showing the fog's light instead of the blocks" : g_fogView == 2 ? "showing the fog's transmittance instead of the blocks" : g_fogView == 3 ? "applied to the blocks" : g_fogView == 4 ? "showing the blocks' own distance as colour bands (a metre, four metres and twenty-five)" : g_fogView == 5 ? "applied to plain grey blocks (no texture, shading, occlusion or glow)" : g_fogView == 6 ? "showing the blocks' light level (white: 15, black: none, magenta: no grid)" : "showing the gain multiplier from the city's brightness (grey: 1, white: 4, dark: below 1)",
			a_index < 0 ? "picked by behaviour" : std::to_string(a_index).c_str());
	}

	void SetFogRange(float a_nearMetres, float a_farMetres)
	{
		g_fogNear = std::clamp(a_nearMetres, 0.01f, 100.0f);
		g_fogFar = std::clamp(a_farMetres, g_fogNear + 0.1f, 5000.0f);
		g_sdk->logger->InfoF(g_handle, "overlay: the fog volume's slices run from %.2f m to %.1f m (%s)", g_fogNear, g_fogFar, g_fogExp ? "exponentially spread" : "evenly spread");
	}

	void SetGlowStart(float a_percent)
	{
		g_glowStart = std::clamp(a_percent, 30.0f, 99.0f) / 100.0f;
		g_sdk->logger->InfoF(g_handle, "overlay: pixels count as light-emitting from %.0f%% of full brightness (after undoing Minecraft's face shading)", g_glowStart * 100.0f);
	}

	void SetFogGlow(float a_percent)
	{
		g_fogGlowExp = std::clamp(a_percent, 0.0f, 150.0f) / 100.0f;
		g_sdk->logger->InfoF(g_handle, "overlay: glowing pixels feel the fog's transmittance to the power %.2f (100%% is the physical amount, 0%% no fog on their glow)", g_fogGlowExp);
	}

	void SetFogCurve(bool a_exponential)
	{
		g_fogExp = a_exponential;
		g_sdk->logger->InfoF(g_handle, "overlay: the fog volume's slices are %s", g_fogExp ? "exponentially spread with distance" : "evenly spread with distance");
	}

	void SetSceneTerrainAo(float a_percent)
	{
		g_terrainAo = std::clamp(a_percent, 0.0f, 100.0f) / 100.0f;
		g_sdk->logger->InfoF(g_handle, "overlay: ambient occlusion on the game's surfaces next to blocks: %s (strength %.0f%%)", g_terrainAo > 0.0f ? "on" : "off", g_terrainAo * 100.0f);
	}

	void SetSceneAoView(bool a_on)
	{
		g_sceneAoView = a_on;
		g_sdk->logger->InfoF(g_handle, "overlay: ambient occlusion view: %s", a_on ? "showing only the occlusion (white: none, dark: a lot); strength 0 is treated as 1%" : "off");
	}

	void SetSceneAo(float a_percent, float a_radiusCm)
	{
		g_sceneAo = std::clamp(a_percent, 0.0f, 100.0f) / 100.0f;
		if (a_radiusCm > 0.0f) {
			g_sceneAoRadius = std::clamp(a_radiusCm, 10.0f, 300.0f) / 100.0f;
		}
		g_sdk->logger->InfoF(g_handle, "overlay: ambient occlusion where blocks meet the game's world: %s (strength %.0f%%, radius %.2f m)", g_sceneAo > 0.0f ? "on" : "off", g_sceneAo * 100.0f, g_sceneAoRadius);
	}

	void SetSceneGlow(float a_percent)
	{
		g_sceneGlow = std::clamp(a_percent, 0.0f, 3000.0f) / 100.0f;
		g_sdk->logger->InfoF(g_handle, "overlay: the brightest pixels of the blocks drawn into the game's scene are boosted by up to %.0f%%", g_sceneGlow * 100.0f);
	}

	void SetSceneGain(float a_percent)
	{
		g_sceneGain = std::clamp(a_percent, 5.0f, 2000.0f) / 100.0f;
		g_sdk->logger->InfoF(g_handle, "overlay: the blocks drawn into the game's scene are %.0f%% as bright as a Minecraft white would be in the scene's units", g_sceneGain * 100.0f);
	}

	void SetSceneDelay(float a_ms)
	{
		g_sceneDelayMs = std::clamp(a_ms, 0.0f, 150.0f);
		g_sdk->logger->InfoF(g_handle, "overlay: the blocks drawn into the game's scene are aimed %.0f ms behind the newest published camera", g_sceneDelayMs);
	}

	void SetUiTransform(float a_scaleXPercent, float a_scaleYPercent, float a_shiftX, float a_shiftY, bool a_setScale)
	{
		if (a_setScale) {
			g_uiScaleX = std::clamp(a_scaleXPercent, 50.0f, 200.0f) / 100.0f;
			g_uiScaleY = std::clamp(a_scaleYPercent, 50.0f, 200.0f) / 100.0f;
		} else {
			g_uiShiftX = std::clamp(a_shiftX, -400.0f, 400.0f);
			g_uiShiftY = std::clamp(a_shiftY, -400.0f, 400.0f);
		}
		g_sdk->logger->InfoF(g_handle, "overlay: the game's interface layer is stretched to %.1f%% across and %.1f%% down about the middle, and shifted by (%.1f, %.1f) pixels", g_uiScaleX * 100.0f, g_uiScaleY * 100.0f, g_uiShiftX, g_uiShiftY);
	}

	void SetUiLayerMode(int a_mode)
	{
		a_mode = std::clamp(a_mode, 0, 4);
		if (a_mode != g_uiMode) {
			g_uiMode = a_mode;
			g_sdk->logger->InfoF(g_handle, "overlay: the game's interface over the blocks: %s", a_mode == 0 ? "off" : a_mode == 1 ? "on (premultiplied alpha)" : a_mode == 2 ? "on (straight alpha)" : a_mode == 3 ? "showing the captured layer alone" : "showing a magenta shadow of the captured layer over everything, to line it up");
		}
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
