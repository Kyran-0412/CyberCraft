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
	inline constexpr std::uint32_t kVersion = 22;
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
	inline constexpr std::uint64_t kOffCamera = 0x400;     // Cyberpunk -> Minecraft: the game's camera, see CameraState
	inline constexpr std::uint64_t kOffGround = 0x1000;   // Cyberpunk -> Minecraft: ground heights, see GroundSlot
	inline constexpr std::uint32_t kGroundN = 64;         // the ground grid is kGroundN x kGroundN cells (a torus)
	inline constexpr std::int32_t  kGroundRadius = 28;    // cells scanned around V (must be < kGroundN / 2)
	inline constexpr std::uint64_t kGroundSlotBytes = 24; // three 64-bit words per cell
	static_assert(kOffGround + std::uint64_t(kGroundN) * kGroundN * kGroundSlotBytes <= 0x19000, "the ground grid must end before the overlay control block");
	inline constexpr std::uint64_t kOffOverlayCtl = 0x19000;      // Minecraft -> Cyberpunk: which overlay frame is newest, see OverlayCtl
	inline constexpr std::uint64_t kOffOverlaySlotHdr = 0x19040;  // 3 x 0x40: size and flags of each overlay slot
	inline constexpr std::uint64_t kOffOverlayPixels = 0x20000;   // 3 slots of RGBA pixels
	inline constexpr std::uint32_t kMaxOverlayW = 3840;
	inline constexpr std::uint32_t kMaxOverlayH = 2160;
	// A slot holds up to three pictures of kMaxOverlayW x kMaxOverlayH x 4 bytes each: layer 0 is the whole frame (or, in a layered
	// frame, the world: Minecraft's blocks and entities), layer 1 is the world's depth (one 32-bit float per pixel) and layer 2 is the
	// overlay (hand, hotbar, screens). See OverlaySlotHdr::flags.
	inline constexpr std::uint64_t kOverlayLayerBytes = std::uint64_t(kMaxOverlayW) * kMaxOverlayH * 4;
	inline constexpr std::uint64_t kOverlaySlotBytes = kOverlayLayerBytes * 3;
	inline constexpr std::uint32_t kOverlaySlots = 3;
	// The collision boxes of what is built in Minecraft sit after the overlay pixels (see BoxTableHdr).
	inline constexpr std::uint64_t kOffBoxTable = kOffOverlayPixels + kOverlaySlotBytes * kOverlaySlots;
	inline constexpr std::uint32_t kBoxTableMax = 4096;
	inline constexpr std::uint64_t kBoxTableEntriesOff = 0x40;
	inline constexpr std::uint64_t kBoxEntryBytes = 24;
	inline constexpr std::uint64_t kBoxTableBytes = kBoxTableEntriesOff + std::uint64_t(kBoxTableMax) * kBoxEntryBytes;
	inline constexpr std::uint64_t kMappingBytes = kOffBoxTable + kBoxTableBytes;

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
		kGameInGame = 1u << 0,   // a save is loaded and the player exists
		kGameRouting = 1u << 1,  // Cyberpunk is sending the keyboard and mouse to Minecraft (see the input ring)
	};

	struct GameState
	{
		std::uint32_t seq;
		std::uint32_t flags;  // GameFlags
		double        posX, posY, posZ;  // V's feet, Minecraft coordinates
		std::uint64_t frameCounter;
		std::uint32_t cmdAck;     // seq of the last McCommand Cyberpunk has dealt with
		std::uint32_t cmdResult;  // CommandResult for that command
		float         lookYaw, lookPitch;  // where the player is looking while input is routed, Minecraft degrees
		float         verticalOffset;      // Minecraft Y = Cyberpunk Z - verticalOffset (see /ccalign)
		float         pad;
	};
	static_assert(sizeof(GameState) == 0x40);

	// ---- Minecraft -> Cyberpunk state @0x200 (seqlock: seq is odd while being written) ------
	enum McFlags : std::uint32_t
	{
		kMcInWorld = 1u << 0,  // Minecraft has a world open and a player in it
		kMcFollow = 1u << 1,   // Cyberpunk should keep moving V to (targetX, targetY, targetZ)
		kMcScreenOpen = 1u << 2,  // a Minecraft screen (inventory, chat, ...) is open: the mouse moves a cursor instead of looking
		// bits 3-4: which camera source the plugin should publish (see CamSource)
		kMcDepthProbe = 1u << 5,    // watch the game's depth textures and log what is seen (/ccdepthprobe)
		kMcDepthCapture = 1u << 6,  // copy the game's main depth texture and check it against the game's rays (/ccdepthcapture)
		// bits 7-8: depth debug view (see kMcDebugShift): 0 off, 1 the game's depth, 2 the blocks' distance, 3 hidden (red) / shown (green)
		kMcNoWarp = 1u << 9,  // don't re-aim the blocks at the game's current camera when drawing (/ccwarp)
	};
	inline constexpr std::uint32_t kMcDebugShift = 7;
	inline constexpr std::uint32_t kMcCamSourceShift = 3;

	enum CamSource : std::uint32_t
	{
		kCamTransform = 0,  // the camera's world transform, as the camera system reports it
		kCamData = 1,       // the camera system's "active camera data"
		kCamProjected = 2,  // the camera's orientation worked out from where the game projects points to the screen
		kCamProjectedPos = 3,  // that, and the camera's position worked out the same way from points close to it
	};

	struct McState
	{
		std::uint32_t seq;
		std::uint32_t flags;  // McFlags
		double        targetX, targetY, targetZ;  // where Minecraft wants V's feet (same space as GameState::pos)
		float         yaw, pitch;                 // Minecraft's look direction, Minecraft degrees
		std::uint64_t frameCounter;
		float         sensitivity;                // Minecraft's mouse sensitivity option, 0 to 1
		float         warpDelayMs;                // how far behind the newest camera the game's picture is, for re-aiming the blocks (see kMcNoWarp)
	};
	static_assert(sizeof(McState) == 0x38);

	// ---- Minecraft -> Cyberpunk command @0x300 (seqlock: seq is odd while being written) -----
	// A latest-value slot. Minecraft writes the fields and bumps seq by 2 (even); Cyberpunk acts on a
	// command whenever it sees a new even, non-zero seq, then copies seq into GameState::cmdAck.
	enum McCommandKind : std::uint32_t
	{
		kCmdNone = 0,
		kCmdTeleport = 1,  // move V to (x, y, z), Minecraft coordinates
		kCmdAlignGround = 2,  // set the vertical offset so that the street under V lands on a whole-number height
		kCmdDebug = 3,        // a debug command (x: the DebugAction, y: its argument)
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

	// ---- overlay triple buffer @0x19000 -------------------------------------------------------
	// Minecraft draws its hotbar, hearts, held item and screens on a transparent background and copies
	// them (RGBA, premultiplied alpha) into one of three slots; Cyberpunk draws the newest one over the game.
	// state: bits 0-1 = index of the "middle" slot, bit 2 = the middle slot holds a frame not yet drawn.
	// Writer (Minecraft) renders into its private back slot, then exchanges state with (back | kOverlayDirty) and
	// keeps the returned index as its new back slot. Reader (Cyberpunk) exchanges state with its front index
	// only when the dirty bit is set, and keeps the returned index as its new front slot.
	// Start: middle = 0, Minecraft's back slot = 1, Cyberpunk's front slot = 2.
	inline constexpr std::uint32_t kOverlayDirty = 1u << 2;

	struct OverlayCtl
	{
		std::uint32_t state;
		std::uint32_t front;  // the slot Cyberpunk is showing now; Minecraft's own slot is the one that is neither this nor the middle
		std::uint64_t framesPublished;
	};

	struct OverlaySlotHdr
	{
		std::uint32_t width;
		std::uint32_t height;
		std::uint32_t flags;  // see OverlayFlags
		std::uint32_t pad;
		std::uint64_t frameId;
		std::uint64_t cameraFrame;  // CameraState::frameCounter of the camera Minecraft drew this frame through
		float         mcA;          // Minecraft's projection matrix m22 (layered frames): distance = mcB / (ndcDepth + mcA)
		float         mcB;          // ... and m32
		float         mcNear;
		float         mcFar;
		std::uint8_t  reserved[0x40 - 0x30];
	};
	static_assert(sizeof(OverlaySlotHdr) == 0x40);

	enum OverlayFlags : std::uint32_t
	{
		kOverlayBottomUp = 1u << 0,    // the rows of every layer are bottom-up (OpenGL order)
		kOverlayLayered = 1u << 1,     // three layers: world colour, world depth, overlay (otherwise layer 0 is the whole frame)
		kOverlayZeroToOne = 1u << 2,   // the depth values are normalised device depth (otherwise they are (depth + 1) / 2)
	};

	// ---- input ring @0x1A000 (Cyberpunk produces, Minecraft consumes) --------------------------
	// While input is routed, the plugin captures the keyboard and mouse from Cyberpunk's window and sends them
	// here; Minecraft replays them as if its own window had focus. head is written by Cyberpunk, tail by Minecraft.
	inline constexpr std::uint64_t kOffInputRing = 0x1A000;
	inline constexpr std::uint32_t kInputRingEntries = 1024;  // power of two
	inline constexpr std::uint64_t kInputRingHeadOff = 0x00;  // u64
	inline constexpr std::uint64_t kInputRingTailOff = 0x40;  // u64
	inline constexpr std::uint64_t kInputRingDataOff = 0x80;
	static_assert(kOffInputRing + kInputRingDataOff + std::uint64_t(kInputRingEntries) * 16 <= kOffOverlayPixels, "the input ring must end before the overlay pixels");

	enum InputType : std::uint16_t
	{
		kInKey = 1,          // code = SDL scancode, a = 1 press / 0 release
		kInMouseButton = 2,  // code = SDL button (1 left, 2 middle, 3 right, 4 and 5 the side buttons), a = 1 press / 0 release
		kInScroll = 3,       // a = wheel notches * 120 (positive = up)
		kInCursor = 4,       // a, b = the cursor's position in overlay pixels (used while a screen is open)
		kInText = 5,         // a = a Unicode code point typed
		kInReleaseAll = 6,   // let go of every key and button (input stopped being routed)
	};

	struct InputEvent
	{
		std::uint16_t type;  // InputType
		std::uint16_t code;
		std::int32_t  a, b, c;
	};
	static_assert(sizeof(InputEvent) == 16);

	// ---- the game's camera @0x400 (seqlock: seq is odd while being written) -------------------
	// Minecraft draws its blocks looking through this camera, so they line up with Night City.
	enum CameraFlags : std::uint32_t
	{
		kCameraValid = 1u << 0,  // the plugin could read the camera this frame
	};

	struct CameraState
	{
		std::uint32_t seq;
		std::uint32_t flags;  // CameraFlags
		double        posX, posY, posZ;  // the camera's position, Minecraft coordinates (blocks)
		float         yaw, pitch;        // where it looks, Minecraft degrees (yaw 0 = south, pitch positive = down)
		float         vfov;              // vertical field of view, degrees
		float         aspect;            // width / height of the game's screen
		std::uint64_t frameCounter;
		float         velX, velY, velZ;  // how fast the camera is moving, blocks per second (smoothed)
		float         yawRate, pitchRate;  // how fast it is turning, degrees per second (smoothed)
		float         roll;                // how far the camera is tilted sideways, degrees (positive = right side up)
	};
	static_assert(sizeof(CameraState) == 0x50);

	// What a kCmdDebug command asks the plugin to do (x of the command; y is its argument).
	enum DebugAction : int
	{
		kDebugDump = 0,   // write the classes behind the collision boxes to the log
		kDebugLog = 1,    // y: 1 detailed logging on, 0 off
		kDebugUi = 2,     // capture the next frame's drawing into the back buffer and summarise it in the log (where does the game draw its interface?)
		kDebugUiLayer = 3,  // y: the game's interface (HUD) over Minecraft's blocks: 0 off, 1 premultiplied alpha, 2 straight alpha, 3 show the captured layer alone, 4 a magenta shadow of it over everything
		kDebugUiScale = 4,  // y: the interface layer's stretch across, about the middle of the screen, in percent; z: its stretch down (0: the same as across)
		kDebugUiShift = 5,  // y, z: the interface layer's shift in pixels (x, y)
		kDebugScene = 6,    // y: 0 off, 1 to 3: draw a dim, bright or very bright square into the game's own HDR scene (the proof of concept), 4: draw Minecraft's blocks into it
		kDebugSceneGain = 7,   // y: the blocks' brightness in the game's HDR scene, in percent (100: a Minecraft white is 1.0 in the scene's units)
		kDebugSceneDelay = 8,  // y: how many milliseconds behind the newest published camera the blocks are aimed when they are drawn into the scene
		kDebugSceneGlow = 10,  // y: how much the brightest pixels of the blocks are boosted in the game's HDR scene, in percent of their brightness (300: up to four times as bright)
		kDebugFind = 9,   // y: up to six letters packed into the number; list the game's classes, enums and global functions with that in their name
	};

	// ---- collision boxes @kOffBoxTable (Minecraft -> Cyberpunk; seqlock: seq is odd while being written) -----------------
	// What is built in Minecraft near the player, as boxes: the plugin keeps invisible collision boxes in Night City exactly
	// where these are, so cars and people are stopped by builds. This is the complete list every time (not a list of changes),
	// so a closed Minecraft, a new world or a missed update all fix themselves: the plugin makes Night City match the list.
	enum BoxTableFlags : std::uint32_t
	{
		kBoxesEnabled = 1u << 0,  // Minecraft wants collision boxes (otherwise the plugin removes all of them)
	};

	struct BoxTableHdr
	{
		std::uint32_t seq;      // seqlock
		std::uint32_t count;    // boxes in the list
		std::uint32_t version;  // changes whenever the list changes
		std::uint32_t flags;    // BoxTableFlags
		std::uint32_t epoch;    // changes when Minecraft asks for every box to be rebuilt
		std::uint32_t reserved[11];
	};
	static_assert(sizeof(BoxTableHdr) == kBoxTableEntriesOff);

	// One box, in Minecraft's coordinates, in sixteenths of a block (min corner, max corner; the max is larger than the min).
	struct BoxEntry
	{
		std::int32_t min[3];  // x, y, z
		std::int32_t max[3];
	};
	static_assert(sizeof(BoxEntry) == kBoxEntryBytes);
}
