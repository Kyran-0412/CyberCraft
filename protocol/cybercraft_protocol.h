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
	inline constexpr std::uint32_t kVersion = 7;
	inline constexpr wchar_t       kMappingName[] = L"Local\\CyberCraft_v1";

	// Cyberpunk uses metres and Minecraft blocks are 1 m, so no scaling is needed.
	// Axes: Cyberpunk is Z-up (X east, Y north). Minecraft is Y-up (X east, -Z north).
	//   mc.x = cp.x,   mc.y = cp.z,   mc.z = -cp.y
	inline constexpr double kUnitsPerBlock = 1.0;

	// ---- region offsets ---------------------------------------------------------------------
	inline constexpr std::uint64_t kOffHeader = 0x0;
	inline constexpr std::uint64_t kOffGameState = 0x100;  // Cyberpunk -> Minecraft
	inline constexpr std::uint64_t kOffMcState = 0x200;    // Minecraft -> Cyberpunk: where Minecraft wants V
	inline constexpr std::uint64_t kOffMcCommand = 0x300;  // Minecraft -> Cyberpunk: one-off commands
	inline constexpr std::uint64_t kOffGround = 0x1000;   // Cyberpunk -> Minecraft: ground heights, see GroundSlot
	inline constexpr std::uint32_t kGroundN = 64;         // the ground grid is kGroundN x kGroundN cells (a torus)
	inline constexpr std::int32_t  kGroundRadius = 28;    // cells scanned around V (must be < kGroundN / 2)
	inline constexpr std::uint64_t kGroundSlotBytes = 24; // three 64-bit words per cell
	inline constexpr std::uint64_t kMappingBytes = kOffGround + std::uint64_t(kGroundN) * kGroundN * kGroundSlotBytes;

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

	// ---- Minecraft -> Cyberpunk state @0x200 (seqlock: seq is odd while being written) ------
	enum McFlags : std::uint32_t
	{
		kMcInWorld = 1u << 0,  // Minecraft has a world open and a player in it
		kMcFollow = 1u << 1,   // Cyberpunk should keep moving V to (targetX, targetY, targetZ)
	};

	struct McState
	{
		std::uint32_t seq;
		std::uint32_t flags;  // McFlags
		double        targetX, targetY, targetZ;  // where Minecraft wants V's feet (same space as GameState::pos)
		float         yaw, pitch;                 // Minecraft's look direction, Minecraft degrees
		std::uint64_t frameCounter;
	};
	static_assert(sizeof(McState) == 0x30);

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

	// ---- ground heights and obstacles @0x1000 -------------------------------------------------
	// Cyberpunk looks at the street around V and stores what it finds, one 1 m x 1 m cell (= one Minecraft
	// block column) per slot. The grid wraps around (a torus): the cell (bx, bz) lives in slot
	// ((bz mod N) * N + (bx mod N)), and each word says which cell it currently holds, so Minecraft can tell
	// fresh data from old data left over from another place. Each slot is three 64-bit words, each written
	// atomically, so no lock is needed.
	//
	//   word 0  (the ground)
	//     bits  0..31  float  height of the ground surface (Minecraft Y = Cyberpunk Z), or kNoGround
	//     bits 32..47  int16  bx   Minecraft block X of the cell
	//     bits 48..63  int16  bz   Minecraft block Z of the cell
	//   word 1  (says which cell the obstacle mask in word 2 belongs to)
	//     bits  0..15  check  the mask's four 16-bit quarters XORed together (to catch a half-written pair)
	//     bits 16..31  unused
	//     bits 32..47  int16  bx
	//     bits 48..63  int16  bz
	//   word 2  (anything standing on the ground that is in the way)
	//     a 64-bit mask of the cell's 8 x 8 sub-squares (0.125 m each): bit (sx + 8 * sz), sx along X and sz along
	//     Z, both counted from the cell's low corner. 0: nothing in the way.
	// A blocked sub-square is solid from the ground up to kObstacleHeight above the cell's ground height.
	// Writers store word 2 first, then word 1; readers read word 1, then word 2, and retry if the check is wrong.
	//
	// Cell (bx, bz) is the square X in [bx, bx+1), Z in [bz, bz+1) in Minecraft coordinates.
	inline constexpr float kNoGround = -1.0e30f;
	inline constexpr int   kObstacleSub = 8;            // sub-squares per cell side
	inline constexpr float kObstacleHeight = 2.5f;      // how tall a blocked sub-square is
}
