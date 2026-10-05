// Phase 2a: find the ground around V.
//
// A few rays per frame are shot straight down on a 1 m grid around V, using the game's own physics query
// (SpatialQueriesSystem.SyncRaycastByCollisionGroup, the same function redscript mods use). Each hit's
// height goes into the shared ground grid for the Minecraft mod to build as blocks.
//
// Each ray starts a little above where the ground is expected to be (from the last scan of that cell, or
// from its neighbour nearer to V), so it finds the street under V rather than the roof above it.

#include "Ground.hpp"
#include "Link.hpp"

#include <cybercraft_protocol.h>

#include <RED4ext/Scripting/Natives/ScriptGameInstance.hpp>
#include <RED4ext/Scripting/Natives/physicsTraceResult.hpp>
#include <RED4ext/Scripting/Natives/Vector4.hpp>

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <vector>

namespace cybercraft::ground
{
	namespace
	{
		constexpr int kCellsPerFrame = 80;       // cells per frame (each costs 1 to 2 rays; tune with the timings in the log)

		// Collision groups to try, in order. Roads, paths and buildings are in "Static"; the ground itself
		// (grass, dirt, sand) is in "Terrain". A ray only moves on to the next group if the last found nothing.
		constexpr const char* kGroups[] = { "Static", "Terrain" };
		constexpr int kGroupCount = sizeof(kGroups) / sizeof(kGroups[0]);
		constexpr double kRayUp = 2.5;           // start this far above the expected ground
		constexpr double kRayDown = 4.0;         // and end this far below it
		constexpr double kRetryRange = 12.0;     // second try, if the first ray found nothing

		RED4ext::v1::PluginHandle g_handle = nullptr;
		const RED4ext::v1::Sdk* g_sdk = nullptr;

		// Lookups, made lazily the first time they are needed and dropped by Reset().
		bool g_triedInit = false;
		bool g_failed = false; // gave up: don't spam the log
		RED4ext::IScriptable* g_spatial = nullptr;
		RED4ext::CClassFunction* g_raycast = nullptr;
		RED4ext::physics::TraceResult g_trace{};  // the script type "TraceResult": the SDK has its exact layout
		bool g_loggedFirstHit = false;
		bool g_loggedExecFail = false;

		struct CellOffset
		{
			int dx, dz;
		};
		std::vector<CellOffset> g_order;  // every cell within the scan radius, nearest to V first
		std::size_t g_next = 0;

		// Timings, reported once a second.
		std::chrono::steady_clock::time_point g_lastReport{};
		std::uint64_t g_rays = 0, g_hits = 0;
		std::uint64_t g_groupHits[kGroupCount] = {};
		double g_rayMicros = 0.0;

		std::uint64_t PackSlot(float a_height, std::int16_t a_bx, std::int16_t a_bz)
		{
			std::uint32_t bits;
			std::memcpy(&bits, &a_height, sizeof(bits));
			return std::uint64_t(bits) | (std::uint64_t(std::uint16_t(a_bx)) << 32) | (std::uint64_t(std::uint16_t(a_bz)) << 48);
		}

		std::uint64_t* SlotAddress(int a_bx, int a_bz)
		{
			const int n = int(proto::kGroundN);
			const int ix = ((a_bx % n) + n) % n;
			const int iz = ((a_bz % n) + n) % n;
			auto* base = Link::Get().Base();
			return reinterpret_cast<std::uint64_t*>(base + proto::kOffGround) + (iz * n + ix);
		}

		// The height stored for a cell, or NaN if the slot holds some other cell (or nothing).
		float StoredHeight(int a_bx, int a_bz)
		{
			const std::uint64_t value = std::atomic_ref<std::uint64_t>(*SlotAddress(a_bx, a_bz)).load(std::memory_order_acquire);
			const auto bx = std::int16_t(std::uint16_t(value >> 32));
			const auto bz = std::int16_t(std::uint16_t(value >> 48));
			if (bx != std::int16_t(a_bx) || bz != std::int16_t(a_bz)) {
				return std::nanf("");
			}
			const auto bits = std::uint32_t(value & 0xFFFFFFFFu);
			float height;
			std::memcpy(&height, &bits, sizeof(height));
			return height <= proto::kNoGround * 0.5f ? std::nanf("") : height;
		}

