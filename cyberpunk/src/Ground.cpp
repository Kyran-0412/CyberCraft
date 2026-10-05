// Phases 2a and 2b-ii: find the ground around V, and what is standing on it.
//
// Using the game's own physics queries (SpatialQueriesSystem, the same functions redscript mods use), the
// plugin looks at the 1 m cells around V, nearest first, as many per frame as fit in a small time budget:
//
//   * A ray is shot straight down through the cell's centre to find the ground's height.
//     It starts a little above where the ground is expected to be (from the neighbouring cell nearer to V),
//     so it finds the street under V rather than the roof above it.
//   * A box about 1.4 m tall, sitting 0.7 m above that ground (higher than Minecraft's 0.6 m step height),
//     is tested for overlap with the world. If it touches anything, the cell is looked at again in 16
//     squares of 0.25 m, and each of those that touches something in 4 squares of 0.125 m. The small
//     squares that touch something are marked as obstacles: Minecraft makes them solid up to head height.
//     This catches walls, trees, lamp posts and the like, which vertical rays slip past, and the small
//     squares keep diagonal walls and thin poles from becoming big blocks.
//
// Everything goes into the shared ground grid for the Minecraft mod to read.

#include "Ground.hpp"
#include "Link.hpp"
#include "Mapping.hpp"

#include <cybercraft_protocol.h>

