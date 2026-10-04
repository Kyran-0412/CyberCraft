package dev.cybercraft.link;

/**
 * Byte layout of the shared memory. Mirrors protocol/cybercraft_protocol.h, which is the source of
 * truth: change both together and bump VERSION.
 */
public final class Proto {
	public static final int MAGIC = 0x43425943; // "CYBC"
	public static final int VERSION = 1;
	public static final String MAPPING_NAME = "Local\\CyberCraft_v1";
	public static final long MAPPING_BYTES = 0x1000;

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

	public static final int GAME_IN_GAME = 1;

	private Proto() {
	}
}