		void StoreHeight(int a_bx, int a_bz, float a_height)
		{
			std::atomic_ref<std::uint64_t>(*SlotAddress(a_bx, a_bz))
				.store(PackSlot(a_height, std::int16_t(a_bx), std::int16_t(a_bz)), std::memory_order_release);
		}

		void BuildOrder()
		{
			g_order.clear();
			const int r = proto::kGroundRadius;
			for (int dz = -r; dz <= r; ++dz) {
				for (int dx = -r; dx <= r; ++dx) {
					if (dx * dx + dz * dz <= r * r) {
						g_order.push_back({ dx, dz });
					}
				}
			}
			std::sort(g_order.begin(), g_order.end(), [](const CellOffset& a, const CellOffset& b) {
				return a.dx * a.dx + a.dz * a.dz < b.dx * b.dx + b.dz * b.dz;
			});
			g_next = 0;
		}

		// Finds the game's spatial queries system and the raycast function on it.
		bool Lookup()
		{
			g_triedInit = true;
			auto rtti = RED4ext::CRTTISystem::Get();

			// The script function GameInstance.GetSpatialQueriesSystem(game), as redscript mods call it.
			RED4ext::Handle<RED4ext::IScriptable> system;
			if (auto cls = rtti->GetClass("ScriptGameInstance")) {
				if (auto func = cls->GetFunction("GetSpatialQueriesSystem")) {
					RED4ext::ScriptGameInstance game;
					RED4ext::StackArgs_t args;
					args.emplace_back(nullptr, &game);
					RED4ext::ExecuteFunction(static_cast<void*>(nullptr), func, &system, args);
				}
			}
			if (!system) {
				g_sdk->logger->Error(g_handle, "ground: could not get the spatial queries system");
				return false;
			}

			auto* type = system.instance->GetType();
			g_raycast = type ? type->GetFunction("SyncRaycastByCollisionGroup") : nullptr;
			if (!g_raycast) {
				g_sdk->logger->Error(g_handle, "ground: could not find SyncRaycastByCollisionGroup");
				return false;
			}
			g_spatial = system.instance;

			// What the game says the function takes, in case a guess here is wrong.
			g_sdk->logger->InfoF(g_handle, "ground: raycast takes %u parameters:", static_cast<unsigned>(g_raycast->params.Size()));
			for (uint32_t i = 0; i < g_raycast->params.Size(); ++i) {
				auto* param = g_raycast->params[i];
				g_sdk->logger->InfoF(g_handle, "  - %s : %s%s", param->name.ToString(),
					param->type ? param->type->GetName().ToString() : "?", param->flags.isOut ? " (out)" : "");
			}

			g_sdk->logger->Info(g_handle, "ground: ready");
			BuildOrder();
			return true;
		}

		// Shoots one ray straight down through (a_x, a_y) between two heights. Returns the height it hit, or NaN.
		float Cast(const char* a_group, double a_x, double a_y, double a_zTop, double a_zBottom)
		{
			RED4ext::Vector4 from(float(a_x), float(a_y), float(a_zTop), 1.0f);
			RED4ext::Vector4 to(float(a_x), float(a_y), float(a_zBottom), 1.0f);
			RED4ext::CName group(a_group);
			bool staticOnly = true;
			bool dynamicOnly = false;

			RED4ext::StackArgs_t args;
			args.emplace_back(nullptr, &from);
			args.emplace_back(nullptr, &to);
			args.emplace_back(nullptr, &group);
			std::memset(&g_trace, 0, sizeof(g_trace));
			args.emplace_back(nullptr, &g_trace);
			args.emplace_back(nullptr, &staticOnly);
			args.emplace_back(nullptr, &dynamicOnly);

			bool hit = false;
			const bool executed = RED4ext::ExecuteFunction(g_spatial, g_raycast, &hit, args);
			if (!executed && !g_loggedExecFail) {
				g_loggedExecFail = true;
				g_sdk->logger->Error(g_handle, "ground: the raycast call itself failed to run");
			}
			if (!executed || !hit) {
				return std::nanf("");
			}
			if (!g_loggedFirstHit) {
				g_loggedFirstHit = true;
				g_sdk->logger->InfoF(g_handle, "ground: first hit at x=%.2f y=%.2f z=%.2f (ray from z=%.2f to z=%.2f)",
					g_trace.position.X, g_trace.position.Y, g_trace.position.Z, a_zTop, a_zBottom);
			}
			return g_trace.position.Z;
		}

