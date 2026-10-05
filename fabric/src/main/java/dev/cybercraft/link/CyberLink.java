package dev.cybercraft.link;

import static dev.cybercraft.link.Proto.*;
import static java.lang.foreign.ValueLayout.*;

import dev.cybercraft.CyberCraft;
import java.lang.foreign.Arena;
import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.Linker;
import java.lang.foreign.MemoryLayout;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.StructLayout;
import java.lang.foreign.SymbolLookup;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.VarHandle;
import java.nio.charset.StandardCharsets;

/**
 * The Minecraft end of the shared-memory link. Cyberpunk (the RED4ext plugin) creates the mapping; we
 * open it when it appears and treat it as gone when Cyberpunk's heartbeat stops.
 *
 * The Windows calls follow the same pattern SkyCraft's SkyLink uses (MIT, by chasmlol).
 */
public final class CyberLink {
	private static final int FILE_MAP_ALL_ACCESS = 0xF001F;
	// Loading screens can stall the game's heartbeat for several seconds.
	private static final long HEARTBEAT_TIMEOUT_MS = 8000;

	private static final MethodHandle OPEN_FILE_MAPPING;
	private static final MethodHandle MAP_VIEW_OF_FILE;
	private static final MethodHandle GET_TICK_COUNT64;
	private static final MethodHandle GET_CURRENT_PROCESS_ID;
	// OpenFileMappingW's GetLastError, captured right after the call.
	private static final StructLayout CALL_STATE = Linker.Option.captureStateLayout();
	private static final VarHandle LAST_ERROR = CALL_STATE.varHandle(MemoryLayout.PathElement.groupElement("GetLastError"));
	private static final MemorySegment OPEN_STATE = Arena.global().allocate(CALL_STATE);
	private static int lastOpenError = -1;

	static {
		Linker linker = Linker.nativeLinker();
		SymbolLookup k32 = SymbolLookup.libraryLookup("kernel32", Arena.global());
		OPEN_FILE_MAPPING = linker.downcallHandle(
			k32.find("OpenFileMappingW").orElseThrow(), FunctionDescriptor.of(ADDRESS, JAVA_INT, JAVA_INT, ADDRESS), Linker.Option.captureCallState("GetLastError")
		);
		MAP_VIEW_OF_FILE = linker.downcallHandle(
			k32.find("MapViewOfFile").orElseThrow(), FunctionDescriptor.of(ADDRESS, ADDRESS, JAVA_INT, JAVA_INT, JAVA_INT, JAVA_LONG)
		);
		GET_TICK_COUNT64 = linker.downcallHandle(k32.find("GetTickCount64").orElseThrow(), FunctionDescriptor.of(JAVA_LONG));
		GET_CURRENT_PROCESS_ID = linker.downcallHandle(k32.find("GetCurrentProcessId").orElseThrow(), FunctionDescriptor.of(JAVA_INT));
	}

	private static long mcFrame;

	private static final VarHandle INT_VH = JAVA_INT.varHandle();
	private static final VarHandle LONG_VH = JAVA_LONG.varHandle();
	private static int overlayBack = 1; // Minecraft's private overlay slot; Cyberpunk's front slot starts at 2, the middle at 0

	private static volatile MemorySegment shm;
	private static long lastOpenAttempt;
	private static int gamePid;

	private CyberLink() {
	}

	/** Plain snapshot of the game state Cyberpunk publishes. */
	public static final class GameState {
		public int flags;
		public double x, y, z; // Minecraft coordinates
		public long frame;
		public int cmdAck; // seq of the last command Cyberpunk dealt with
		public int cmdResult; // Proto.RESULT_*
		public float lookYaw, lookPitch; // where the player looks while input is routed (Minecraft degrees)
		public float verticalOffset; // Minecraft Y = Cyberpunk Z - this

		public boolean inGame() {
			return (this.flags & GAME_IN_GAME) != 0;
		}

		/** Cyberpunk is sending the keyboard and mouse to Minecraft right now. */
		public boolean routing() {
			return (this.flags & GAME_ROUTING) != 0;
		}
	}

