// Collision for what is built in Minecraft (see Collision.hpp).
//
// Night City is not made of blocks, so the blocks you build are only a picture: cars would drive through them. Minecraft publishes the complete
// list of boxes of what is built near the player (see BoxTableHdr in the protocol), and this file keeps invisible collision boxes in Night City
// exactly there: boxes that are new get spawned, boxes that are gone get removed, a few per frame.
//
// A box is an empty entity with a collider component. The component can only be added while the game is setting the entity up, so the spawning
// is done by a small script (cyberpunk\scripts\Colliders.reds, installed as <game>\r6\scripts\CyberCraft\Colliders.reds) that hooks that moment
// through Codeware's callbacks; this file asks it for each box (CyberCraftColliders.SpawnBox) and removes boxes through Codeware's
// StaticEntitySystem.
//
// Nothing is remembered between sessions: the list is rebuilt from Minecraft's world each time, so each world only ever puts up its own boxes.
//
// What is known about NPCs: they don't collide with these boxes. Cars and V do. NPCs seem to move through the game's navigation data and a
// separate kind of physical shape ("character obstacle"), not through ordinary collider components, and none of the things we tried (the collision
// numbers, collision presets, "collide with everything", the runtime navigation obstacle API) changed that; World Builder's live boxes behave the
// same way. See the README.

#include "Collision.hpp"
#include "Link.hpp"
#include "Log.hpp"
#include "Mapping.hpp"

#include "../../protocol/cybercraft_protocol.h"

#include <RED4ext/Scripting/Natives/ScriptGameInstance.hpp>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <vector>

namespace cybercraft::collision
{
	namespace
	{
		RED4ext::v1::PluginHandle g_handle = nullptr;
		const RED4ext::v1::Sdk* g_sdk = nullptr;

		// ---- finding things in the game --------------------------------------------------------------------------------------------

		// One of Codeware's systems (StaticEntitySystem), if Codeware is installed.
		RED4ext::IScriptable* FindSystem(const char* a_class, const char* a_getter)
		{
			auto rtti = RED4ext::CRTTISystem::Get();
			auto* cls = rtti->GetClass(a_class);
			if (!cls) {
				return nullptr;
			}
			if (auto sgi = rtti->GetClass("ScriptGameInstance")) {
				if (auto func = sgi->GetFunction(a_getter)) {
					RED4ext::ScriptGameInstance game;
					RED4ext::Handle<RED4ext::IScriptable> system;
					RED4ext::StackArgs_t args;
					args.emplace_back(nullptr, &game);
					RED4ext::ExecuteFunction(static_cast<void*>(nullptr), func, &system, args);
					if (system) {
						return system.instance;
					}
				}
			}
			auto engine = RED4ext::CGameEngine::Get();
			if (engine && engine->framework && engine->framework->gameInstance) {
				return engine->framework->gameInstance->GetSystem(cls);
			}
			return nullptr;
		}

		RED4ext::IScriptable* StaticSystem()
		{
			return FindSystem("StaticEntitySystem", "GetStaticEntitySystem");
		}

		// Calls a function of the entity system that takes an entity ID and answers yes or no.
		bool CallBool(RED4ext::IScriptable* a_system, const char* a_function, std::uint64_t a_id, bool& a_result)
		{
			auto* fn = a_system->GetType()->GetFunction(a_function);
			if (!fn) {
				return false;
			}
			RED4ext::StackArgs_t args;
			args.emplace_back(nullptr, &a_id);
			a_result = false;
			return RED4ext::ExecuteFunction(a_system, fn, &a_result, args);
		}

		bool ContainsNoCase(const std::string& a_text, const std::string& a_word)
		{
			auto lower = [](std::string s) {
				for (char& c : s) {
					c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
				}
				return s;
			};
			return lower(a_text).find(lower(a_word)) != std::string::npos;
		}

