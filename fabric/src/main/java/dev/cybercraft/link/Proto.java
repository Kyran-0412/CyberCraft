package dev.cybercraft.link;

/**
 * Byte layout of the shared memory. Mirrors protocol/cybercraft_protocol.h, which is the source of
 * truth: change both together and bump VERSION.
 */
public final class Proto {
	public static final int MAGIC = 0x43425943; // "CYBC"
	public static final int VERSION = 20;
	public static final String MAPPING_NAME = "Local\\CyberCraft_v1";

	// ---- header @0x0 ----
	public static final long OFF_HEADER = 0x0;
	public static final long H_MAGIC = 0;
	public static final long H_VERSION = 4;
	public static final long H_GAME_PID = 8;
	public static final long H_MC_PID = 12;
	public static final long H_GAME_HEARTBEAT = 16; // u64, GetTickCount64
	public static final long H_MC_HEARTBEAT = 24; // u64, GetTickCount64

	// ---- Cyberpunk -> Minecraft state @0x100 (seqlock) ----
	public static final long OFF_GAME_STATE = 0x100;
	public static final long G_SEQ = 0; // u32, odd while being written
	public static final long G_FLAGS = 4; // u32
	public static final long G_X = 8; // double, Minecraft coordinates
	public static final long G_Y = 16;
	public static final long G_Z = 24;
	public static final long G_FRAME = 32; // u64
	public static final long G_CMD_ACK = 40; // u32, seq of the last command Cyberpunk dealt with
	public static final long G_CMD_RESULT = 44; // u32, RESULT_*
	public static final long G_LOOK_YAW = 48; // float, where the player looks while input is routed, Minecraft degrees
	public static final long G_LOOK_PITCH = 52; // float
	public static final long G_VERT_OFFSET = 56; // float: Minecraft Y = Cyberpunk Z - this (see /ccalign)

	public static final int GAME_IN_GAME = 1;
	public static final int GAME_ROUTING = 2; // Cyberpunk is sending the keyboard and mouse to Minecraft (the input ring)

	// ---- Minecraft -> Cyberpunk state @0x200 (seqlock): where Minecraft wants V ----
	public static final long OFF_MC_STATE = 0x200;
	public static final long M_SEQ = 0; // u32, odd while being written
	public static final long M_FLAGS = 4; // u32
	public static final long M_X = 8; // double, target for V's feet (same space as the game state position)
	public static final long M_Y = 16;
	public static final long M_Z = 24;
	public static final long M_YAW = 32; // float, Minecraft degrees
	public static final long M_PITCH = 36; // float
	public static final long M_FRAME = 40; // u64
	public static final long M_SENSITIVITY = 48; // float, Minecraft's mouse sensitivity option, 0 to 1
	public static final long M_WARP_DELAY = 52; // float, ms: how far behind the newest camera the game's picture is (what the blocks are re-aimed at)

	public static final int MC_IN_WORLD = 1;
	public static final int MC_FOLLOW = 2;
	public static final int MC_SCREEN_OPEN = 4; // a Minecraft screen (inventory, chat, ...) is open
	public static final int MC_DEPTH_PROBE = 1 << 5; // watch the game's depth textures and log what is seen
	public static final int MC_DEBUG_SHIFT = 7; // bits 7-8: depth debug view
	public static final int MC_NO_WARP = 1 << 9; // don't re-aim the blocks at the game's current camera
	public static final int MC_DEPTH_CAPTURE = 1 << 6; // copy the game's main depth texture and check it against the game's rays
	public static final int MC_CAM_SOURCE_SHIFT = 3; // bits 3-4: which camera the plugin should publish: 0 transform, 1 data, 2 projected

	// ---- Minecraft -> Cyberpunk command @0x300 (seqlock) ----
	public static final long OFF_MC_COMMAND = 0x300;
	public static final long C_SEQ = 0; // u32, odd while being written; Cyberpunk acts on each new even value
	public static final long C_KIND = 4; // u32
	public static final long C_X = 8; // double, Minecraft coordinates
	public static final long C_Y = 16;
	public static final long C_Z = 24;

	public static final int CMD_TELEPORT = 1;
	public static final int CMD_ALIGN_GROUND = 2;
	public static final int CMD_TEST_BOX = 3; // the collision experiment: x = 0 dump what is on offer, 1 spawn a test object, 2 remove them

	public static final int RESULT_NONE = 0;
	public static final int RESULT_OK = 1;
	public static final int RESULT_FAILED = 2;

	// ---- ground heights and obstacles @0x1000 (Cyberpunk -> Minecraft) ----
	// A GROUND_N x GROUND_N torus of 24-byte slots, one per 1 m cell (= Minecraft block column), each three 64-bit words:
	//   word 0 (ground):   bits 0..31 float height (Minecraft Y), bits 32..47 int16 bx, bits 48..63 int16 bz.
	//   word 1 (id):       bits 0..15 check (the mask's four 16-bit quarters XORed), then bx and bz as above.
	//   word 2 (obstacle): a 64-bit mask of blocked 0.125 m sub-squares (bit sx + 8 * sz, from the cell's low corner;
	//                      0 = nothing in the way).
	public static final long OFF_GROUND = 0x1000;
	public static final int GROUND_N = 64;
	public static final int GROUND_RADIUS = 28; // cells scanned around V
	public static final int OBSTACLE_SUB = 8; // sub-squares per cell side
	public static final double OBSTACLE_HEIGHT = 2.5; // how tall a blocked sub-square is