	public static long tickCount() {
		try {
			return (long) GET_TICK_COUNT64.invokeExact();
		} catch (Throwable t) {
			throw new RuntimeException(t);
		}
	}

	/** True when a live Cyberpunk is on the other end. Cheap; safe from any thread. */
	public static boolean active() {
		MemorySegment s = shm;
		if (s == null) {
			return false;
		}
		long beat = s.get(JAVA_LONG, OFF_HEADER + H_GAME_HEARTBEAT);
		VarHandle.acquireFence();
		return tickCount() - beat < HEARTBEAT_TIMEOUT_MS;
	}

	public static int gamePid() {
		return gamePid;
	}

	/** Call every client tick: writes our heartbeat, and tries to open the mapping (once a second) until it works. */
	public static void poll() {
		MemorySegment s = shm;
		if (s != null) {
			s.set(JAVA_LONG, OFF_HEADER + H_MC_HEARTBEAT, tickCount());
			VarHandle.releaseFence();
			int pid = s.get(JAVA_INT, OFF_HEADER + H_GAME_PID);
			if (pid != gamePid) {
				// Cyberpunk restarted and made a fresh mapping; drop ours and open the new one.
				CyberCraft.LOG.info("CyberCraft: Cyberpunk instance changed (pid {} -> {}), reconnecting", gamePid, pid);
				gamePid = pid;
				shm = null;
			}
			return;
		}
		long now = System.currentTimeMillis();
		if (now - lastOpenAttempt < 1000) {
			return;
		}
		lastOpenAttempt = now;
		try (Arena arena = Arena.ofConfined()) {
			MemorySegment name = arena.allocateFrom(MAPPING_NAME, StandardCharsets.UTF_16LE);
			MemorySegment handle = (MemorySegment) OPEN_FILE_MAPPING.invokeExact(OPEN_STATE, FILE_MAP_ALL_ACCESS, 0, name);
			if (handle.address() == 0) {
				// Say why, once per reason: 2 is "the game hasn't made it yet" (normal), 5 is "not allowed".
				int error = (int) LAST_ERROR.get(OPEN_STATE, 0L);
				if (error != lastOpenError) {
					lastOpenError = error;
					CyberCraft.LOG.info("CyberCraft: can't open Cyberpunk's shared memory yet (Windows error {}{})", error,
						error == 2 ? ": the CyberCraft plugin hasn't created it yet; start Cyberpunk" : error == 5 ? ": access denied; is Cyberpunk running as administrator?" : "");
				}
				return;
			}
			MemorySegment view = (MemorySegment) MAP_VIEW_OF_FILE.invokeExact(handle, FILE_MAP_ALL_ACCESS, 0, 0, 0L);
			if (view.address() == 0) {
				CyberCraft.LOG.error("CyberCraft: MapViewOfFile failed");
				return;
			}
			MemorySegment seg = view.reinterpret(MAPPING_BYTES);
			int magic = seg.get(JAVA_INT, OFF_HEADER + H_MAGIC);
			int version = seg.get(JAVA_INT, OFF_HEADER + H_VERSION);
			if (magic != MAGIC || version != VERSION) {
				CyberCraft.LOG.error("CyberCraft: protocol mismatch (magic {} version {}); expected version {}", Integer.toHexString(magic), version, VERSION);
				return;
			}
			seg.set(JAVA_INT, OFF_HEADER + H_MC_PID, (int) GET_CURRENT_PROCESS_ID.invokeExact());
			seg.set(JAVA_LONG, OFF_HEADER + H_MC_HEARTBEAT, tickCount());
			VarHandle.releaseFence();
			gamePid = seg.get(JAVA_INT, OFF_HEADER + H_GAME_PID);
			// Our private overlay slot is the one that is neither Cyberpunk's front slot nor the middle one.
			int middle = seg.get(JAVA_INT, OFF_OVERLAY_CTL + OC_STATE) & 3;
			int front = seg.get(JAVA_INT, OFF_OVERLAY_CTL + OC_FRONT);
			int back = 3 - middle - front;
			overlayBack = front >= 0 && front <= 2 && middle <= 2 && front != middle && back >= 0 && back <= 2 ? back : 1;
			// Skip anything Cyberpunk queued before we got here.
			seg.set(JAVA_LONG, OFF_INPUT_RING + IR_TAIL, seg.get(JAVA_LONG, OFF_INPUT_RING + IR_HEAD));
			shm = seg;
			lastOpenError = -1;
			CyberCraft.LOG.info("CyberCraft: linked to Cyberpunk (pid {})", gamePid);
		} catch (Throwable t) {
			CyberCraft.LOG.error("CyberCraft: failed to open shared memory", t);
		}
	}

