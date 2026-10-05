package dev.cybercraft.link;

/**
 * Byte layout of the shared memory. Mirrors protocol/cybercraft_protocol.h, which is the source of
 * truth: change both together and bump VERSION.
 */
public final class Proto {
	public static final int MAGIC = 0x43425943; // "CYBC"
	public static final int VERSION = 8;
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

	public static final int GAME_IN_GAME = 1;

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

	public static final int MC_IN_WORLD = 1;
	public static final int MC_FOLLOW = 2;

	// ---- Minecraft -> Cyberpunk command @0x300 (seqlock) ----
	public static final long OFF_MC_COMMAND = 0x300;
	public static final long C_SEQ = 0; // u32, odd while being written; Cyberpunk acts on each new even value
	public static final long C_KIND = 4; // u32
	public static final long C_X = 8; // double, Minecraft coordinates
	public static final long C_Y = 16;
	public static final long C_Z = 24;

	public static final int CMD_TELEPORT = 1;

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
	public static final long OFF_OVERLAY_PIXELS = 0x20000;
	public static final int MAX_OVERLAY_W = 3840;
	public static final int MAX_OVERLAY_H = 2160;
	public static final long OVERLAY_SLOT_BYTES = (long) MAX_OVERLAY_W * MAX_OVERLAY_H * 4;
	public static final int OVERLAY_SLOTS = 3;
	public static final int OVERLAY_DIRTY = 1 << 2;

	public static final long MAPPING_BYTES = OFF_OVERLAY_PIXELS + OVERLAY_SLOT_BYTES * OVERLAY_SLOTS;

	private Proto() {
	}
}
