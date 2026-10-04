// CyberCraft - Phase 1b-ii: the Cyberpunk end of the link.
//
// Every frame, while a game is running, it finds V (the player) and publishes V's position to shared
// memory (see protocol/cybercraft_protocol.h) for the Minecraft mod to read. It also listens for one-off
// commands from Minecraft ("teleport V here"), and while Minecraft says "follow" it keeps moving V to the
// position Minecraft's player has. Once a second it writes a line to the RED4ext
// log for this plugin:   <game folder>\red4ext\logs\CyberCraft.log

#include "Link.hpp"

#include <cybercraft_protocol.h>

#include <RED4ext/RED4ext.hpp>
#include <RED4ext/Scripting/Natives/Generated/EulerAngles.hpp>
#include <RED4ext/Scripting/Natives/ScriptGameInstance.hpp>
#include <RED4ext/Scripting/Natives/Vector4.hpp>

#include <Windows.h>

#include <chrono>
#include <cmath>

namespace
{
RED4ext::v1::PluginHandle g_handle = nullptr;
const RED4ext::v1::Sdk* g_sdk = nullptr;

RED4ext::CClassFunction* g_getWorldPosition = nullptr;
RED4ext::CClassFunction* g_getWorldYaw = nullptr;
bool g_lookedUpFunction = false;
bool g_hadPlayer = false;
bool g_mcWasLinked = false;
std::chrono::steady_clock::time_point g_lastLog{};

// "Follow" mode: V is moved to wherever Minecraft's player is, every frame.
struct FollowState
{
    bool active = false;
    bool gaveUp = false; // too many failures; stays off until Minecraft stops asking and asks again
    double x = 0.0, y = 0.0, z = 0.0; // smoothed target, protocol (Minecraft-axes) coordinates
    float yaw = 0.0f;                  // smoothed, Cyberpunk degrees
    double lastSentX = 0.0, lastSentY = 0.0, lastSentZ = 0.0;
    float lastSentYaw = 0.0f;
    bool haveSent = false;
    int failures = 0;
    std::chrono::steady_clock::time_point last{};
} g_follow;

// Cached after the first successful teleport (class and function pointers don't change while the game runs).
RED4ext::CClass* g_facilityCls = nullptr;
RED4ext::CClassFunction* g_teleportFunc = nullptr;

// Finds the game's teleportation facility (the object whose Teleport function moves things around).
// Two routes are tried, because we can't tell from here which one this game version supports:
//   1. as a game system, looked up by its class name;
//   2. through the script function ScriptGameInstance.GetTeleportationFacility, like redscript mods do.
RED4ext::Handle<RED4ext::IScriptable> FindTeleportFacility()
{
    static bool loggedRoute = false;
    auto rtti = RED4ext::CRTTISystem::Get();

    if (auto cls = rtti->GetClass("gameTeleportationFacility"))
    {
        auto engine = RED4ext::CGameEngine::Get();
        if (engine && engine->framework && engine->framework->gameInstance)
        {
            if (auto system = engine->framework->gameInstance->GetSystem(cls))
            {
                if (!loggedRoute)
                {
                    loggedRoute = true;
                    g_sdk->logger->Info(g_handle, "teleport: found the teleportation facility as a game system");
                }
                return RED4ext::Handle<RED4ext::IScriptable>(system);
            }
        }
    }

    if (auto cls = rtti->GetClass("ScriptGameInstance"))
    {
        if (auto func = cls->GetFunction("GetTeleportationFacility"))
        {
            RED4ext::ScriptGameInstance game;
            RED4ext::Handle<RED4ext::IScriptable> facility;
            RED4ext::StackArgs_t args;
            args.emplace_back(nullptr, &game);
            RED4ext::ExecuteFunction(static_cast<void*>(nullptr), func, &facility, args);
            if (facility)
            {
                if (!loggedRoute)
                {
                    loggedRoute = true;
                    g_sdk->logger->Info(g_handle, "teleport: found the teleportation facility through ScriptGameInstance");
                }
                return facility;
            }
        }
    }

    return {};
}

// Moves V to a position in Cyberpunk coordinates (X east, Y north, Z up, metres). Returns false on any failure.
// If aYawOverride is a number, V is turned to face that way (degrees); otherwise V keeps facing the same way.
bool TeleportPlayer(RED4ext::Handle<RED4ext::IScriptable>& aPlayer, double aX, double aY, double aZ,
                    float aYawOverride = std::nanf(""), bool aQuiet = false)
{
    auto facility = FindTeleportFacility();
    if (!facility)
    {
        g_sdk->logger->Error(g_handle, "teleport: could not find the teleportation facility");
        return false;
    }

    if (!g_teleportFunc)
    {
        auto rtti = RED4ext::CRTTISystem::Get();
        g_facilityCls = rtti->GetClass("gameTeleportationFacility");
        g_teleportFunc = g_facilityCls ? g_facilityCls->GetFunction("Teleport") : nullptr;
        if (!g_teleportFunc)
        {
            g_sdk->logger->Error(g_handle, "teleport: could not find gameTeleportationFacility::Teleport");
            return false;
        }
    }
    auto teleport = g_teleportFunc;

    // Keep V facing the way V already faces, unless a direction was given.
    float yaw = 0.0f;
    if (!std::isnan(aYawOverride))
    {
        yaw = aYawOverride;
    }
    else if (g_getWorldYaw)
    {
        RED4ext::ExecuteFunction(aPlayer.instance, g_getWorldYaw, &yaw);
    }

    RED4ext::Vector4 position(static_cast<float>(aX), static_cast<float>(aY), static_cast<float>(aZ), 1.0f);
    RED4ext::EulerAngles rotation{0.0f, 0.0f, yaw};

    RED4ext::StackArgs_t args;
    args.emplace_back(nullptr, &aPlayer);
    args.emplace_back(nullptr, &position);
    args.emplace_back(nullptr, &rotation);

    bool result = false; // some versions return a bool, some return nothing
    const bool executed = RED4ext::ExecuteFunction(facility.instance, teleport, &result, args);
    if (!aQuiet)
    {
        g_sdk->logger->InfoF(g_handle, "teleport: Teleport executed=%d returned=%d", executed ? 1 : 0, result ? 1 : 0);
    }
    return executed;
}

// Acts on a command from Minecraft. The player may be null (main menu, loading screen).
void HandleCommand(const cybercraft::Link::Command& aCommand, RED4ext::Handle<RED4ext::IScriptable>& aPlayer)
{
    auto& link = cybercraft::Link::Get();

    if (aCommand.kind != cybercraft::proto::kCmdTeleport)
    {
        g_sdk->logger->WarnF(g_handle, "command #%u: unknown kind %u", aCommand.seq, aCommand.kind);
        link.AckCommand(aCommand.seq, false);
        return;
    }

    if (!aPlayer)
    {
        g_sdk->logger->WarnF(g_handle, "command #%u: teleport ignored, there is no player right now", aCommand.seq);
        link.AckCommand(aCommand.seq, false);
        return;
    }

    // Minecraft (X east, Y up, -Z north) -> Cyberpunk (X east, Y north, Z up).
    const double cx = aCommand.x;
    const double cy = -aCommand.z;
    const double cz = aCommand.y;
    g_sdk->logger->InfoF(g_handle, "command #%u: teleport V to x=%.2f y=%.2f z=%.2f", aCommand.seq, cx, cy, cz);

    link.AckCommand(aCommand.seq, TeleportPlayer(aPlayer, cx, cy, cz));
}

// Wraps an angle difference into -180..180.
float WrapDegrees(float aDegrees)
{
    while (aDegrees > 180.0f)
    {
        aDegrees -= 360.0f;
    }
    while (aDegrees < -180.0f)
    {
        aDegrees += 360.0f;
    }
    return aDegrees;
}

// One frame of follow mode: ease V's position towards where Minecraft's player is, and teleport V there.
// Minecraft only sends its position 20 times a second, so easing hides the steps between updates.
void StepFollow(const cybercraft::Link::McSnapshot& aMc, RED4ext::Handle<RED4ext::IScriptable>& aPlayer,
                const RED4ext::Vector4& aCurrent)
{
    using Clock = std::chrono::steady_clock;
    const auto now = Clock::now();

    // Minecraft yaw: 0 = south, increasing clockwise from above. Cyberpunk yaw: 0 = north, increasing counter-clockwise.
    const float targetYaw = WrapDegrees(180.0f - aMc.yaw);

    if (!g_follow.active)
    {
        g_follow.active = true;
        g_follow.failures = 0;
        g_follow.haveSent = false;
        g_follow.last = now;
        g_follow.x = aMc.x;
        g_follow.y = aMc.y;
        g_follow.z = aMc.z;
        g_follow.yaw = targetYaw;
        g_sdk->logger->Info(g_handle, "follow: engaged, V now follows Minecraft's player");
        (void)aCurrent;
    }
    else
    {
        double dt = std::chrono::duration<double>(now - g_follow.last).count();
        g_follow.last = now;
        if (dt > 0.1)
        {
            dt = 0.1;
        }

        const double dx = aMc.x - g_follow.x;
        const double dy = aMc.y - g_follow.y;
        const double dz = aMc.z - g_follow.z;
        if (dx * dx + dy * dy + dz * dz > 8.0 * 8.0)
        {
            g_follow.x = aMc.x; // far away (a jump or a teleport): don't glide, just go
            g_follow.y = aMc.y;
            g_follow.z = aMc.z;
        }
        else
        {
            const double alpha = 1.0 - std::exp(-dt * 25.0);
            g_follow.x += dx * alpha;
            g_follow.y += dy * alpha;
            g_follow.z += dz * alpha;
        }

        const float yawAlpha = static_cast<float>(1.0 - std::exp(-dt * 25.0));
        g_follow.yaw = WrapDegrees(g_follow.yaw + WrapDegrees(targetYaw - g_follow.yaw) * yawAlpha);
    }

    // Standing still: don't keep re-teleporting to the same spot.
    if (g_follow.haveSent)
    {
        const double mx = g_follow.x - g_follow.lastSentX;
        const double my = g_follow.y - g_follow.lastSentY;
        const double mz = g_follow.z - g_follow.lastSentZ;
        if (mx * mx + my * my + mz * mz < 0.005 * 0.005 && std::fabs(WrapDegrees(g_follow.yaw - g_follow.lastSentYaw)) < 0.2f)
        {
            return;
        }
    }

    // Minecraft (X east, Y up, -Z north) -> Cyberpunk (X east, Y north, Z up).
    if (TeleportPlayer(aPlayer, g_follow.x, -g_follow.z, g_follow.y, g_follow.yaw, true))
    {
        g_follow.failures = 0;
        g_follow.haveSent = true;
        g_follow.lastSentX = g_follow.x;
        g_follow.lastSentY = g_follow.y;
        g_follow.lastSentZ = g_follow.z;
        g_follow.lastSentYaw = g_follow.yaw;
    }
    else if (++g_follow.failures >= 30)
    {
        g_follow.gaveUp = true;
        g_follow.active = false;
        g_sdk->logger->Error(g_handle, "follow: the teleport keeps failing, giving up (use /ccstop then /ccfollow in Minecraft to retry)");
    }
}

void StopFollow(const char* aReason)
{
    if (g_follow.active)
    {
        g_follow.active = false;
        g_sdk->logger->InfoF(g_handle, "follow: stopped (%s)", aReason);
    }
}

// Called every frame while the game is in its "Running" state (on the game's main thread).
bool OnRunningUpdate(RED4ext::CGameApplication*)
{
    using Clock = std::chrono::steady_clock;

    auto& link = cybercraft::Link::Get();
    link.Beat();

    const auto now = Clock::now();
    const bool logNow = (now - g_lastLog) >= std::chrono::seconds(1);
    if (logNow)
    {
        g_lastLog = now;

        // Tell the log when Minecraft connects or disconnects.
        const bool linked = link.McPid() != 0 && (GetTickCount64() - link.McHeartbeatMs()) < 8000;
        if (linked != g_mcWasLinked)
        {
            g_mcWasLinked = linked;
            if (linked)
            {
                g_sdk->logger->InfoF(g_handle, "Minecraft linked (pid %u)", static_cast<unsigned>(link.McPid()));
            }
            else
            {
                g_sdk->logger->Info(g_handle, "Minecraft link lost");
            }
        }
    }

    // Ask the game for the player: the same call the RED4ext SDK examples use.
    RED4ext::ScriptGameInstance gameInstance;
    RED4ext::Handle<RED4ext::IScriptable> player;
    RED4ext::ExecuteGlobalFunction("GetPlayer;GameInstance", &player, gameInstance);

    cybercraft::Link::Command command{};
    const bool haveCommand = link.PollCommand(command);

    if (!player)
    {
        // Main menu, loading screen, etc.
        if (haveCommand)
        {
            HandleCommand(command, player);
        }
        StopFollow("no player right now");
        link.PublishPlayer(false, 0.0, 0.0, 0.0);
        if (g_hadPlayer)
        {
            g_sdk->logger->Info(g_handle, "player is gone (loading screen or main menu)");
            g_hadPlayer = false;
        }
        return false;
    }

    if (!g_hadPlayer)
    {
        g_sdk->logger->Info(g_handle, "player found");
        g_hadPlayer = true;
    }

    if (!g_lookedUpFunction)
    {
        g_lookedUpFunction = true;

        auto rtti = RED4ext::CRTTISystem::Get();
        auto playerPuppet = rtti->GetClass("PlayerPuppet");
        if (playerPuppet)
        {
            g_getWorldPosition = playerPuppet->GetFunction("GetWorldPosition");
            g_getWorldYaw = playerPuppet->GetFunction("GetWorldYaw");
        }

        if (!g_getWorldPosition)
        {
            g_sdk->logger->Error(g_handle, "could not find PlayerPuppet::GetWorldPosition");
        }
    }

    if (!g_getWorldPosition)
    {
        return false;
    }

    RED4ext::Vector4 position;
    RED4ext::ExecuteFunction(player.instance, g_getWorldPosition, &position);

    if (haveCommand)
    {
        HandleCommand(command, player);
    }

    // Is Minecraft asking us to follow its player?
    cybercraft::Link::McSnapshot mc{};
    const bool mcAlive = link.McPid() != 0 && (GetTickCount64() - link.McHeartbeatMs()) < 3000;
    const bool mcFollow = mcAlive && link.ReadMcState(mc) && (mc.flags & cybercraft::proto::kMcInWorld) != 0 &&
                          (mc.flags & cybercraft::proto::kMcFollow) != 0;
    if (mcFollow)
    {
        if (!g_follow.gaveUp)
        {
            StepFollow(mc, player, position);
        }
    }
    else
    {
        g_follow.gaveUp = false;
        StopFollow(mcAlive ? "Minecraft stopped asking" : "Minecraft is gone");
    }

    // Cyberpunk is Z-up (X east, Y north) in metres; Minecraft is Y-up (X east, -Z north) in blocks.
    link.PublishPlayer(true, position.X, position.Z, -position.Y);

    if (logNow)
    {
        g_sdk->logger->InfoF(g_handle, "V is at x=%.2f y=%.2f z=%.2f", position.X, position.Y, position.Z);
    }
    return false;
}
} // namespace