		// Tries each collision group in turn; returns the first hit's height, or NaN.
		float CastAllGroups(double a_x, double a_y, double a_zTop, double a_zBottom)
		{
			for (int i = 0; i < kGroupCount; ++i) {
				const float height = Cast(kGroups[i], a_x, a_y, a_zTop, a_zBottom);
				if (!std::isnan(height)) {
					++g_groupHits[i];
					return height;
				}
			}
			return std::nanf("");
		}

		void ScanCell(int a_bx, int a_bz, int a_dx, int a_dz, double a_vz)
		{
			// Where do we expect the ground to be? Last scan of this cell, else the cell one step nearer to V, else V's feet.
			float expected = StoredHeight(a_bx, a_bz);
			if (std::isnan(expected)) {
				const int sx = (a_dx > 0) - (a_dx < 0);
				const int sz = (a_dz > 0) - (a_dz < 0);
				expected = StoredHeight(a_bx - sx, a_bz - sz);
			}
			if (std::isnan(expected)) {
				expected = float(a_vz);
			}

			// The cell's centre: Minecraft block (bx, bz) covers X in [bx, bx+1) and Z in [bz, bz+1); Cyberpunk Y is -Z.
			const double cx = a_bx + 0.5;
			const double cy = -(a_bz + 0.5);

			float height = CastAllGroups(cx, cy, expected + kRayUp, expected - kRayDown);
			if (std::isnan(height)) {
				height = CastAllGroups(cx, cy, expected + kRetryRange, expected - kRetryRange);
			}

			++g_rays;
			if (!std::isnan(height)) {
				++g_hits;
			}
			StoreHeight(a_bx, a_bz, std::isnan(height) ? proto::kNoGround : height);
		}
	}

	void Init(RED4ext::v1::PluginHandle a_handle, const RED4ext::v1::Sdk* a_sdk)
	{
		g_handle = a_handle;
		g_sdk = a_sdk;
	}

	void Reset()
	{
		// Cached game objects are only valid inside one loaded world.
		g_spatial = nullptr;
		g_raycast = nullptr;
		g_triedInit = false;
		g_failed = false;
	}

	void Update(double a_vx, double a_vy, double a_vz)
	{
		auto& link = Link::Get();
		if (!link.IsOpen() || g_failed) {
			return;
		}
		if (!g_triedInit && !Lookup()) {
			g_failed = true;
			return;
		}

		// V's cell, in Minecraft block coordinates: X stays, Z is -Y.
		const int centreBx = int(std::floor(a_vx));
		const int centreBz = int(std::floor(-a_vy));

		const auto start = std::chrono::steady_clock::now();
		for (int i = 0; i < kCellsPerFrame; ++i) {
			const CellOffset& o = g_order[g_next];
			g_next = (g_next + 1) % g_order.size();
			ScanCell(centreBx + o.dx, centreBz + o.dz, o.dx, o.dz, a_vz);
		}
		g_rayMicros += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();

		const auto now = std::chrono::steady_clock::now();
		if (now - g_lastReport >= std::chrono::seconds(5)) {
			g_lastReport = now;
			if (g_rays > 0) {
				g_sdk->logger->InfoF(g_handle, "ground: %llu cells in 5 s, %.0f%% found ground, %.1f us per cell",
					static_cast<unsigned long long>(g_rays), 100.0 * double(g_hits) / double(g_rays), g_rayMicros / double(g_rays));
				for (int i = 0; i < kGroupCount; ++i) {
					g_sdk->logger->InfoF(g_handle, "  hits in group \"%s\": %llu", kGroups[i], static_cast<unsigned long long>(g_groupHits[i]));
				}
			}
			g_rays = g_hits = 0;
			std::fill(std::begin(g_groupHits), std::end(g_groupHits), 0ull);
			g_rayMicros = 0.0;
		}
	}
}