		// The static functions of a script class are not in the class: the game keeps them among its global functions, as "Class::Function;parameters".
		RED4ext::CBaseFunction* FindStatic(const char* a_class, const char* a_name)
		{
			RED4ext::DynArray<RED4ext::CBaseFunction*> all;
			RED4ext::CRTTISystem::Get()->GetGlobalFunctions(all);
			const RED4ext::CName wanted(a_name);
			for (uint32_t i = 0; i < all.Size(); ++i) {
				if (all[i]->shortName == wanted && std::string(all[i]->fullName.ToString()).find(a_class) != std::string::npos) {
					return all[i];
				}
			}
			return nullptr;
		}

		// The script's SpawnBox(x, y, z, halfX, halfY, halfZ) -> entity id, looked up once.
		RED4ext::CBaseFunction* g_spawnFn = nullptr;
		bool g_spawnMissingReported = false;

		RED4ext::CBaseFunction* SpawnFunction()
		{
			if (!g_spawnFn) {
				g_spawnFn = FindStatic("CyberCraftColliders", "SpawnBox");
				if (!g_spawnFn && !g_spawnMissingReported) {
					g_spawnMissingReported = true;
					g_sdk->logger->Error(g_handle,
						"collision: the collider script isn't working, so there are no collision boxes. Check that <game>\\r6\\scripts\\CyberCraft\\Colliders.reds exists (the build copies it), "
						"that Codeware is installed, and look for errors in r6\\logs\\redscript_rCURRENT.log");
				}
			}
			return g_spawnFn;
		}

		// Asks the script for one box (centre and half sizes in metres, in the game's coordinates). Returns the entity's ID, or 0.
		std::uint64_t SpawnBox(RED4ext::CBaseFunction* a_fn, float a_x, float a_y, float a_z, float a_hx, float a_hy, float a_hz)
		{
			float values[6] = { a_x, a_y, a_z, a_hx, a_hy, a_hz };
			RED4ext::StackArgs_t args;
			for (float& v : values) {
				args.emplace_back(nullptr, &v);
			}
			std::uint64_t id = 0;
			const bool ran = RED4ext::ExecuteFunction(static_cast<void*>(nullptr), a_fn, &id, args);
			return ran ? id : 0;
		}

		// ---- the list of boxes from Minecraft ---------------------------------------------------------------------------------------

		struct BoxKey
		{
			std::int32_t v[6];  // min x, y, z, max x, y, z in sixteenths of a block, Minecraft coordinates
			bool operator==(const BoxKey& o) const { return std::memcmp(v, o.v, sizeof(v)) == 0; }
		};
		struct BoxKeyHash
		{
			std::size_t operator()(const BoxKey& k) const
			{
				std::uint64_t h = 1469598103934665603ull;
				for (int i = 0; i < 6; ++i) {
					h = (h ^ static_cast<std::uint32_t>(k.v[i])) * 1099511628211ull;
				}
				return static_cast<std::size_t>(h);
			}
		};

		std::unordered_map<BoxKey, std::uint64_t, BoxKeyHash> g_live;  // box -> its entity (0 until it has been spawned)
		std::deque<BoxKey> g_toAdd;
		std::deque<std::uint64_t> g_toRemove;
		std::uint32_t g_seenVersion = 0xFFFFFFFFu;
		std::uint32_t g_seenEpoch = 0xFFFFFFFFu;
		bool g_seenWanted = false;
		double g_liveOffset = 0.0;
		std::size_t g_lastReportedLive = static_cast<std::size_t>(-1);
		std::chrono::steady_clock::time_point g_lastReport{};
		constexpr int kAddsPerFrame = 10;
		constexpr int kRemovesPerFrame = 40;

		// Where a box is in the game: the centre and the half sizes in metres. Minecraft's x is the game's x, Minecraft's z is minus the game's y,
		// and Minecraft's y plus the vertical offset is the game's z.
		void BoxToGame(const BoxKey& k, double a_offset, float& x, float& y, float& z, float& hx, float& hy, float& hz)
		{
			x = static_cast<float>((k.v[0] + k.v[3]) / 32.0);
			hx = static_cast<float>((k.v[3] - k.v[0]) / 32.0);
			y = static_cast<float>(-(k.v[2] + k.v[5]) / 32.0);
			hy = static_cast<float>((k.v[5] - k.v[2]) / 32.0);
			z = static_cast<float>((k.v[1] + k.v[4]) / 32.0 + a_offset);
			hz = static_cast<float>((k.v[4] - k.v[1]) / 32.0);
		}

