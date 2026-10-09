// Minecraft's time of day and weather, and (to come) making the game's follow them.
//
// Minecraft publishes its day time (ticks: 0 is 6:00, 6000 noon, 12000 18:00, 18000 midnight) and its rain and thunder levels, and whether the player has turned the sync on with
// /ccsync time and /ccsync weather. This module reads them and, for now, writes to the log what it sees, so the link can be checked before anything in the game is changed.

#include "World.hpp"
#include "Link.hpp"
#include "Log.hpp"

#include <cybercraft_protocol.h>

#include <chrono>
#include <cmath>
#include <cstdint>

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
	}

	void Init(RED4ext::v1::PluginHandle a_handle, const RED4ext::v1::Sdk* a_sdk)
	{
		g_handle = a_handle;
		g_sdk = a_sdk;
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
			g_sdk->logger->InfoF(g_handle, "world: time sync %s", syncTime ? "ON (Minecraft's time of day is now read; setting the game's time is not done yet)" : "off");
			g_lastTimeLog = {};
		}
		if (syncWeather != g_lastSyncWeather) {
			g_lastSyncWeather = syncWeather;
			g_sdk->logger->InfoF(g_handle, "world: weather sync %s", syncWeather ? "ON (Minecraft's weather is now read; setting the game's weather is not done yet)" : "off");
			g_lastWeatherClass = -1;
		}
		if (!inWorld) {
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
		if (syncWeather) {
			const int cls = WeatherClass(w.rain, w.thunder);
			if (cls != g_lastWeatherClass) {
				g_lastWeatherClass = cls;
				g_sdk->logger->InfoF(g_handle, "world: Minecraft's weather is %s (rain %.2f, thunder %.2f)", WeatherName(cls), w.rain, w.thunder);
			}
		}
	}
}