	/** The game's camera, in Minecraft coordinates and degrees. */
	public static final class CameraState {
		public boolean valid;
		public double x, y, z;
		public float yaw, pitch;
		public float vfov; // vertical field of view, degrees
		public float aspect; // width / height of the game's screen
		public float velX, velY, velZ; // how fast the camera moves, blocks per second
		public float yawRate, pitchRate; // how fast it turns, degrees per second
		public float roll; // degrees
		public long frame; // the plugin's counter for this camera
	}

	/** Seqlock read of the game's camera into {@code out}. Returns false if the link is down or the read kept failing. */
	public static boolean readCamera(CameraState out) {
		MemorySegment s = shm;
		if (s == null) {
			return false;
		}
		for (int attempt = 0; attempt < 8; attempt++) {
			int seq1 = s.get(JAVA_INT, OFF_CAMERA + CAM_SEQ);
			if ((seq1 & 1) != 0) {
				continue;
			}
			VarHandle.acquireFence();
			out.valid = (s.get(JAVA_INT, OFF_CAMERA + CAM_FLAGS) & CAMERA_VALID) != 0;
			out.x = s.get(JAVA_DOUBLE, OFF_CAMERA + CAM_X);
			out.y = s.get(JAVA_DOUBLE, OFF_CAMERA + CAM_Y);
			out.z = s.get(JAVA_DOUBLE, OFF_CAMERA + CAM_Z);
			out.yaw = s.get(JAVA_FLOAT, OFF_CAMERA + CAM_YAW);
			out.pitch = s.get(JAVA_FLOAT, OFF_CAMERA + CAM_PITCH);
			out.vfov = s.get(JAVA_FLOAT, OFF_CAMERA + CAM_VFOV);
			out.aspect = s.get(JAVA_FLOAT, OFF_CAMERA + CAM_ASPECT);
			out.velX = s.get(JAVA_FLOAT, OFF_CAMERA + CAM_VEL_X);
			out.velY = s.get(JAVA_FLOAT, OFF_CAMERA + CAM_VEL_Y);
			out.velZ = s.get(JAVA_FLOAT, OFF_CAMERA + CAM_VEL_Z);
			out.yawRate = s.get(JAVA_FLOAT, OFF_CAMERA + CAM_YAW_RATE);
			out.pitchRate = s.get(JAVA_FLOAT, OFF_CAMERA + CAM_PITCH_RATE);
			out.roll = s.get(JAVA_FLOAT, OFF_CAMERA + CAM_ROLL);
			out.frame = s.get(JAVA_LONG, OFF_CAMERA + CAM_FRAME);
			VarHandle.acquireFence();
			if (seq1 == s.get(JAVA_INT, OFF_CAMERA + CAM_SEQ)) {
				return true;
			}
		}
		return false;
	}

	/** Receives one keyboard or mouse event from Cyberpunk. */
	public interface InputSink {
		void accept(int type, int code, int a, int b, int c);
	}

