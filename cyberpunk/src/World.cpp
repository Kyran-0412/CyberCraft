// Minecraft's time of day and weather, and making the game's follow them.
//
// Minecraft publishes its day time (ticks: 0 is 6:00, 6000 noon, 12000 18:00, 18000 midnight) and its rain and thunder levels, and whether the player has turned the sync on with
// /ccsync time and /ccsync weather. With the time sync on, the game's time of day is set from Minecraft's ten times a second, through gameTimeSystem::SetGameTimeByHMS (found with
// /ccdebug class): Minecraft's day is 20 minutes long, so the game's days run at that rate (/gamerule doDaylightCycle false freezes them). The weather is still only read and logged:
// the call that sets it has yet to be found.

#include "World.hpp"
#include "Link.hpp"
#include "Log.hpp"

#include <cybercraft_protocol.h>

#include <RED4ext/Scripting/Natives/ScriptGameInstance.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <vector>

namespace cybercraft::world
{
	namespace
	{
		RED4ext::v1::PluginHandle g_handle = nullptr;
		const RED4ext::v1::Sdk* g_sdk = nullptr;

		bool g_lastSyncTime = false;
		bool g_lastSyncWeather = false;
		int g_lastWeatherClass = -1;
		std::chrono::steady_clock::time_point g_lastTimeLog{};

		// 0 clear, 1 rain, 2 thunder: Minecraft's three states.
		int WeatherClass(float a_rain, float a_thunder)
		{
			if (a_thunder > 0.05f) {
				return 2;
			}
			return a_rain > 0.05f ? 1 : 0;
		}

		const char* WeatherName(int a_class)
		{
			return a_class == 2 ? "thunder" : a_class == 1 ? "rain" : "clear";
		}

		// The game's weathers, by the short names used in /ccweather. Each has the names the game may know it by; the first one the game accepts is used. (SetWeather returns whether it
		// accepted, so the log shows which names are real.) Minecraft's three weathers map onto the first, and the fifth: clear is sunny, rain and thunder are rain.
		struct WeatherAlias
		{
			const char* word;
			std::vector<const char*> names;
		};
		const std::vector<WeatherAlias>& Aliases()
		{
			static const std::vector<WeatherAlias> table = {
				{ "sunny", { "24h_weather_sunny" } },
				{ "clear", { "24h_weather_sunny" } },
				{ "lightclouds", { "24h_weather_light_clouds" } },
				{ "cloudy", { "24h_weather_cloudy" } },
				{ "heavyclouds", { "24h_weather_heavy_clouds" } },
				{ "rain", { "24h_weather_rain" } },
				{ "lightrain", { "24h_weather_light_rain", "24h_weather_drizzle" } },
				{ "fog", { "24h_weather_fog" } },
				{ "pollution", { "24h_weather_pollution" } },
				{ "acid", { "24h_weather_toxic_rain" } },
				{ "sandstorm", { "24h_weather_sandstorm" } },
			};
			return table;
		}

		// A weather chosen with /ccweather is pinned: it is kept (re-sent every 30 seconds) until /ccweather reset, or until Minecraft's weather changes while the weather sync is on (Minecraft
		// is the master, so a /weather command takes over again). Without this the sync's own re-sending of Minecraft's weather wiped out the choice within 30 seconds.
		std::string g_pinnedWord;
		std::chrono::steady_clock::time_point g_lastPinSend{};

		RED4ext::CClassFunction* g_setWeatherFunction = nullptr;
		RED4ext::CClassFunction* g_resetWeatherFunction = nullptr;
		std::chrono::steady_clock::time_point g_lastWeatherSet{};
		int g_weatherSent = -1;  // the Minecraft weather class last sent to the game

