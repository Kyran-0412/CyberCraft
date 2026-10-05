package dev.cybercraft.client;

import static dev.cybercraft.link.Proto.GROUND_RADIUS;

import dev.cybercraft.CyberCraft;
import dev.cybercraft.link.CyberLink;
import java.util.ArrayList;
import java.util.List;
import java.util.UUID;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import net.minecraft.client.Minecraft;
import net.minecraft.core.BlockPos;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.state.BlockState;

/**
 * Phase 2a: builds the ground Cyberpunk found around V out of real Minecraft blocks, so Minecraft's own
 * collision lets the player walk on Night City's streets.
 *
 * Every 1 m cell has a height h. The blocks of that column end exactly at ceil(h), so the player
 * stands at Y = ceil(h) and V's real height is that minus (ceil(h) - h): see {@link #groundError}.
 */
public final class GroundMirror {
	private static final int THICKNESS = 2; // blocks per column: cobblestone on top, stone under it
	private static final int NONE = Integer.MIN_VALUE;
	private static final int MAX_CHANGES_PER_TICK = 600;
	private static final int MIN_Y = -58;
	private static final int MAX_Y = 300;

	// Cells around V, nearest first.
	private static final int[][] ORDER = buildOrder();

	// Column (bx, bz) -> the Y its blocks end at. Only touched for columns that have blocks.
	private static final ConcurrentHashMap<Long, Integer> PLACED = new ConcurrentHashMap<>();
	private static final AtomicBoolean BUSY = new AtomicBoolean();
	private static volatile boolean enabled;
	private static Object lastLevel;

	// For the log: what the last pass saw.
	private static volatile int statKnown, statWithGround;
	private static final AtomicInteger STAT_UNLOADED = new AtomicInteger();
	private static final AtomicInteger STAT_BUILT = new AtomicInteger();

	private GroundMirror() {
	}

	public static boolean enabled() {
		return enabled;
	}

	public static void start() {
		PLACED.clear();
		STAT_UNLOADED.set(0);
		STAT_BUILT.set(0);
		enabled = true;
	}

	/** One line for the log: how much ground Cyberpunk has reported, and how much has been built. */
	public static String describe() {
		return "cells reported by Cyberpunk: " + statKnown + " (" + statWithGround + " with ground), columns built: " + PLACED.size()
			+ ", skipped because the chunk wasn't loaded: " + STAT_UNLOADED.get();
	}

	public static void stop() {
		enabled = false;
	}

	private static int[][] buildOrder() {
		List<int[]> cells = new ArrayList<>();
		for (int dz = -GROUND_RADIUS; dz <= GROUND_RADIUS; dz++) {
			for (int dx = -GROUND_RADIUS; dx <= GROUND_RADIUS; dx++) {
				if (dx * dx + dz * dz <= GROUND_RADIUS * GROUND_RADIUS) {
					cells.add(new int[] { dx, dz });
				}
			}
		}
		cells.sort((a, b) -> Integer.compare(a[0] * a[0] + a[1] * a[1], b[0] * b[0] + b[1] * b[1]));
		return cells.toArray(new int[0][]);
	}

	private static long key(int bx, int bz) {
		return ((long) bx << 32) | (bz & 0xFFFFFFFFL);
	}

	/** The Y that blocks of a column end at (the surface a player stands on), or NaN if that cell isn't known to have ground. */
	public static double surfaceAt(double x, double z) {
		float h = CyberLink.groundHeight((int) Math.floor(x), (int) Math.floor(z));
		return Float.isNaN(h) || Float.isInfinite(h) ? Double.NaN : Math.ceil(h);
	}

	/** How far Cyberpunk's real ground under (x, z) is below the top of the Minecraft blocks there (0 to -1). */
	public static double groundError(double x, double z) {
		float h = CyberLink.groundHeight((int) Math.floor(x), (int) Math.floor(z));
		return Float.isNaN(h) || Float.isInfinite(h) ? 0.0 : h - Math.ceil(h);
	}

	/** Call every client tick with the latest game state: builds whatever ground has changed since last time. */
	public static void tick(Minecraft client, CyberLink.GameState state) {
		if (!enabled || client.player == null || client.level == null || !state.inGame()) {
			return;
		}
		if (client.level != lastLevel) {
			lastLevel = client.level;
			PLACED.clear(); // a different world: nothing we placed is in it
		}
		if (BUSY.get()) {
			return; // the server hasn't finished the last batch
		}
		var server = client.getSingleplayerServer();
		if (server == null) {
			return;
		}

		int cx = (int) Math.floor(state.x);
		int cz = (int) Math.floor(state.z);
		List<int[]> changes = new ArrayList<>();
		int known = 0;
		int withGround = 0;
		for (int[] o : ORDER) {
			int bx = cx + o[0];
			int bz = cz + o[1];
			float h = CyberLink.groundHeight(bx, bz);
			if (Float.isNaN(h)) {
				continue; // not scanned yet: leave the column as it is
			}
			known++;
			if (!Float.isInfinite(h)) {
				withGround++;
			}
			int top = Float.isInfinite(h) ? NONE : (int) Math.ceil(h);
			if (top != NONE && (top < MIN_Y + THICKNESS || top > MAX_Y)) {
				top = NONE;
			}
			Integer old = PLACED.get(key(bx, bz));
			if (old == null ? top == NONE : old == top) {
				continue; // already as wanted
			}
			if (changes.size() < MAX_CHANGES_PER_TICK) {
				changes.add(new int[] { bx, bz, top });
			}
		}
		statKnown = known;
		statWithGround = withGround;
		if (changes.isEmpty()) {
			return;
		}

		UUID uuid = client.player.getUUID();
		BUSY.set(true);
		server.execute(() -> {
			try {
				apply(server.getPlayerList().getPlayer(uuid), changes);
			} catch (Throwable t) {
				CyberCraft.LOG.error("CyberCraft: building the ground failed", t);
			} finally {
				BUSY.set(false);
			}
		});
	}

	// Runs on the server thread.
	private static void apply(ServerPlayer player, List<int[]> changes) {
		if (player == null) {
			return;
		}
		ServerLevel level = player.level();
		BlockState air = Blocks.AIR.defaultBlockState();
		BlockState top = Blocks.COBBLESTONE.defaultBlockState();
		BlockState under = Blocks.STONE.defaultBlockState();

		for (int[] c : changes) {
			int bx = c[0];
			int bz = c[1];
			int newTop = c[2];
			if (!level.isLoaded(new BlockPos(bx, 0, bz))) {
				STAT_UNLOADED.incrementAndGet();
				continue; // that part of the world isn't loaded yet: try again next tick
			}
			Integer old = PLACED.get(key(bx, bz));
			if (old != null) {
				for (int dy = 1; dy <= THICKNESS; dy++) {
					level.setBlock(new BlockPos(bx, old - dy, bz), air, 2 | 16);
				}
			}
			if (newTop == NONE) {
				PLACED.remove(key(bx, bz));
				continue;
			}
			for (int dy = 1; dy <= THICKNESS; dy++) {
				level.setBlock(new BlockPos(bx, newTop - dy, bz), dy == 1 ? top : under, 2 | 16);
			}
			PLACED.put(key(bx, bz), newTop);
		}
	}
}