	/** Hands every input event Cyberpunk has queued since last time to {@code sink}. */
	public static void drainInput(InputSink sink) {
		MemorySegment s = shm;
		if (s == null) {
			return;
		}
		long head = (long) LONG_VH.getAcquire(s, OFF_INPUT_RING + IR_HEAD);
		long tail = s.get(JAVA_LONG, OFF_INPUT_RING + IR_TAIL);
		if (head - tail > INPUT_RING_ENTRIES) {
			tail = head - INPUT_RING_ENTRIES; // Cyberpunk lapped us: drop the oldest
		}
		while (tail < head) {
			long e = OFF_INPUT_RING + IR_DATA + (tail & (INPUT_RING_ENTRIES - 1)) * 16L;
			int type = Short.toUnsignedInt(s.get(JAVA_SHORT, e));
			int code = Short.toUnsignedInt(s.get(JAVA_SHORT, e + 2));
			int a = s.get(JAVA_INT, e + 4);
			int b = s.get(JAVA_INT, e + 8);
			int c = s.get(JAVA_INT, e + 12);
			tail++;
			sink.accept(type, code, a, b, c);
		}
		LONG_VH.setRelease(s, OFF_INPUT_RING + IR_TAIL, tail);
	}

	/** The shared memory, once opened (whether or not Cyberpunk is still alive). */
	public static MemorySegment segment() {
		return shm;
	}

	/** The byte offset where the next overlay frame should be written. */
	public static long overlayBackSlotOffset() {
		return OFF_OVERLAY_PIXELS + overlayBack * OVERLAY_SLOT_BYTES;
	}

	/** Hands the frame just written at {@link #overlayBackSlotOffset()} to Cyberpunk. */
	public static void publishOverlay(int width, int height, boolean bottomUp, long frameId, long cameraFrame) {
		publishOverlay(width, height, bottomUp ? SHF_BOTTOM_UP : 0, frameId, cameraFrame, 0.0f, 0.0f, 0.0f, 0.0f);
	}

	/** As above, with the slot's flag bits (SHF_*) and Minecraft's projection numbers, for a layered frame. */
	public static void publishOverlay(int width, int height, int flags, long frameId, long cameraFrame, float mcA, float mcB, float mcNear, float mcFar) {
		MemorySegment s = shm;
		if (s == null) {
			return;
		}
		long hdr = OFF_OVERLAY_SLOT_HDR + overlayBack * SLOT_HDR_SIZE;
		s.set(JAVA_INT, hdr + SH_WIDTH, width);
		s.set(JAVA_INT, hdr + SH_HEIGHT, height);
		s.set(JAVA_INT, hdr + SH_FLAGS, flags);
		s.set(JAVA_LONG, hdr + SH_FRAME_ID, frameId);
		s.set(JAVA_LONG, hdr + SH_CAMERA_FRAME, cameraFrame);
		s.set(JAVA_FLOAT, hdr + SH_MC_A, mcA);
		s.set(JAVA_FLOAT, hdr + SH_MC_B, mcB);
		s.set(JAVA_FLOAT, hdr + SH_MC_NEAR, mcNear);
		s.set(JAVA_FLOAT, hdr + SH_MC_FAR, mcFar);
		int old = (int) INT_VH.getAndSet(s, OFF_OVERLAY_CTL + OC_STATE, overlayBack | OVERLAY_DIRTY);
		overlayBack = old & 3;
		LONG_VH.getAndAdd(s, OFF_OVERLAY_CTL + OC_FRAMES_PUBLISHED, 1L);
	}

