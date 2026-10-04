// CyberCraft shared-memory protocol (Cyberpunk RED4ext plugin <-> Minecraft Fabric mod).
//
// This header is the single source of truth for the byte layout. The Java side mirrors it in
// fabric/src/main/java/dev/cybercraft/link/Proto.java: if you change anything here, change it there
// too and bump kVersion.
//
// All multi-byte values are little-endian. Cyberpunk creates the mapping; Minecraft opens it.
// Coordinates in this protocol are always Minecraft space (blocks, Y up, Z south).
#pragma once

#include <cstdint>

namespace cybercraft::proto
{
	inline constexpr std::uint32_t kMagic = 0x43425943;  // "CYBC"
	inline constexpr std::uint32_t kVersion = 2;
	inline constexpr wchar_t       kMappingName[] = L"Local\\CyberCraft_v1";

	// Cyberpunk uses metres and Minecraft blocks are 1 m, so no scaling is needed.
	// Axes: Cyberpunk is Z-up (X east, Y north). Minecraft is Y-up (X east, -Z north).
	//   mc.x = cp.x,   mc.y = cp.z,   mc.z = -cp.y
	inline constexpr double kUnitsPerBlock = 1.0;

	// ---- region offsets ---------------------------------------------------------------------
	inline constexpr std::uint64_t kOffHeader = 0x0;
	inline constexpr std::uint64_t kOffGameState = 0x100;  // Cyberpunk -> Minecraft
	inline constexpr std::uint64_t kOffMcState = 0x200;    // Minecraft -> Cyberpunk: player state (reserved for phase 1b-ii)
	inline constexpr std::uint64_t kOffMcCommand = 0x300;  // Minecraft -> Cyberpunk: one-off commands
	inline constexpr std::uint64_t kMappingBytes = 0x1000;

	// ---- header @0x0 ------------------------------------------------------------------------
	struct Header
	{
		std::uint32_t magic;
		std::uint32_t version;
		std::uint32_t gamePid;
		std::uint32_t mcPid;
		std::uint64_t gameHeartbeatMs;  // GetTickCount64() at the last Cyberpunk frame
		std::uint64_t mcHeartbeatMs;    // GetTickCount64() at the last Minecraft tick
	};
	static_assert(sizeof(Header) == 0x20);

	// ---- Cyberpunk -> Minecraft state @0x100 (seqlock: seq is odd while being written) ------
	enum GameFlags : std::uint32_t
	{
		kGameInGame = 1u << 0,  // a save is loaded and the player exists
	};

	struct GameState
	{
		std::uint32_t seq;
		std::uint32_t flags;  // GameFlags
		double        posX, posY, posZ;  // V's feet, Minecraft coordinates
		std::uint64_t frameCounter;
		std::uint32_t cmdAck;     // seq of the last McCommand Cyberpunk has dealt with
		std::uint32_t cmdResult;  // CommandResult for that command
	};
	static_assert(sizeof(GameState) == 0x30);

	// ---- Minecraft -> Cyberpunk command @0x300 (seqlock: seq is odd while being written) -----
	// A latest-value slot. Minecraft writes the fields and bumps seq by 2 (even); Cyberpunk acts on a
	// command whenever it sees a new even, non-zero seq, then copies seq into GameState::cmdAck.
	enum McCommandKind : std::uint32_t
	{
		kCmdNone = 0,
		kCmdTeleport = 1,  // move V to (x, y, z), Minecraft coordinates
	};

	enum CommandResult : std::uint32_t
	{
		kResultNone = 0,
		kResultOk = 1,
		kResultFailed = 2,
	};

	struct McCommand
	{
		std::uint32_t seq;
		std::uint32_t kind;  // McCommandKind
		double        x, y, z;
	};
	static_assert(sizeof(McCommand) == 0x20);
}