		// Reads Minecraft's list. Returns false if there is no consistent copy right now. a_entries is only filled when a_wantEntries.
		bool ReadBoxTable(bool a_wantEntries, std::uint32_t& a_count, std::uint32_t& a_version, std::uint32_t& a_flags, std::uint32_t& a_epoch, std::vector<BoxKey>& a_entries)
		{
			auto& link = Link::Get();
			if (!link.IsOpen() || !link.Base()) {
				return false;
			}
			auto* table = link.Base() + proto::kOffBoxTable;
			auto* hdr = reinterpret_cast<volatile proto::BoxTableHdr*>(table);
			for (int attempt = 0; attempt < 4; ++attempt) {
				const std::uint32_t seq1 = hdr->seq;
				if (seq1 & 1u) {
					continue;
				}
				std::atomic_thread_fence(std::memory_order_acquire);
				const std::uint32_t rawCount = hdr->count;
				a_count = rawCount < proto::kBoxTableMax ? rawCount : proto::kBoxTableMax;
				a_version = hdr->version;
				a_flags = hdr->flags;
				a_epoch = hdr->epoch;
				if (a_wantEntries) {
					a_entries.resize(a_count);
					if (a_count) {
						std::memcpy(a_entries.data(), table + proto::kBoxTableEntriesOff, a_count * sizeof(BoxKey));
					}
				}
				std::atomic_thread_fence(std::memory_order_acquire);
				if (hdr->seq == seq1) {
					return true;
				}
			}
			return false;
		}

		void SyncBoxes(std::chrono::steady_clock::time_point a_now)
		{
			auto& link = Link::Get();
			const bool mcAlive = link.IsOpen() && link.McPid() != 0 && (::GetTickCount64() - link.McHeartbeatMs()) < 3000;
			std::uint32_t count = 0, version = 0, flags = 0, epoch = 0;
			std::vector<BoxKey> entries;
			bool wanted = false;
			if (mcAlive && ReadBoxTable(false, count, version, flags, epoch, entries)) {
				wanted = (flags & proto::kBoxesEnabled) != 0;
			} else if (mcAlive) {
				return;  // no consistent copy this frame: try again next frame
			} else {
				version = 0xFFFFFFFEu;  // Minecraft is gone: want nothing
			}

			const double offset = mapping::Offset();
			const bool offsetMoved = !g_live.empty() && std::abs(offset - g_liveOffset) > 0.001;
			const bool changed = version != g_seenVersion || epoch != g_seenEpoch || wanted != g_seenWanted || offsetMoved;
			if (changed) {
				std::unordered_set<BoxKey, BoxKeyHash> desired;
				if (wanted && mcAlive) {
					std::uint32_t c2 = 0, v2 = 0, f2 = 0, e2 = 0;
					if (!ReadBoxTable(true, c2, v2, f2, e2, entries)) {
						return;
					}
					version = v2;
					epoch = e2;
					for (const BoxKey& k : entries) {
						desired.insert(k);
					}
				}
				const bool everything = epoch != g_seenEpoch && g_seenEpoch != 0xFFFFFFFFu;
				// Boxes that are no longer wanted (or all of them, if everything is to be rebuilt or the vertical offset moved).
				for (auto it = g_live.begin(); it != g_live.end();) {
					if (everything || offsetMoved || desired.find(it->first) == desired.end()) {
						if (it->second != 0) {
							g_toRemove.push_back(it->second);
						}
						it = g_live.erase(it);
					} else {
						++it;
					}
				}
				// Boxes that are wanted and don't exist yet.
				g_toAdd.clear();
				for (const BoxKey& k : desired) {
					if (g_live.find(k) == g_live.end()) {
						g_live.emplace(k, 0);
						g_toAdd.push_back(k);
					}
				}
				g_seenVersion = version;
				g_seenEpoch = epoch;
				g_seenWanted = wanted;
				g_liveOffset = offset;
			}

			// A few removals and additions per frame.
			if (!g_toRemove.empty()) {
				if (auto* system = StaticSystem()) {
					for (int i = 0; i < kRemovesPerFrame && !g_toRemove.empty(); ++i) {
						bool result = false;
						CallBool(system, "DespawnEntity", g_toRemove.front(), result);
						g_toRemove.pop_front();
					}
				}
			}
			if (!g_toAdd.empty()) {
				auto* spawnFn = SpawnFunction();
				if (!spawnFn) {
					g_toAdd.clear();
				}
				for (int i = 0; i < kAddsPerFrame && !g_toAdd.empty() && spawnFn; ++i) {
					const BoxKey k = g_toAdd.front();
					g_toAdd.pop_front();
					auto it = g_live.find(k);
					if (it == g_live.end()) {
						continue;  // went away again before it was made
					}
					float x, y, z, hx, hy, hz;
					BoxToGame(k, g_liveOffset, x, y, z, hx, hy, hz);
					it->second = SpawnBox(spawnFn, x, y, z, hx, hy, hz);
				}
			}

			// One line when the number of boxes has settled on something new (not on a timer, so an idle game writes nothing).
			if (g_toAdd.empty() && g_toRemove.empty() && g_live.size() != g_lastReportedLive && a_now - g_lastReport > std::chrono::seconds(3)) {
				g_lastReport = a_now;
				g_lastReportedLive = g_live.size();
				g_sdk->logger->InfoF(g_handle, "collision: %u collision boxes in Night City", static_cast<unsigned>(g_live.size()));
			}
		}