	// ---- overlay triple buffer (Minecraft -> Cyberpunk): the HUD, hand and screens as RGBA pixels ----
	// state: bits 0-1 = index of the "middle" slot, bit 2 = the middle slot holds a frame not yet drawn. Minecraft
	// renders into its private back slot, then exchanges state with (back | OVERLAY_DIRTY) and keeps the returned
	// index as its new back slot. At the start: middle = 0, Minecraft's back slot = 1, Cyberpunk's front slot = 2.
	public static final long OFF_OVERLAY_CTL = 0x19000;
	public static final long OC_STATE = 0; // u32
	public static final long OC_FRONT = 4; // u32, the slot Cyberpunk is showing
	public static final long OC_FRAMES_PUBLISHED = 8; // u64
	public static final long OFF_OVERLAY_SLOT_HDR = 0x19040;
	public static final long SLOT_HDR_SIZE = 0x40;
	public static final long SH_WIDTH = 0; // u32
	public static final long SH_HEIGHT = 4; // u32
	public static final long SH_FLAGS = 8; // u32, bit 0: rows are bottom-up
	public static final long SH_FRAME_ID = 16; // u64
	public static final long SH_CAMERA_FRAME = 24; // u64, the camera frame this picture was drawn through
	public static final long SH_MC_A = 32; // float: Minecraft's projection matrix m22 (layered frames): distance = B / (ndcDepth + A)
	public static final long SH_MC_B = 36; // float: m32
	public static final long SH_MC_NEAR = 40; // float
	public static final long SH_MC_FAR = 44; // float
	public static final int SHF_BOTTOM_UP = 1; // the rows of every layer are bottom-up
	public static final int SHF_LAYERED = 2; // three layers: world colour, world depth, overlay
	public static final int SHF_ZERO_TO_ONE = 4; // depth values are normalised device depth
	public static final long OFF_OVERLAY_PIXELS = 0x20000;
	public static final int MAX_OVERLAY_W = 3840;
	public static final int MAX_OVERLAY_H = 2160;
	public static final long OVERLAY_LAYER_BYTES = (long) MAX_OVERLAY_W * MAX_OVERLAY_H * 4;
	public static final long OVERLAY_SLOT_BYTES = OVERLAY_LAYER_BYTES * 3;
	public static final int OVERLAY_SLOTS = 3;
	public static final int OVERLAY_DIRTY = 1 << 2;

	public static final long MAPPING_BYTES = OFF_OVERLAY_PIXELS + OVERLAY_SLOT_BYTES * OVERLAY_SLOTS;

	// ---- the game's camera @0x400 (seqlock): Minecraft draws its blocks looking through it ----
	public static final long OFF_CAMERA = 0x400;
	public static final long CAM_SEQ = 0; // u32, odd while being written
	public static final long CAM_FLAGS = 4; // u32, bit 0: valid
	public static final long CAM_X = 8; // double, Minecraft coordinates
	public static final long CAM_Y = 16;
	public static final long CAM_Z = 24;
	public static final long CAM_YAW = 32; // float, Minecraft degrees (0 = south)
	public static final long CAM_PITCH = 36; // float, positive = down
	public static final long CAM_VFOV = 40; // float, vertical field of view in degrees
	public static final long CAM_ASPECT = 44; // float, width / height of the game's screen
	public static final long CAM_FRAME = 48; // u64
	public static final long CAM_VEL_X = 56; // float, blocks per second
	public static final long CAM_VEL_Y = 60;
	public static final long CAM_VEL_Z = 64;
	public static final long CAM_YAW_RATE = 68; // float, degrees per second
	public static final long CAM_PITCH_RATE = 72;
	public static final long CAM_ROLL = 76; // float, degrees: how far the camera is tilted sideways (positive = right side up)
	public static final int CAMERA_VALID = 1;

	// ---- input ring @0x1A000 (Cyberpunk produces, Minecraft consumes) ----
	// Entries are 16 bytes: u16 type, u16 code, i32 a, i32 b, i32 c.
	public static final long OFF_INPUT_RING = 0x1A000;
	public static final int INPUT_RING_ENTRIES = 1024;
	public static final long IR_HEAD = 0x00; // u64, written by Cyberpunk
	public static final long IR_TAIL = 0x40; // u64, written by Minecraft
	public static final long IR_DATA = 0x80;
	public static final int IN_KEY = 1; // code = SDL scancode, a = 1 press / 0 release
	public static final int IN_MOUSE_BUTTON = 2; // code = SDL button (1 left, 2 middle, 3 right, 4/5 side), a = 1 press / 0 release
	public static final int IN_SCROLL = 3; // a = wheel notches * 120 (positive = up)
	public static final int IN_CURSOR = 4; // a, b = cursor position in overlay pixels (while a screen is open)
	public static final int IN_TEXT = 5; // a = Unicode code point typed
	public static final int IN_RELEASE_ALL = 6; // let go of every key and button

	private Proto() {
	}
}
