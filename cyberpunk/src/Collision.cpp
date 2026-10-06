// The collision experiment (see Collision.hpp).
//
// Night City is not made of blocks, so the blocks you build are only a picture: cars drive through them and people walk through
// them. To stop them the game needs a physical object where each block (or, better, each merged box of blocks) is.
//
// The collider is made by a small script (cyberpunk\scripts\Colliders.reds, installed in the game's r6\scripts\CyberCraft folder), because a
// component can only be added to an entity while the game is setting it up, and the script can hook that moment through Codeware's
// callbacks. This file asks the script for a box (CyberCraftSpawnBox), removes the boxes (CyberCraftClearBoxes), and, a moment
// after a spawn, writes down in the log what the new entity contains.

#include "Collision.hpp"
#include "Camera.hpp"
#include "Ground.hpp"
#include "Mapping.hpp"

#include <RED4ext/Scripting/Natives/ScriptGameInstance.hpp>
#include <RED4ext/Scripting/Natives/Generated/WorldTransform.hpp>
#include <RED4ext/Scripting/Natives/Transform.hpp>
#include <RED4ext/Scripting/Natives/Vector3.hpp>
#include <RED4ext/Scripting/Natives/Vector4.hpp>
#include <RED4ext/Scripting/Natives/Quaternion.hpp>
#include <RED4ext/ResourcePath.hpp>