		// ---- discovery tools (/ccdebug) ---------------------------------------------------------------------------------------------

		// Writes the properties and functions of a class to the log.
		void DumpClass(const char* a_name)
		{
			auto* cls = RED4ext::CRTTISystem::Get()->GetClass(a_name);
			if (!cls) {
				g_sdk->logger->InfoF(g_handle, "collision: class %s: not found", a_name);
				return;
			}
			std::string chain;
			for (auto* c = cls; c; c = c->parent) {
				chain += c->name.ToString();
				if (c->parent) {
					chain += " < ";
				}
			}
			g_sdk->logger->InfoF(g_handle, "collision: class %s (%s)", a_name, chain.c_str());
			int lines = 0;
			for (auto* c = cls; c && lines < 90; c = c->parent) {
				for (uint32_t i = 0; i < c->props.Size() && lines < 90; ++i) {
					auto* prop = c->props[i];
					g_sdk->logger->InfoF(g_handle, "    prop %s::%s : %s", c->name.ToString(), prop->name.ToString(), prop->type ? prop->type->GetName().ToString() : "?");
					++lines;
				}
				for (uint32_t i = 0; i < c->funcs.Size() && lines < 90; ++i) {
					auto* func = c->funcs[i];
					std::string params;
					for (uint32_t p = 0; p < func->params.Size(); ++p) {
						auto* param = func->params[p];
						if (p) {
							params += ", ";
						}
						params += param->type ? param->type->GetName().ToString() : "?";
						params += " ";
						params += param->name.ToString();
					}
					const char* ret = func->returnType && func->returnType->type ? func->returnType->type->GetName().ToString() : "void";
					g_sdk->logger->InfoF(g_handle, "    fn   %s::%s(%s) -> %s", c->name.ToString(), func->shortName.ToString(), params.c_str(), ret);
					++lines;
				}
			}
		}

		// The name of a type, for a parameter, a return value or a field ("?" if the type isn't known).
		std::string TypeName(RED4ext::CProperty* a_property)
		{
			if (!a_property || !a_property->type) {
				return "Void";
			}
			return a_property->type->GetName().ToString();
		}

		std::string Signature(RED4ext::CBaseFunction* a_function)
		{
			std::string text = a_function->shortName.ToString();
			text += "(";
			for (uint32_t i = 0; i < a_function->params.Size(); ++i) {
				RED4ext::CProperty* p = a_function->params[i];
				if (i) {
					text += ", ";
				}
				text += std::string(p->name.ToString()) + ": " + TypeName(p);
			}
			text += ") -> " + TypeName(a_function->returnType);
			return text;
		}