		// The game's weather interface (worldWeatherScriptInterface), through the ScriptGameInstance function that returns it.
		RED4ext::Handle<RED4ext::IScriptable> FindWeatherSystem()
		{
			auto rtti = RED4ext::CRTTISystem::Get();
			auto cls = rtti->GetClass("ScriptGameInstance");
			if (!cls) {
				return {};
			}
			static RED4ext::CBaseFunction* getter = nullptr;
			static bool searched = false;
			if (!searched) {
				searched = true;
				getter = cls->GetFunction("GetWeatherSystem");
				if (!getter) {
					for (uint32_t i = 0; i < cls->staticFuncs.Size(); ++i) {
						const std::string name = cls->staticFuncs[i]->shortName.ToString();
						if (name.find("Weather") != std::string::npos) {
							getter = cls->staticFuncs[i];
							break;
						}
					}
				}
				g_sdk->logger->InfoF(g_handle, "world: the game's weather interface comes from %s", getter ? getter->shortName.ToString() : "NOWHERE (no ScriptGameInstance function with Weather in its name)");
			}
			if (!getter) {
				return {};
			}
			RED4ext::ScriptGameInstance game;
			RED4ext::Handle<RED4ext::IScriptable> system;
			RED4ext::StackArgs_t args;
			args.emplace_back(nullptr, &game);
			RED4ext::ExecuteFunction(static_cast<void*>(nullptr), getter, &system, args);
			return system;
		}

		// Asks the game to blend to a weather by name. Returns whether the game said yes (false also if it couldn't be asked).
		bool SetGameWeather(const char* a_name, float a_blendSeconds)
		{
			auto system = FindWeatherSystem();
			if (!system) {
				return false;
			}
			if (!g_setWeatherFunction) {
				auto cls = RED4ext::CRTTISystem::Get()->GetClass("worldWeatherScriptInterface");
				g_setWeatherFunction = cls ? cls->GetFunction("SetWeather") : nullptr;
				if (!g_setWeatherFunction) {
					return false;
				}
			}
			RED4ext::CName weather(a_name);
			float blend = a_blendSeconds;
			std::uint32_t priority = 9;
			RED4ext::StackArgs_t args;
			args.emplace_back(nullptr, &weather);
			args.emplace_back(nullptr, &blend);
			args.emplace_back(nullptr, &priority);
			bool accepted = false;
			if (!RED4ext::ExecuteFunction(system.instance, g_setWeatherFunction, &accepted, args)) {
				return false;
			}
			return accepted;
		}

		bool ResetGameWeather(float a_blendSeconds)
		{
			auto system = FindWeatherSystem();
			if (!system) {
				return false;
			}
			if (!g_resetWeatherFunction) {
				auto cls = RED4ext::CRTTISystem::Get()->GetClass("worldWeatherScriptInterface");
				g_resetWeatherFunction = cls ? cls->GetFunction("ResetWeather") : nullptr;
				if (!g_resetWeatherFunction) {
					return false;
				}
			}
			bool force = true;
			float blend = a_blendSeconds;
			RED4ext::StackArgs_t args;
			args.emplace_back(nullptr, &force);
			args.emplace_back(nullptr, &blend);
			bool done = false;
			return RED4ext::ExecuteFunction(system.instance, g_resetWeatherFunction, &done, args) && done;
		}

		// Tries each name an alias may go by until the game accepts one.
		bool SetByAlias(const std::string& a_word, float a_blendSeconds, bool a_log = true)
		{
			for (const WeatherAlias& a : Aliases()) {
				if (a_word != a.word) {
					continue;
				}
				for (const char* name : a.names) {
					const bool ok = SetGameWeather(name, a_blendSeconds);
					if (a_log || !ok) {
						g_sdk->logger->InfoF(g_handle, "world: weather %s -> %s: the game %s", a.word, name, ok ? "accepted it" : "did NOT accept it");
					}
					if (ok) {
						return true;
					}
				}
				return false;
			}
			return false;
		}

		std::chrono::steady_clock::time_point g_syncOnSince{};
		bool g_warnedNoWorld = false;
		std::chrono::steady_clock::time_point g_lastTimeSet{};
		unsigned g_timeSets = 0;
		unsigned g_timeFailures = 0;
		RED4ext::CClassFunction* g_setTimeFunction = nullptr;

		// The game's time system, as a game system (looked up by its class name, like the teleportation facility), or through ScriptGameInstance.GetTimeSystem.
		RED4ext::Handle<RED4ext::IScriptable> FindTimeSystem()
		{
			auto rtti = RED4ext::CRTTISystem::Get();
			auto engine = RED4ext::CGameEngine::Get();
			if (engine && engine->framework && engine->framework->gameInstance) {
				for (const char* name : { "gameTimeSystem", "gameITimeSystem" }) {
					if (auto cls = rtti->GetClass(name)) {
						if (auto system = engine->framework->gameInstance->GetSystem(cls)) {
							return RED4ext::Handle<RED4ext::IScriptable>(system);
						}
					}
				}
			}
			if (auto cls = rtti->GetClass("ScriptGameInstance")) {
				if (auto func = cls->GetFunction("GetTimeSystem")) {
					RED4ext::ScriptGameInstance game;
					RED4ext::Handle<RED4ext::IScriptable> system;
					RED4ext::StackArgs_t args;
					args.emplace_back(nullptr, &game);
					RED4ext::ExecuteFunction(static_cast<void*>(nullptr), func, &system, args);
					return system;
				}
			}
			return {};
		}