RED4EXT_C_EXPORT bool RED4EXT_CALL Main(RED4ext::v1::PluginHandle aHandle, RED4ext::v1::EMainReason aReason,
                                        const RED4ext::v1::Sdk* aSdk)
{
    switch (aReason)
    {
    case RED4ext::v1::EMainReason::Load:
    {
        g_handle = aHandle;
        g_sdk = aSdk;

        aSdk->logger->InfoF(aHandle, "CyberCraft loaded (game version %u.%u.%u)", static_cast<unsigned>(aSdk->runtime->major),
                            static_cast<unsigned>(aSdk->runtime->minor), static_cast<unsigned>(aSdk->runtime->patch));

        if (cybercraft::Link::Get().Create())
        {
            aSdk->logger->Info(aHandle, "shared memory created, waiting for Minecraft");
        }
        else
        {
            aSdk->logger->ErrorF(aHandle, "could not create shared memory (Windows error %lu)", GetLastError());
        }

        static RED4ext::v1::GameState runningState{
            .OnEnter = nullptr,
            .OnUpdate = &OnRunningUpdate,
            .OnExit = nullptr,
        };
        aSdk->gameStates->Add(aHandle, RED4ext::EGameStateType::Running, &runningState);
        break;
    }
    case RED4ext::v1::EMainReason::Unload:
    {
        cybercraft::Link::Get().Close();
        break;
    }
    }

    return true;
}

RED4EXT_C_EXPORT void RED4EXT_CALL Query(RED4ext::v1::PluginInfo* aInfo)
{
    aInfo->name = L"CyberCraft";
    aInfo->author = L"Kyran";
    aInfo->version = RED4EXT_V1_SEMVER(0, 4, 0);
    aInfo->runtime = RED4EXT_V1_RUNTIME_VERSION_LATEST;
    aInfo->sdk = RED4EXT_V1_SDK_VERSION_CURRENT;
}

RED4EXT_C_EXPORT uint32_t RED4EXT_CALL Supports()
{
    return RED4EXT_API_VERSION_1;
}
