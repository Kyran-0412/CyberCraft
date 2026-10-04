// CyberCraft - Phase 0: "hello world" RED4ext plugin.
//
// Once per second, while a game is running, it finds V (the player) and writes V's world position to
// the RED4ext log for this plugin:   <game folder>\red4ext\logs\CyberCraft.log
//
// Later phases will send this position over shared memory to the hidden Minecraft (Fabric) mod.

#include <RED4ext/RED4ext.hpp>
#include <RED4ext/Scripting/Natives/ScriptGameInstance.hpp>
#include <RED4ext/Scripting/Natives/Vector4.hpp>

#include <chrono>

namespace
{
RED4ext::v1::PluginHandle g_handle = nullptr;
const RED4ext::v1::Sdk* g_sdk = nullptr;

RED4ext::CClassFunction* g_getWorldPosition = nullptr;
bool g_lookedUpFunction = false;
bool g_hadPlayer = false;
std::chrono::steady_clock::time_point g_lastLog{};

// Called every frame while the game is in its "Running" state (on the game's main thread).
bool OnRunningUpdate(RED4ext::CGameApplication*)
{
    using Clock = std::chrono::steady_clock;

    const auto now = Clock::now();
    if (now - g_lastLog < std::chrono::seconds(1))
    {
        return false;
    }
    g_lastLog = now;

    // Ask the game for the player: the same call the RED4ext SDK examples use.
    RED4ext::ScriptGameInstance gameInstance;
    RED4ext::Handle<RED4ext::IScriptable> player;
    RED4ext::ExecuteGlobalFunction("GetPlayer;GameInstance", &player, gameInstance);

    if (!player)
    {
        // Main menu, loading screen, etc.
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

    // Cyberpunk is Z-up and uses metres, so 1 Minecraft block == 1 metre.
    g_sdk->logger->InfoF(g_handle, "V is at x=%.2f y=%.2f z=%.2f", position.X, position.Y, position.Z);
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
        break;
    }
    }

    return true;
}

RED4EXT_C_EXPORT void RED4EXT_CALL Query(RED4ext::v1::PluginInfo* aInfo)
{
    aInfo->name = L"CyberCraft";
    aInfo->author = L"Kyran";
    aInfo->version = RED4EXT_V1_SEMVER(0, 1, 0);
    aInfo->runtime = RED4EXT_V1_RUNTIME_VERSION_LATEST;
    aInfo->sdk = RED4EXT_V1_SDK_VERSION_CURRENT;
}

RED4EXT_C_EXPORT uint32_t RED4EXT_CALL Supports()
{
    return RED4EXT_API_VERSION_1;
}