	/**
	 * Tells Cyberpunk what Minecraft wants. With {@code follow} set, Cyberpunk keeps moving V to (x, y, z)
	 * (the same coordinate space as the game state position) and turns V to face {@code yaw}.
	 */
	public static void publishMcState(boolean inWorld, boolean follow, boolean screenOpen, int camSource, boolean depthProbe, boolean depthCapture, int debugView, boolean noWarp, float warpDelayMs, float sensitivity, double x, double y, double z, float yaw, float pitch) {
		MemorySegment s = shm;
		if (s == null) {
			return;
		}
		int base = s.get(JAVA_INT, OFF_MC_STATE + M_SEQ) & ~1;
		s.set(JAVA_INT, OFF_MC_STATE + M_SEQ, base + 1); // odd: write in progress
		VarHandle.releaseFence();
		s.set(JAVA_INT, OFF_MC_STATE + M_FLAGS, (inWorld ? MC_IN_WORLD : 0) | (follow ? MC_FOLLOW : 0) | (screenOpen ? MC_SCREEN_OPEN : 0) | ((camSource & 3) << MC_CAM_SOURCE_SHIFT) | (depthProbe ? MC_DEPTH_PROBE : 0) | (depthCapture ? MC_DEPTH_CAPTURE : 0) | ((debugView & 3) << MC_DEBUG_SHIFT) | (noWarp ? MC_NO_WARP : 0));
		s.set(JAVA_DOUBLE, OFF_MC_STATE + M_X, x);
		s.set(JAVA_DOUBLE, OFF_MC_STATE + M_Y, y);
		s.set(JAVA_DOUBLE, OFF_MC_STATE + M_Z, z);
		s.set(JAVA_FLOAT, OFF_MC_STATE + M_YAW, yaw);
		s.set(JAVA_FLOAT, OFF_MC_STATE + M_PITCH, pitch);
		s.set(JAVA_LONG, OFF_MC_STATE + M_FRAME, ++mcFrame);
		s.set(JAVA_FLOAT, OFF_MC_STATE + M_SENSITIVITY, sensitivity);
		s.set(JAVA_FLOAT, OFF_MC_STATE + M_WARP_DELAY, warpDelayMs);
		VarHandle.releaseFence();
		s.set(JAVA_INT, OFF_MC_STATE + M_SEQ, base + 2); // even: done
	}

	/**
	 * Asks Cyberpunk to teleport V to a position (Minecraft coordinates). Returns the command's sequence
	 * number (watch for it in GameState.cmdAck), or 0 if the link is down.
	 */
	public static int sendTeleport(double x, double y, double z) {
		MemorySegment s = shm;
		if (s == null) {
			return 0;
		}
		int base = s.get(JAVA_INT, OFF_MC_COMMAND + C_SEQ) & ~1; // the last finished value (even)
		s.set(JAVA_INT, OFF_MC_COMMAND + C_SEQ, base + 1); // odd: write in progress
		VarHandle.releaseFence();
		s.set(JAVA_INT, OFF_MC_COMMAND + C_KIND, CMD_TELEPORT);
		s.set(JAVA_DOUBLE, OFF_MC_COMMAND + C_X, x);
		s.set(JAVA_DOUBLE, OFF_MC_COMMAND + C_Y, y);
		s.set(JAVA_DOUBLE, OFF_MC_COMMAND + C_Z, z);
		VarHandle.releaseFence();
		s.set(JAVA_INT, OFF_MC_COMMAND + C_SEQ, base + 2); // even: done
		return base + 2;
	}

	/**
	 * The ground height Cyberpunk found for the Minecraft block column (bx, bz): the height in blocks,
	 * {@code Float.NEGATIVE_INFINITY} if it looked and found no ground, or {@code NaN} if that cell hasn't
	 * been scanned (yet).
	 */
	public static float groundHeight(int bx, int bz) {
		return slotValue(bx, bz, 0, Float.NaN);
	}

	/**
	 * Which 0.125 m sub-squares of the Minecraft block column (bx, bz) have something standing in the way (a
	 * wall, tree or post): bit {@code sx + 8 * sz}, counted from the cell's low corner. 0 if nothing is in the
	 * way or the cell hasn't been looked at.
	 */
	public static long obstacleMask(int bx, int bz) {
		MemorySegment s = shm;
		if (s == null) {
			return 0;
		}
		int ix = Math.floorMod(bx, GROUND_N);
		int iz = Math.floorMod(bz, GROUND_N);
		long slot = OFF_GROUND + ((long) iz * GROUND_N + ix) * 3 * 8L;
		for (int attempt = 0; attempt < 4; attempt++) {
			long id = s.get(JAVA_LONG, slot + 8);
			if ((short) (id >>> 32) != (short) bx || (short) (id >>> 48) != (short) bz) {
				return 0; // the slot holds some other cell
			}
			VarHandle.acquireFence();
			long mask = s.get(JAVA_LONG, slot + 16);
			long check = (mask ^ (mask >>> 16) ^ (mask >>> 32) ^ (mask >>> 48)) & 0xFFFFL;
			if (check == (id & 0xFFFFL)) {
				return mask;
			}
			// The plugin was in the middle of updating this cell: look again.
		}
		return 0;
	}