		// Writes to the log the methods and fields of the classes whose name contains the word (the shortest names first, at most four classes): what a script can call on them.
		void DumpClasses(const std::string& a_word)
		{
			if (a_word.empty()) {
				return;
			}
			auto rtti = RED4ext::CRTTISystem::Get();
			RED4ext::DynArray<RED4ext::CClass*> classes;
			rtti->GetClasses(nullptr, classes, nullptr, true);
			std::vector<RED4ext::CClass*> matches;
			for (uint32_t i = 0; i < classes.Size(); ++i) {
				if (ContainsNoCase(classes[i]->name.ToString(), a_word)) {
					matches.push_back(classes[i]);
				}
			}
			std::sort(matches.begin(), matches.end(), [](RED4ext::CClass* a, RED4ext::CClass* b) {
				return std::string(a->name.ToString()).size() < std::string(b->name.ToString()).size();
			});
			g_sdk->logger->InfoF(g_handle, "debug: class \"%s\": %u classes match; the %u shortest names are described:", a_word.c_str(), static_cast<unsigned>(matches.size()), static_cast<unsigned>(std::min<std::size_t>(matches.size(), 4)));
			for (std::size_t m = 0; m < matches.size() && m < 4; ++m) {
				RED4ext::CClass* c = matches[m];
				std::string chain;
				for (auto* p = c->parent; p; p = p->parent) {
					chain += " < ";
					chain += p->name.ToString();
				}
				g_sdk->logger->InfoF(g_handle, "  class %s%s", c->name.ToString(), chain.c_str());
				for (uint32_t i = 0; i < c->funcs.Size() && i < 150; ++i) {
					g_sdk->logger->InfoF(g_handle, "      method %s", Signature(c->funcs[i]).c_str());
				}
				for (uint32_t i = 0; i < c->staticFuncs.Size() && i < 80; ++i) {
					g_sdk->logger->InfoF(g_handle, "      static %s", Signature(c->staticFuncs[i]).c_str());
				}
				for (uint32_t i = 0; i < c->props.Size() && i < 80; ++i) {
					g_sdk->logger->InfoF(g_handle, "      field %s: %s", c->props[i]->name.ToString(), TypeName(c->props[i]).c_str());
				}
			}
			g_sdk->logger->Info(g_handle, "debug: class: done");
		}

		// Writes to the log every class, enum and global function whose name contains the word.
		void Find(const std::string& a_word)
		{
			if (a_word.empty()) {
				return;
			}
			auto rtti = RED4ext::CRTTISystem::Get();
			g_sdk->logger->InfoF(g_handle, "debug: find \"%s\":", a_word.c_str());
			{
				RED4ext::DynArray<RED4ext::CClass*> classes;
				rtti->GetClasses(nullptr, classes, nullptr, true);
				unsigned shown = 0, total = 0;
				for (uint32_t i = 0; i < classes.Size(); ++i) {
					const std::string name = classes[i]->name.ToString();
					if (!ContainsNoCase(name, a_word)) {
						continue;
					}
					++total;
					if (shown < 80) {
						std::string chain;
						for (auto* p = classes[i]->parent; p; p = p->parent) {
							chain += " < ";
							chain += p->name.ToString();
						}
						g_sdk->logger->InfoF(g_handle, "    class %s%s (%u properties)", name.c_str(), chain.c_str(), static_cast<unsigned>(classes[i]->props.Size()));
						++shown;
					}
				}
				g_sdk->logger->InfoF(g_handle, "debug: %u classes match (%u shown)", total, shown);
			}
			{
				RED4ext::DynArray<RED4ext::CEnum*> enums;
				rtti->GetEnums(enums, false);
				unsigned total = 0;
				for (uint32_t i = 0; i < enums.Size(); ++i) {
					const std::string name = enums[i]->name.ToString();
					if (!ContainsNoCase(name, a_word)) {
						continue;
					}
					++total;
					std::string values;
					for (uint32_t v = 0; v < enums[i]->hashList.Size() && v < 70; ++v) {
						values += std::string(enums[i]->hashList[v].ToString()) + "=" + std::to_string(enums[i]->valueList[v]) + " ";
					}
					g_sdk->logger->InfoF(g_handle, "    enum %s: %s", name.c_str(), values.c_str());
				}
				g_sdk->logger->InfoF(g_handle, "debug: %u enums match", total);
			}
			{
				RED4ext::DynArray<RED4ext::CBaseFunction*> functions;
				rtti->GetGlobalFunctions(functions);
				unsigned shown = 0, total = 0;
				for (uint32_t i = 0; i < functions.Size(); ++i) {
					const std::string full = functions[i]->fullName.ToString();
					if (!ContainsNoCase(full, a_word)) {
						continue;
					}
					++total;
					if (shown < 60) {
						g_sdk->logger->InfoF(g_handle, "    global function %s", full.c_str());
						++shown;
					}
				}
				g_sdk->logger->InfoF(g_handle, "debug: %u global functions match (%u shown)", total, shown);
			}
		}