		// Sets the game's time of day to a number of seconds since midnight.
		bool SetGameTime(std::int64_t a_secondsOfDay)
		{
			auto system = FindTimeSystem();
			if (!system) {
				return false;
			}
			if (!g_setTimeFunction) {
				auto cls = RED4ext::CRTTISystem::Get()->GetClass("gameTimeSystem");
				g_setTimeFunction = cls ? cls->GetFunction("SetGameTimeByHMS") : nullptr;
				if (!g_setTimeFunction) {
					return false;
				}
			}
			std::int32_t hours = static_cast<std::int32_t>(a_secondsOfDay / 3600);
			std::int32_t minutes = static_cast<std::int32_t>((a_secondsOfDay / 60) % 60);
			std::int32_t seconds = static_cast<std::int32_t>(a_secondsOfDay % 60);
			RED4ext::CName reason("CyberCraft");
			RED4ext::StackArgs_t args;
			args.emplace_back(nullptr, &hours);
			args.emplace_back(nullptr, &minutes);
			args.emplace_back(nullptr, &seconds);
			args.emplace_back(nullptr, &reason);
			bool ignored = false;  // the function returns nothing
			return RED4ext::ExecuteFunction(system.instance, g_setTimeFunction, &ignored, args);
		}
	}

	void Init(RED4ext::v1::PluginHandle a_handle, const RED4ext::v1::Sdk* a_sdk)
	{
		g_handle = a_handle;
		g_sdk = a_sdk;
	}

	std::string UnpackWord(double a_first, double a_second)
	{
		std::string word;
		for (const double part : { a_first, a_second }) {
			const std::uint64_t packed = static_cast<std::uint64_t>(part);
			for (int i = 0; i < 6; ++i) {
				const char ch = static_cast<char>((packed >> (8 * i)) & 0x7F);
				if (!ch) {
					break;
				}
				word += ch;
			}
			if (word.size() < 6) {
				break;
			}
		}
		return word;
	}

	void SetWeatherByWord(const std::string& a_word)
	{
		if (a_word == "reset") {
			g_pinnedWord.clear();
			const bool ok = ResetGameWeather(6.0f);
			g_weatherSent = -1;
			g_sdk->logger->InfoF(g_handle, "world: weather given back to the game's own schedule: %s", ok ? "the game said yes" : "the game did NOT say yes");
			return;
		}
		if (SetByAlias(a_word, 6.0f)) {
			g_pinnedWord = a_word;
			g_lastPinSend = std::chrono::steady_clock::now();
			g_sdk->logger->InfoF(g_handle, "world: weather held at %s until /ccweather reset%s", a_word.c_str(), g_lastSyncWeather ? " or until Minecraft's weather changes" : "");
			return;
		}
		{
			std::string known;
			for (const WeatherAlias& a : Aliases()) {
				known += std::string(a.word) + " ";
			}
			g_sdk->logger->InfoF(g_handle, "world: no weather could be set for \"%s\" (known: %sreset)", a_word.c_str(), known.c_str());
		}
	}