#include <RED4ext/Scripting/Natives/Generated/EulerAngles.hpp>
#include <RED4ext/Scripting/Natives/ScriptGameInstance.hpp>
#include <RED4ext/Scripting/Natives/physicsTraceResult.hpp>
#include <RED4ext/Scripting/Natives/Vector4.hpp>

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace cybercraft::ground
{
	namespace
	{
		// How much of each frame the scan may use. Cells are scanned until the budget runs out (but at least
		// kMinCells and at most kMaxCells). A faster PC gets through more cells in the same time.
		constexpr double kBudgetMs = 2.5;
		constexpr int kMinCells = 16;
		constexpr int kMaxCells = 800;

		// Collision groups to shoot the ground ray at. Roads, paths and buildings are in "Static"; the ground
		// itself (grass, dirt, sand) is in "Terrain". Every group is tried and the HIGHEST hit wins: a ray at
		// "Static" alone passes straight through the terrain and can land on something buried under it
		// (foundations, tunnels, pipes), which would put the ground below the real surface.
		constexpr const char* kGroups[] = { "Static", "Terrain" };
		constexpr int kGroupCount = sizeof(kGroups) / sizeof(kGroups[0]);
		constexpr double kRayUp = 1.6;        // start this far above the expected ground (below head height, so an awning doesn't count)
		constexpr double kRayDown = 4.0;      // and end this far below it
		constexpr double kRetryRange = 12.0;  // second try, if the first ray found nothing

		// Obstacles: groups tested, and the box tested in each cell (all relative to the ground at the cell's centre).
		constexpr const char* kObstacleGroups[] = { "Static" };
		constexpr int kObstacleGroupCount = sizeof(kObstacleGroups) / sizeof(kObstacleGroups[0]);
		constexpr double kObstacleBottom = 0.7;   // the box starts this far above the ground (Minecraft steps up 0.6)
		constexpr double kObstacleBoxHeight = 1.05;  // and ends 1.75 m above it: a ceiling above that doesn't stop you
		constexpr double kBodyRadius = 0.45;      // small squares this close to V's feet are never marked, so V is never walled in
		constexpr double kObstacleHalfWidth = 0.5;
		constexpr int kSub = proto::kObstacleSub;  // a blocked cell is looked at again in kSub x kSub small squares
		static_assert(kSub == 8, "BlockedSquares is written for 8 x 8");

		RED4ext::v1::PluginHandle g_handle = nullptr;
		const RED4ext::v1::Sdk* g_sdk = nullptr;

		// Lookups, made lazily the first time they are needed and dropped by Reset().
		bool g_triedInit = false;
		bool g_failed = false; // gave up: don't spam the log
		RED4ext::IScriptable* g_spatial = nullptr;
		RED4ext::CClassFunction* g_raycast = nullptr;
		RED4ext::CClassFunction* g_overlap = nullptr;
		RED4ext::physics::TraceResult g_trace{};  // the script type "TraceResult": the SDK has its exact layout
		bool g_loggedFirstHit = false;
		bool g_loggedExecFail = false;

		// Obstacle testing: off until the game's Overlap function has been checked against known cases.
		bool g_obstaclesOn = false;
		bool g_calibrated = false;
		double g_boxScale = 1.0;  // 1: Overlap takes half-widths, 2: it takes full widths
		const char* g_lastGroundGroup = nullptr;

		struct CellOffset
		{
			int dx, dz;
		};
		std::vector<CellOffset> g_order;  // every cell within the scan radius, nearest to V first
		std::size_t g_next = 0;

		// Timings, reported every few seconds.
		std::chrono::steady_clock::time_point g_lastReport{};
		std::uint64_t g_cells = 0, g_hits = 0, g_blocked = 0, g_frames = 0, g_subTests = 0;
		std::uint64_t g_groupHits[kGroupCount] = {};
		std::uint64_t g_lowerHits[kGroupCount] = {};  // times a group found something 0.3 m or more below the winner
		std::uint64_t g_obstacleHits[kObstacleGroupCount] = {};
		double g_scanMicros = 0.0;

		std::uint64_t PackWord(float a_value, std::int16_t a_bx, std::int16_t a_bz)
		{
			std::uint32_t bits;
			std::memcpy(&bits, &a_value, sizeof(bits));
			return std::uint64_t(bits) | (std::uint64_t(std::uint16_t(a_bx)) << 32) | (std::uint64_t(std::uint16_t(a_bz)) << 48);
		}

		// Word 0 of a cell's slot is the ground, word 1 says which cell the obstacle mask in word 2 is for.
		std::uint64_t* SlotWord(int a_bx, int a_bz, int a_word)
		{
			const int n = int(proto::kGroundN);
			const int ix = ((a_bx % n) + n) % n;
			const int iz = ((a_bz % n) + n) % n;
			auto* base = Link::Get().Base();
			return reinterpret_cast<std::uint64_t*>(base + proto::kOffGround) + (iz * n + ix) * 3 + a_word;
		}

		// The height stored for a cell, or NaN if the slot holds some other cell (or nothing).
		float StoredHeight(int a_bx, int a_bz)
		{
			const std::uint64_t value = std::atomic_ref<std::uint64_t>(*SlotWord(a_bx, a_bz, 0)).load(std::memory_order_acquire);
			const auto bx = std::int16_t(std::uint16_t(value >> 32));
			const auto bz = std::int16_t(std::uint16_t(value >> 48));
			if (bx != std::int16_t(a_bx) || bz != std::int16_t(a_bz)) {
				return std::nanf("");
			}
			const auto bits = std::uint32_t(value & 0xFFFFFFFFu);
			float height;
			std::memcpy(&height, &bits, sizeof(height));
			// Stored in Minecraft's height; the scanner works in Cyberpunk's.
			return height <= proto::kNoGround * 0.5f ? std::nanf("") : height + float(mapping::Offset());
		}

		void StoreHeight(int a_bx, int a_bz, float a_height)
		{
			// a_height is Cyberpunk's height (or the "no ground" marker, which is kept as it is).
			const float stored = a_height <= proto::kNoGround * 0.5f ? a_height : a_height - float(mapping::Offset());
			std::atomic_ref<std::uint64_t>(*SlotWord(a_bx, a_bz, 0))
				.store(PackWord(stored, std::int16_t(a_bx), std::int16_t(a_bz)), std::memory_order_release);
		}

		// The mask of blocked sub-squares (0: nothing in the way) goes in word 2, then word 1 says which cell it
		// is for, with a check value so a reader can tell if it caught the two half-way through an update.
		void StoreObstacle(int a_bx, int a_bz, std::uint64_t a_mask)
		{
			const std::uint64_t check = (a_mask ^ (a_mask >> 16) ^ (a_mask >> 32) ^ (a_mask >> 48)) & 0xFFFFu;
			const std::uint64_t id = check | (std::uint64_t(std::uint16_t(a_bx)) << 32) | (std::uint64_t(std::uint16_t(a_bz)) << 48);
			std::atomic_ref<std::uint64_t>(*SlotWord(a_bx, a_bz, 2)).store(a_mask, std::memory_order_release);
			std::atomic_ref<std::uint64_t>(*SlotWord(a_bx, a_bz, 1)).store(id, std::memory_order_release);
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

		void LogParameters(const char* a_name, RED4ext::CClassFunction* a_func)
		{
			g_sdk->logger->InfoF(g_handle, "ground: %s takes %u parameters:", a_name, static_cast<unsigned>(a_func->params.Size()));
			for (uint32_t i = 0; i < a_func->params.Size(); ++i) {
				auto* param = a_func->params[i];
				g_sdk->logger->InfoF(g_handle, "  - %s : %s%s", param->name.ToString(),
					param->type ? param->type->GetName().ToString() : "?", param->flags.isOut ? " (out)" : "");
			}
		}

		// Finds the game's spatial queries system and the functions on it.
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

			// What the game says the functions take, in case a guess here is wrong.
			LogParameters("raycast", g_raycast);

			g_overlap = type->GetFunction("Overlap");
			if (g_overlap) {
				LogParameters("overlap", g_overlap);
			} else {
				g_sdk->logger->Warn(g_handle, "ground: no Overlap function; walls, trees and poles won't be solid");
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

		// Shoots at every collision group and returns the highest hit's height, or NaN if none hit.
		float CastAllGroups(double a_x, double a_y, double a_zTop, double a_zBottom)
		{
			float hits[kGroupCount];
			float best = std::nanf("");
			int bestGroup = -1;
			for (int i = 0; i < kGroupCount; ++i) {
				hits[i] = Cast(kGroups[i], a_x, a_y, a_zTop, a_zBottom);
				if (!std::isnan(hits[i]) && (std::isnan(best) || hits[i] > best)) {
					best = hits[i];
					bestGroup = i;
				}
			}
			if (bestGroup >= 0) {
				++g_groupHits[bestGroup];
				g_lastGroundGroup = kGroups[bestGroup];
				for (int i = 0; i < kGroupCount; ++i) {
					if (!std::isnan(hits[i]) && best - hits[i] >= 0.3f) {
						++g_lowerHits[i];
					}
				}
			}
			return best;
		}

		// Does a box (half-widths a_hx, a_hy, a_hz, centred at a_x, a_y, a_z) touch anything in this group?
		bool Overlap(const char* a_group, double a_x, double a_y, double a_z, double a_hx, double a_hy, double a_hz)
		{
			RED4ext::Vector4 size(float(a_hx * g_boxScale), float(a_hy * g_boxScale), float(a_hz * g_boxScale), 0.0f);
			RED4ext::Vector4 position(float(a_x), float(a_y), float(a_z), 1.0f);
			RED4ext::EulerAngles rotation{ 0.0f, 0.0f, 0.0f };
			RED4ext::CName group(a_group);

			RED4ext::StackArgs_t args;
			args.emplace_back(nullptr, &size);
			args.emplace_back(nullptr, &position);
			args.emplace_back(nullptr, &rotation);
			args.emplace_back(nullptr, &group);
			std::memset(&g_trace, 0, sizeof(g_trace));
			args.emplace_back(nullptr, &g_trace);

			bool hit = false;
			return RED4ext::ExecuteFunction(g_spatial, g_overlap, &hit, args) && hit;
		}

		// Checks the game's Overlap function against two cases whose answers we know, once, using a spot
		// where a ray just found ground: a box that dips into the ground must touch it, and a box high in the
		// air must not. Using a thin box that is only 0.5 tall, centred 0.4 above the ground, also tells whether
		// the size is taken as half-widths (reaches down to 0.1 below the ground: touches) or full widths
		// (only reaches down to 0.15 above it: doesn't).
		void Calibrate(double a_x, double a_y, double a_ground)
		{
			g_calibrated = true;
			g_obstaclesOn = false;
			if (!g_overlap || !g_lastGroundGroup) {
				return;
			}

			g_boxScale = 1.0;
			const bool dipsIn = Overlap(g_lastGroundGroup, a_x, a_y, a_ground + 0.4, 0.05, 0.05, 0.5);
			const bool airHit = Overlap(g_lastGroundGroup, a_x, a_y, a_ground + 5.0, 0.05, 0.05, 0.05);
			g_sdk->logger->InfoF(g_handle, "ground: overlap check: box dipping into the ground = %d, box in the air = %d",
				dipsIn ? 1 : 0, airHit ? 1 : 0);
			if (airHit) {
				g_sdk->logger->Warn(g_handle, "ground: Overlap says a box in empty air is touching something, so it can't be trusted; obstacles are off");
				return;
			}

			if (dipsIn) {
				g_boxScale = 1.0;  // sizes are half-widths
			} else {
				g_boxScale = 2.0;  // sizes are full widths: pass twice as much
				if (!Overlap(g_lastGroundGroup, a_x, a_y, a_ground + 0.4, 0.05, 0.05, 0.5)) {
					g_sdk->logger->Warn(g_handle, "ground: Overlap never touches the ground, so it can't be trusted; obstacles are off");
					return;
				}
				g_sdk->logger->Info(g_handle, "ground: Overlap takes full widths");
			}
			g_obstaclesOn = true;
			g_sdk->logger->Info(g_handle, "ground: obstacles on");
		}

		// Does the box of the given half-width, standing on the ground at (a_cx, a_cy), touch anything?
		bool BoxBlocked(double a_cx, double a_cy, double a_ground, double a_halfWidth, bool a_count)
		{
			const double halfHeight = kObstacleBoxHeight * 0.5;
			const double centreZ = a_ground + kObstacleBottom + halfHeight;
			for (int i = 0; i < kObstacleGroupCount; ++i) {
				if (Overlap(kObstacleGroups[i], a_cx, a_cy, centreZ, a_halfWidth, a_halfWidth, halfHeight)) {
					if (a_count) {
						++g_obstacleHits[i];
					}
					return true;
				}
			}
			return false;
		}

		// For a cell whose whole box is blocked: which of its small squares are? Bit (sx + 8 * sz), with sx counted
		// along Minecraft's X and sz along its Z from the low corner of the cell. First the 16 quarter-metre
		// squares are tested; only the blocked ones are split into 4 squares of 0.125 m and tested again. Squares
		// within kBodyRadius of V's feet are never marked, so V can never be walled in.
		std::uint64_t BlockedSquares(int a_bx, int a_bz, double a_ground, double a_vx, double a_vy)
		{
			constexpr double kQuad = 1.0 / 4.0;
			constexpr double kFine = 1.0 / double(kSub);
			std::uint64_t mask = 0;
			bool skippedForBody = false;

			for (int qz = 0; qz < 4; ++qz) {
				for (int qx = 0; qx < 4; ++qx) {
					const double qxc = a_bx + (qx + 0.5) * kQuad;
					const double qyc = -(a_bz + (qz + 0.5) * kQuad);  // Cyberpunk Y is -Z
					++g_subTests;
					if (!BoxBlocked(qxc, qyc, a_ground, kQuad * 0.5, false)) {
						continue;
					}

					std::uint64_t quadMask = 0;
					std::uint64_t quadAll = 0;
					for (int cz = 0; cz < 2; ++cz) {
						for (int cx = 0; cx < 2; ++cx) {
							const int sx = qx * 2 + cx;
							const int sz = qz * 2 + cz;
							const double x = a_bx + (sx + 0.5) * kFine;
							const double y = -(a_bz + (sz + 0.5) * kFine);
							if (std::hypot(x - a_vx, y - a_vy) < kBodyRadius) {
								skippedForBody = true;
								continue;
							}
							const std::uint64_t bit = 1ull << (sx + kSub * sz);
							quadAll |= bit;
							++g_subTests;
							if (BoxBlocked(x, y, a_ground, kFine * 0.5, false)) {
								quadMask |= bit;
							}
						}
					}
					// The quarter-metre square touched something but none of its halves did (a thin edge between
					// them): stay safe and block the whole quarter.
					mask |= quadMask != 0 ? quadMask : quadAll;
				}
			}
			// The whole cell touched something but no quarter did: block everything except next to V.
			if (mask == 0 && !skippedForBody) {
				for (int sz = 0; sz < kSub; ++sz) {
					for (int sx = 0; sx < kSub; ++sx) {
						const double x = a_bx + (sx + 0.5) * kFine;
						const double y = -(a_bz + (sz + 0.5) * kFine);
						if (std::hypot(x - a_vx, y - a_vy) >= kBodyRadius) {
							mask |= 1ull << (sx + kSub * sz);
						}
					}
				}
			}
			return mask;
		}

		void ScanCell(int a_bx, int a_bz, int a_dx, int a_dz, double a_vx, double a_vy, double a_vz)
		{
			// Where do we expect the ground to be? Best guess first: the cell one step nearer to V (it is looked at
			// more often, and is on the same level as V), then this cell's own last height, then V's feet.
			// (This cell's old height can be stale: a bridge deck found earlier would otherwise keep being found
			// again after V has gone down to the road under it.)
			float expected = std::nanf("");
			if (a_dx == 0 && a_dz == 0) {
				expected = float(a_vz);
			} else {
				const int sx = (a_dx > 0) - (a_dx < 0);
				const int sz = (a_dz > 0) - (a_dz < 0);
				expected = StoredHeight(a_bx - sx, a_bz - sz);
			}
			const bool neighbourHadGround = !std::isnan(expected);
			if (std::isnan(expected)) {
				expected = StoredHeight(a_bx, a_bz);
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
			// Next to ground but found none: a ray can slip through a seam between two collision triangles. Try
			// again a little off-centre before calling the cell empty.
			if (std::isnan(height) && neighbourHadGround) {
				static const double kOffsets[4][2] = { { 0.3, 0.3 }, { -0.3, 0.3 }, { 0.3, -0.3 }, { -0.3, -0.3 } };
				for (const auto& o : kOffsets) {
					height = CastAllGroups(cx + o[0], cy + o[1], expected + kRayUp, expected - kRayDown);
					if (!std::isnan(height)) {
						break;
					}
				}
			}

			++g_cells;
			if (std::isnan(height)) {
				StoreHeight(a_bx, a_bz, proto::kNoGround);
				StoreObstacle(a_bx, a_bz, 0);
				return;
			}
			++g_hits;
			StoreHeight(a_bx, a_bz, height);

			if (g_overlap && !g_calibrated) {
				Calibrate(cx, cy, height);
			}

			// Anything in the way? (Including in V's own cell: a wall can be inside it. The small squares right
			// next to V's feet are left out in BlockedSquares.)
			std::uint64_t mask = 0;
			if (g_obstaclesOn && BoxBlocked(cx, cy, height, kObstacleHalfWidth, true)) {
				mask = BlockedSquares(a_bx, a_bz, height, a_vx, a_vy);
				++g_blocked;
			}
			StoreObstacle(a_bx, a_bz, mask);
		}
	}

	void Init(RED4ext::v1::PluginHandle a_handle, const RED4ext::v1::Sdk* a_sdk)
	{
		g_handle = a_handle;
		g_sdk = a_sdk;
	}

	double RayDistance(double a_ox, double a_oy, double a_oz, double a_dx, double a_dy, double a_dz, double a_maxDistance)
	{
		if (!Link::Get().IsOpen() || g_failed || !g_triedInit) {
			return std::nan("");
		}
		const double length = std::sqrt(a_dx * a_dx + a_dy * a_dy + a_dz * a_dz);
		if (length < 1.0e-9) {
			return std::nan("");
		}
		const double ux = a_dx / length;
		const double uy = a_dy / length;
		const double uz = a_dz / length;
		double best = std::nan("");
		for (int i = 0; i < kGroupCount; ++i) {
			RED4ext::Vector4 from(float(a_ox), float(a_oy), float(a_oz), 1.0f);
			RED4ext::Vector4 to(float(a_ox + ux * a_maxDistance), float(a_oy + uy * a_maxDistance), float(a_oz + uz * a_maxDistance), 1.0f);
			RED4ext::CName group(kGroups[i]);
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
			if (!RED4ext::ExecuteFunction(g_spatial, g_raycast, &hit, args) || !hit) {
				continue;
			}
			const double hx = g_trace.position.X - a_ox;
			const double hy = g_trace.position.Y - a_oy;
			const double hz = g_trace.position.Z - a_oz;
			const double distance = std::sqrt(hx * hx + hy * hy + hz * hz);
			if (std::isnan(best) || distance < best) {
				best = distance;
			}
		}
		return best;
	}

	double HeightAt(double a_x, double a_y)
	{
		if (!Link::Get().IsOpen()) {
			return std::nan("");
		}
		const float h = StoredHeight(int(std::floor(a_x)), int(std::floor(-a_y)));
		return std::isnan(h) ? std::nan("") : double(h);
	}

	void OffsetChanged(double a_oldOffset, double a_newOffset)
	{
		if (!Link::Get().IsOpen()) {
			return;
		}
		// Heights are stored as (Cyberpunk Z - offset): with a new offset every stored value moves by the difference.
		const float delta = float(a_oldOffset - a_newOffset);
		auto* words = reinterpret_cast<std::uint64_t*>(Link::Get().Base() + proto::kOffGround);
		for (std::uint32_t i = 0; i < proto::kGroundN * proto::kGroundN; ++i) {
			std::uint64_t& word = words[i * 3];
			const auto bits = std::uint32_t(word & 0xFFFFFFFFu);
			float height;
			std::memcpy(&height, &bits, sizeof(height));
			if (height <= proto::kNoGround * 0.5f) {
				continue;
			}
			height += delta;
			std::uint32_t newBits;
			std::memcpy(&newBits, &height, sizeof(newBits));
			word = (word & 0xFFFFFFFF00000000ull) | newBits;
		}
	}

	void Reset()
	{
		// Cached game objects are only valid inside one loaded world.
		g_spatial = nullptr;
		g_raycast = nullptr;
		g_overlap = nullptr;
		g_triedInit = false;
		g_failed = false;
		g_calibrated = false;
		g_obstaclesOn = false;
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
		int done = 0;
		while (done < kMaxCells) {
			const CellOffset& o = g_order[g_next];
			g_next = (g_next + 1) % g_order.size();
			ScanCell(centreBx + o.dx, centreBz + o.dz, o.dx, o.dz, a_vx, a_vy, a_vz);
			++done;
			if (done >= kMinCells && std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() > kBudgetMs) {
				break;
			}
		}
		const auto end = std::chrono::steady_clock::now();
		g_scanMicros += std::chrono::duration<double, std::micro>(end - start).count();
		++g_frames;

		if (end - g_lastReport >= std::chrono::seconds(5)) {
			g_lastReport = end;
			if (g_cells > 0) {
				g_sdk->logger->InfoF(g_handle, "ground: %llu cells in 5 s (%llu per frame), %.0f%% found ground, %.0f%% blocked, %.0f us per frame, %.1f us per cell",
					static_cast<unsigned long long>(g_cells), static_cast<unsigned long long>(g_cells / std::max<std::uint64_t>(1, g_frames)),
					100.0 * double(g_hits) / double(g_cells), 100.0 * double(g_blocked) / double(std::max<std::uint64_t>(1, g_hits)),
					g_scanMicros / double(std::max<std::uint64_t>(1, g_frames)), g_scanMicros / double(g_cells));
				g_sdk->logger->InfoF(g_handle, "  small-square tests: %llu", static_cast<unsigned long long>(g_subTests));
				for (int i = 0; i < kGroupCount; ++i) {
					g_sdk->logger->InfoF(g_handle, "  group \"%s\": was the highest hit %llu times; also found something 0.3 m or more BELOW the ground %llu times",
						kGroups[i], static_cast<unsigned long long>(g_groupHits[i]), static_cast<unsigned long long>(g_lowerHits[i]));
				}
				for (int i = 0; i < kObstacleGroupCount; ++i) {
					g_sdk->logger->InfoF(g_handle, "  obstacles found in group \"%s\": %llu", kObstacleGroups[i], static_cast<unsigned long long>(g_obstacleHits[i]));
				}
			}
			g_cells = g_hits = g_blocked = g_frames = g_subTests = 0;
			std::fill(std::begin(g_groupHits), std::end(g_groupHits), 0ull);
			std::fill(std::begin(g_lowerHits), std::end(g_lowerHits), 0ull);
			std::fill(std::begin(g_obstacleHits), std::end(g_obstacleHits), 0ull);
			g_scanMicros = 0.0;
		}
	}
}