		// The classes behind the collision boxes, for checking after a game update.
		void DumpCollisionClasses()
		{
			g_sdk->logger->Info(g_handle, "debug: the classes behind the collision boxes:");
			static const char* const kClasses[] = { "StaticEntitySystem", "StaticEntitySpec", "entEntity", "entColliderComponent", "physicsColliderBox", "physicsFilterData", "physicsQueryFilter",
				"physicsSimulationFilter", "CyberCraftColliders", "CyberCraftColliderService" };
			for (const char* name : kClasses) {
				DumpClass(name);
			}
			RED4ext::DynArray<RED4ext::CBaseFunction*> all;
			RED4ext::CRTTISystem::Get()->GetGlobalFunctions(all);
			for (uint32_t i = 0; i < all.Size(); ++i) {
				const std::string full = all[i]->fullName.ToString();
				if (full.find("CyberCraft") != std::string::npos) {
					g_sdk->logger->InfoF(g_handle, "    global function %s", full.c_str());
				}
			}
		}
	}

	void Init(RED4ext::v1::PluginHandle a_handle, const RED4ext::v1::Sdk* a_sdk)
	{
		g_handle = a_handle;
		g_sdk = a_sdk;
	}

	void Command(int a_action, double a_arg, double a_arg2)
	{
		if (a_action == proto::kDebugDump) {
			DumpCollisionClasses();
		} else if (a_action == proto::kDebugLog) {
			log::SetVerbose(a_arg != 0.0);
			g_sdk->logger->InfoF(g_handle, "debug: detailed logging %s", log::Verbose() ? "ON (statistics every few seconds)" : "off");
		} else if (a_action == proto::kDebugFind || a_action == proto::kDebugClass) {
			// The word to look for arrives as up to six letters packed into the number.
			std::string word;
			const std::uint64_t packed = static_cast<std::uint64_t>(a_arg);
			for (int i = 0; i < 6; ++i) {
				const char ch = static_cast<char>((packed >> (8 * i)) & 0x7F);
				if (!ch) {
					break;
				}
				word += ch;
			}
			if (a_action == proto::kDebugClass && word.size() == 6) {
				// /ccdebug class takes up to twelve letters: the next six arrive in the second argument.
				const std::uint64_t packed2 = static_cast<std::uint64_t>(a_arg2);
				for (int i = 0; i < 6; ++i) {
					const char ch = static_cast<char>((packed2 >> (8 * i)) & 0x7F);
					if (!ch) {
						break;
					}
					word += ch;
				}
			}
			if (a_action == proto::kDebugClass) {
				DumpClasses(word);
			} else {
				Find(word);
			}
		}
	}

	void Update()
	{
		SyncBoxes(std::chrono::steady_clock::now());
	}

	void Reset()
	{
		// The game has no player (a loading screen or the main menu): every entity we made is gone with the world, so forget them without asking
		// for their removal; the boxes are made again from Minecraft's list when there is a player.
		g_live.clear();
		g_toAdd.clear();
		g_toRemove.clear();
		g_seenVersion = 0xFFFFFFFFu;
		g_seenEpoch = 0xFFFFFFFFu;
		g_seenWanted = false;
		g_lastReportedLive = static_cast<std::size_t>(-1);
	}
}