	private static float slotValue(int bx, int bz, int word, float unknown) {
		MemorySegment s = shm;
		if (s == null) {
			return unknown;
		}
		int ix = Math.floorMod(bx, GROUND_N);
		int iz = Math.floorMod(bz, GROUND_N);
		long v = s.get(JAVA_LONG, OFF_GROUND + (((long) iz * GROUND_N + ix) * 3 + word) * 8L);
		if ((short) (v >>> 32) != (short) bx || (short) (v >>> 48) != (short) bz) {
			return unknown; // the slot holds some other cell
		}
		float value = Float.intBitsToFloat((int) v);
		return value < -1.0e29f ? Float.NEGATIVE_INFINITY : value;
	}

	/** Asks Cyberpunk to set the vertical offset so the street under V lands on a whole-number height. Returns the command's number (0: link down). */
	public static int sendAlignGround() {
		MemorySegment s = shm;
		if (s == null) {
			return 0;
		}
		int base = s.get(JAVA_INT, OFF_MC_COMMAND + C_SEQ) & ~1;
		s.set(JAVA_INT, OFF_MC_COMMAND + C_SEQ, base + 1);
		VarHandle.releaseFence();
		s.set(JAVA_INT, OFF_MC_COMMAND + C_KIND, CMD_ALIGN_GROUND);
		s.set(JAVA_DOUBLE, OFF_MC_COMMAND + C_X, 0.0);
		s.set(JAVA_DOUBLE, OFF_MC_COMMAND + C_Y, 0.0);
		s.set(JAVA_DOUBLE, OFF_MC_COMMAND + C_Z, 0.0);
		VarHandle.releaseFence();
		s.set(JAVA_INT, OFF_MC_COMMAND + C_SEQ, base + 2);
		return base + 2;
	}

	/** Seqlock read of the game state into {@code out}. Returns false if the link is down or the writer was mid-update every try. */
	public static boolean readGameState(GameState out) {
		MemorySegment s = shm;
		if (s == null) {
			return false;
		}
		for (int attempt = 0; attempt < 8; attempt++) {
			int seq1 = s.get(JAVA_INT, OFF_GAME_STATE + G_SEQ);
			if ((seq1 & 1) != 0) {
				continue; // being written
			}
			VarHandle.acquireFence();
			out.flags = s.get(JAVA_INT, OFF_GAME_STATE + G_FLAGS);
			out.x = s.get(JAVA_DOUBLE, OFF_GAME_STATE + G_X);
			out.y = s.get(JAVA_DOUBLE, OFF_GAME_STATE + G_Y);
			out.z = s.get(JAVA_DOUBLE, OFF_GAME_STATE + G_Z);
			out.frame = s.get(JAVA_LONG, OFF_GAME_STATE + G_FRAME);
			out.cmdAck = s.get(JAVA_INT, OFF_GAME_STATE + G_CMD_ACK);
			out.cmdResult = s.get(JAVA_INT, OFF_GAME_STATE + G_CMD_RESULT);
			out.lookYaw = s.get(JAVA_FLOAT, OFF_GAME_STATE + G_LOOK_YAW);
			out.lookPitch = s.get(JAVA_FLOAT, OFF_GAME_STATE + G_LOOK_PITCH);
			out.verticalOffset = s.get(JAVA_FLOAT, OFF_GAME_STATE + G_VERT_OFFSET);
			VarHandle.acquireFence();
			int seq2 = s.get(JAVA_INT, OFF_GAME_STATE + G_SEQ);
			if (seq1 == seq2) {
				return true;
			}
		}
		return false;
	}
}