#include <chrono>
#include <cmath>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace cybercraft::collision
{
	namespace
	{
		RED4ext::v1::PluginHandle g_handle = nullptr;
		const RED4ext::v1::Sdk* g_sdk = nullptr;

		struct Spawned
		{
			std::uint64_t id = 0;
			std::chrono::steady_clock::time_point at{};
			bool inspected = false;
		};
		std::vector<Spawned> g_spawned;

		// Every step of the risky parts is written to collision-trace.txt (next to the plugin) and flushed at once, so if the game crashes
		// the last line says where.
		void Trace(const char* a_text)
		{
			static bool first = true;
			std::ofstream file(mapping::PluginFolder() / "collision-trace.txt", first ? std::ios::trunc : std::ios::app);
			first = false;
			file << a_text << std::endl;
			g_sdk->logger->InfoF(g_handle, "collision: step: %s", a_text);
		}

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

		// One of Codeware's systems (StaticEntitySystem: spawn an entity detached, change it, attach it), if Codeware is installed.
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


		// Every global function whose name mentions "CyberCraft" (the game keeps a script class's static functions there, not in the class).
		void ListOurGlobalFunctions()
		{
			RED4ext::DynArray<RED4ext::CBaseFunction*> all;
			RED4ext::CRTTISystem::Get()->GetGlobalFunctions(all);
			unsigned found = 0;
			for (uint32_t i = 0; i < all.Size(); ++i) {
				const std::string full = all[i]->fullName.ToString();
				const std::string shortName = all[i]->shortName.ToString();
				if (full.find("CyberCraft") != std::string::npos || shortName.find("CyberCraft") != std::string::npos) {
					g_sdk->logger->InfoF(g_handle, "    global function: short \"%s\", full \"%s\"", shortName.c_str(), full.c_str());
					++found;
				}
			}
			g_sdk->logger->InfoF(g_handle, "collision: %u of the game's %u global functions are ours", found, static_cast<unsigned>(all.Size()));
		}

		// A static function of a script class, by its short name. Says in the log what is there if it isn't found.
		RED4ext::CBaseFunction* FindStatic(const char* a_class, const char* a_name)
		{
			auto* cls = RED4ext::CRTTISystem::Get()->GetClass(a_class);
			if (!cls) {
				g_sdk->logger->ErrorF(g_handle, "collision: the game has no class %s: the script didn't make it into the game (see the redscript log)", a_class);
				return nullptr;
			}
			const RED4ext::CName wanted(a_name);
			for (uint32_t i = 0; i < cls->staticFuncs.Size(); ++i) {
				if (cls->staticFuncs[i]->shortName == wanted) {
					return cls->staticFuncs[i];
				}
			}
			for (uint32_t i = 0; i < cls->funcs.Size(); ++i) {
				if (cls->funcs[i]->shortName == wanted) {
					return cls->funcs[i];
				}
			}
			// Not in the class itself: look among the global functions for one called Class::Name.
			{
				RED4ext::DynArray<RED4ext::CBaseFunction*> all;
				RED4ext::CRTTISystem::Get()->GetGlobalFunctions(all);
				for (uint32_t i = 0; i < all.Size(); ++i) {
					if (all[i]->shortName == wanted) {
						const std::string full = all[i]->fullName.ToString();
						if (full.find(a_class) != std::string::npos) {
							g_sdk->logger->InfoF(g_handle, "collision: found %s::%s among the global functions (full name \"%s\")", a_class, a_name, full.c_str());
							return all[i];
						}
					}
				}
			}
			ListOurGlobalFunctions();
			std::string have;
			for (uint32_t i = 0; i < cls->staticFuncs.Size(); ++i) {
				have += std::string(cls->staticFuncs[i]->shortName.ToString()) + " ";
			}
			g_sdk->logger->ErrorF(g_handle, "collision: class %s has no function %s (its static functions: %s)", a_class, a_name, have.c_str());
			return nullptr;
		}

		void Spawn()
		{
			auto* spawnFn = FindStatic("CyberCraftColliders", "SpawnBox");
			if (!spawnFn) {
				g_sdk->logger->Error(g_handle, "collision: the collider script isn't working. Copy Colliders.reds (from cyberpunk\\scripts in the CyberCraft folder) to <game>\\r6\\scripts\\CyberCraft\\ and start the game again; if it is there, look in red4ext\\logs\\redscript_rCURRENT.log (or r6\\logs) for errors in it.");
				return;
			}
			double px, py, pz, fx, fy, fz;
			if (!camera::LastPose(px, py, pz, fx, fy, fz)) {
				g_sdk->logger->Warn(g_handle, "collision: no camera yet");
				return;
			}
			// A 2 x 2 x 2 metre box on the block grid, three metres ahead of the camera, standing on the street there: its corners are on
			// whole block boundaries, so a 2 x 2 x 2 build of blocks fills it exactly.
			const double flat = std::sqrt(fx * fx + fy * fy);
			const double dx = flat > 1.0e-6 ? fx / flat : 0.0;
			const double dy = flat > 1.0e-6 ? fy / flat : 1.0;
			const double x = std::round(px + dx * 3.0);
			const double y = std::round(py + dy * 3.0);
			double ground = ground::HeightAt(x, y);
			if (std::isnan(ground)) {
				ground = pz - 1.6;
			}
			const double offset = mapping::Offset();
			const double bottom = std::floor(ground - offset + 0.001) + offset;  // the whole-number height (in Minecraft's terms) at or just below the street
			const double centreZ = bottom + 1.0;

			float args6[6] = { float(x), float(y), float(centreZ), 1.0f, 1.0f, 1.0f };
			RED4ext::StackArgs_t args;
			for (float& a : args6) {
				args.emplace_back(nullptr, &a);
			}
			Trace("spawn: calling CyberCraftColliders.SpawnBox");
			std::uint64_t id = 0;
			const bool ran = RED4ext::ExecuteFunction(static_cast<void*>(nullptr), spawnFn, &id, args);
			Trace("spawn: CyberCraftColliders.SpawnBox returned");
			// In Minecraft's terms: x stays, y is height minus the offset, z is minus the game's y.
			g_sdk->logger->InfoF(g_handle,
				"collision: asked for a 2 x 2 x 2 m collision box centred at game (%.1f, %.1f, %.2f): ran=%d, entity id %llu. In Minecraft that is the blocks x %.0f to %.0f, y %.0f to %.0f, z %.0f to %.0f",
				x, y, centreZ, ran ? 1 : 0, static_cast<unsigned long long>(id), x - 1.0, x, bottom - offset, bottom - offset + 1.0, -y - 1.0, -y);
			if (ran && id != 0) {
				g_spawned.push_back({ id, std::chrono::steady_clock::now(), false });
			}
		}

		void Clear()
		{
			auto* clearFn = FindStatic("CyberCraftColliders", "ClearBoxes");
			if (!clearFn) {
				return;
			}
			Trace("clear: calling CyberCraftColliders.ClearBoxes");
			RED4ext::StackArgs_t args;
			RED4ext::ExecuteFunction(static_cast<void*>(nullptr), clearFn, static_cast<void*>(nullptr), args);
			Trace("clear: CyberCraftColliders.ClearBoxes returned");
			g_spawned.clear();
		}

		// What did the game make of it? Its class and its components, in the log.
		void Inspect(Spawned& a_spawned)
		{
			a_spawned.inspected = true;
			auto* system = StaticSystem();
			auto* getFn = system ? system->GetType()->GetFunction("GetEntity") : nullptr;
			if (!getFn) {
				return;
			}
			std::uint64_t id = a_spawned.id;
			RED4ext::Handle<RED4ext::IScriptable> entity;
			RED4ext::StackArgs_t args;
			args.emplace_back(nullptr, &id);
			RED4ext::ExecuteFunction(system, getFn, &entity, args);
			if (!entity) {
				g_sdk->logger->InfoF(g_handle, "collision: entity %llu does not exist", static_cast<unsigned long long>(id));
				return;
			}
			auto* componentsFn = entity->GetType()->GetFunction("GetComponents");
			if (!componentsFn) {
				return;
			}
			RED4ext::DynArray<RED4ext::Handle<RED4ext::IScriptable>> components;
			RED4ext::ExecuteFunction(entity.instance, componentsFn, &components);
			bool anyCollider = false;
			for (uint32_t i = 0; i < components.Size(); ++i) {
				auto& c = components[i];
				if (!c) {
					continue;
				}
				auto* cls = c->GetType();
				std::string name;
				if (auto* prop = cls->GetProperty("name")) {
					name = prop->GetValue<RED4ext::CName>(c.instance).ToString();
				}
				const std::string type = cls->name.ToString();
				anyCollider = anyCollider || type.find("Collider") != std::string::npos;
				g_sdk->logger->InfoF(g_handle, "    component %u: %s \"%s\"", i, type.c_str(), name.c_str());
			}
			g_sdk->logger->InfoF(g_handle, "collision: entity %llu %s a collider component", static_cast<unsigned long long>(id), anyCollider ? "HAS" : "has NO");
		}
	}

	void Init(RED4ext::v1::PluginHandle a_handle, const RED4ext::v1::Sdk* a_sdk)
	{
		g_handle = a_handle;
		g_sdk = a_sdk;
	}

	void Command(int a_action, int)
	{
		if (a_action == 0) {
			g_sdk->logger->Info(g_handle, "collision: what the game offers for spawning objects and colliders:");
			static const char* const kClasses[] = { "StaticEntitySystem", "StaticEntitySpec", "entEntity", "physicsFilterData", "physicsSimulationFilter", "physicsQueryFilter",
				"entColliderComponent", "physicsColliderBox", "CyberCraftColliders", "CyberCraftColliderService" };
			for (const char* name : kClasses) {
				DumpClass(name);
			}
			ListOurGlobalFunctions();
		} else if (a_action == 1) {
			Spawn();
		} else if (a_action == 2) {
			Clear();
		}
	}

	void Update()
	{
		if (g_spawned.empty()) {
			return;
		}
		const auto now = std::chrono::steady_clock::now();
		for (Spawned& s : g_spawned) {
			if (!s.inspected && now - s.at > std::chrono::milliseconds(2000)) {
				Inspect(s);
			}
		}
	}
}
