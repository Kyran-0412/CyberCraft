package dev.cybercraft.client;

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

		public boolean inGame() {
			return (this.flags & GAME_IN_GAME) != 0;
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
			shm = seg;
			lastOpenError = -1;
			CyberCraft.LOG.info("CyberCraft: linked to Cyberpunk (pid {})", gamePid);
		} catch (Throwable t) {
			CyberCraft.LOG.error("CyberCraft: failed to open shared memory", t);
		}
	}

	/**
	 * Tells Cyberpunk what Minecraft wants. With {@code follow} set, Cyberpunk keeps moving V to (x, y, z)
	 * (the same coordinate space as the game state position) and turns V to face {@code yaw}.
	 */
	public static void publishMcState(boolean inWorld, boolean follow, double x, double y, double z, float yaw, float pitch) {
		MemorySegment s = shm;
		if (s == null) {
			return;
		}
		int base = s.get(JAVA_INT, OFF_MC_STATE + M_SEQ) & ~1;
		s.set(JAVA_INT, OFF_MC_STATE + M_SEQ, base + 1); // odd: write in progress
		VarHandle.releaseFence();
		s.set(JAVA_INT, OFF_MC_STATE + M_FLAGS, (inWorld ? MC_IN_WORLD : 0) | (follow ? MC_FOLLOW : 0));
		s.set(JAVA_DOUBLE, OFF_MC_STATE + M_X, x);
		s.set(JAVA_DOUBLE, OFF_MC_STATE + M_Y, y);
		s.set(JAVA_DOUBLE, OFF_MC_STATE + M_Z, z);
		s.set(JAVA_FLOAT, OFF_MC_STATE + M_YAW, yaw);
		s.set(JAVA_FLOAT, OFF_MC_STATE + M_PITCH, pitch);
		s.set(JAVA_LONG, OFF_MC_STATE + M_FRAME, ++mcFrame);
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
			VarHandle.acquireFence();
			int seq2 = s.get(JAVA_INT, OFF_GAME_STATE + G_SEQ);
			if (seq1 == seq2) {
				return true;
			}
		}
		return false;
	}
}