	void Update()
	{
		Link::McWorldSnapshot w;
		if (!Link::Get().ReadMcWorld(w)) {
			return;
		}
		const bool inWorld = (w.flags & proto::kWorldInWorld) != 0;
		const bool syncTime = (w.flags & proto::kWorldSyncTime) != 0;
		const bool syncWeather = (w.flags & proto::kWorldSyncWeather) != 0;
		const auto now = std::chrono::steady_clock::now();

		if (syncTime != g_lastSyncTime) {
			g_lastSyncTime = syncTime;
			g_sdk->logger->InfoF(g_handle, "world: time sync %s", syncTime ? "ON (the game's time of day now follows Minecraft's; its day is 20 minutes long)" : "off (the game keeps whatever time it was last set to, and runs it itself)");
			g_lastTimeLog = {};
			g_syncOnSince = now;
			g_warnedNoWorld = false;
			g_timeSets = 0;
			g_timeFailures = 0;
		}
		if (syncWeather != g_lastSyncWeather) {
			g_lastSyncWeather = syncWeather;
			g_sdk->logger->InfoF(g_handle, "world: weather sync %s", syncWeather ? "ON (the game's weather now follows Minecraft's: clear is sunny, rain and thunder are rain)" : "off (the weather goes back to the game's own schedule)");
			g_lastWeatherClass = -1;
			g_weatherSent = -1;
			if (!syncWeather && g_pinnedWord.empty()) {
				ResetGameWeather(6.0f);
			}
		}
		// A pinned weather is kept whether or not Minecraft has a world: re-sent every 30 seconds (the game forgets a set weather after a while, and the time it takes isn't the same each time).
		if (!g_pinnedWord.empty() && now - g_lastPinSend > std::chrono::seconds(30)) {
			g_lastPinSend = now;
			SetByAlias(g_pinnedWord, 6.0f, false);
		}
		if (!inWorld) {
			if ((syncTime || syncWeather) && !g_warnedNoWorld && now - g_syncOnSince > std::chrono::seconds(5)) {
				g_warnedNoWorld = true;
				g_sdk->logger->Info(g_handle, "world: Minecraft is not reporting a time of day or weather (it can't read them from its world): type /ccsync status in Minecraft to see what it found");
			}
			return;
		}
		if (syncTime && now - g_lastTimeLog > std::chrono::seconds(5)) {
			g_lastTimeLog = now;
			const std::int64_t ticks = ((w.dayTime % 24000) + 24000) % 24000;
			const double hours = std::fmod(static_cast<double>(ticks) / 1000.0 + 6.0, 24.0);
			const int hour = static_cast<int>(hours);
			const int minute = static_cast<int>((hours - hour) * 60.0);
			g_sdk->logger->InfoF(g_handle, "world: Minecraft's time of day is %02d:%02d (day tick %lld)", hour, minute, static_cast<long long>(w.dayTime));
		}
		if (syncTime && now - g_lastTimeSet > std::chrono::milliseconds(100)) {
			g_lastTimeSet = now;
			const std::int64_t ticks = ((w.dayTime % 24000) + 24000) % 24000;
			const std::int64_t secondsOfDay = (((ticks + 6000) % 24000) * 36) / 10;  // 24000 ticks are 86400 seconds, and tick 0 is 6:00
			if (SetGameTime(secondsOfDay)) {
				++g_timeSets;
				if (g_timeSets <= 3) {
					g_sdk->logger->InfoF(g_handle, "world: set the game's time of day to %02d:%02d:%02d", static_cast<int>(secondsOfDay / 3600), static_cast<int>((secondsOfDay / 60) % 60), static_cast<int>(secondsOfDay % 60));
				}
			} else if (++g_timeFailures == 1 || g_timeFailures % 600 == 0) {
				g_sdk->logger->InfoF(g_handle, "world: could not set the game's time of day (failure %u): the time system or SetGameTimeByHMS was not found", g_timeFailures);
			}
		}
		if (syncWeather) {
			const int cls = WeatherClass(w.rain, w.thunder);
			if (cls != g_lastWeatherClass) {
				g_lastWeatherClass = cls;
				g_sdk->logger->InfoF(g_handle, "world: Minecraft's weather is %s (rain %.2f, thunder %.2f)", WeatherName(cls), w.rain, w.thunder);
				if (!g_pinnedWord.empty()) {
					g_sdk->logger->InfoF(g_handle, "world: Minecraft's weather changed, so the weather held at %s is let go", g_pinnedWord.c_str());
					g_pinnedWord.clear();
					g_weatherSent = -1;
				}
			}
			// Tell the game when it changes, and again every 30 seconds in case something else in the game took the weather back.
			if (g_pinnedWord.empty() && (cls != g_weatherSent || now - g_lastWeatherSet > std::chrono::seconds(30))) {
				const bool changed = cls != g_weatherSent;
				g_weatherSent = cls;
				g_lastWeatherSet = now;
				SetByAlias(cls == 0 ? "sunny" : "rain", 6.0f, changed);
			}
		}
	}
}
