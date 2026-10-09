package dev.cybercraft.client;

import dev.cybercraft.CyberCraft;
import dev.cybercraft.link.CyberLink;
import dev.cybercraft.link.Proto;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import net.minecraft.client.Minecraft;
import net.minecraft.client.multiplayer.ClientLevel;
import net.minecraft.client.player.LocalPlayer;
import net.minecraft.core.BlockPos;
import net.minecraft.world.level.block.state.BlockState;

/**
 * Tells Cyberpunk which blocks are light sources.
 *
 * Every Minecraft block has a light emission level from 0 to 15 (glowstone 15, a torch 14, lava 15, a candle 3, white concrete 0). Cyberpunk draws the blocks into its own scene and
 * would like to make the light sources glow (bloom) without making a sunlit pale block glow too, which can't be told from the picture alone, so this asks the world.
 *
 * A box of 128 x 64 x 128 blocks around the player (four chunks each way, and 32 blocks up and down) is kept: one byte per block, its light emission level. It is re-read a few
 * 16 x 16 x 16 cells at a time, nearest the player first, so a block you place or break shows up within a couple of seconds, and published to the plugin through shared memory
 * whenever it changes. When the player walks into the next cell of 16 blocks the box moves with them, keeping what it already knew. Nothing here changes the Minecraft world.
 *
 * The method that gives a block's light level is looked up by name (the names differ between versions); /ccemit status says what was found.
 */
public final class EmissionGrid {
	private static final int SX = Proto.EMISSION_SIZE_X; // 128
	private static final int SY = Proto.EMISSION_SIZE_Y; // 64
	private static final int SZ = Proto.EMISSION_SIZE_Z; // 128
	private static final int CELL = 16;
	private static final int CX = SX / CELL; // 8 x 4 x 8 cells
	private static final int CY = SY / CELL;
	private static final int CZ = SZ / CELL;
	private static final int CELL_COUNT = CX * CY * CZ;
	private static final int CELLS_PER_TICK = 6;
	/** The cells, nearest the middle of the box first: so the blocks around the player are read before the far ones. */
	private static final int[] ORDER = nearestFirst();

	private static byte[] grid = new byte[SX * SY * SZ]; // (z * SY + y) * SX + x, counted from the origin
	private static int originX, originY, originZ;
	private static boolean haveOrigin;
	private static int nextCell;
	private static boolean dirty;
	private static int generation;
	private static boolean enabled = true;
	private static boolean published; // the plugin was last told "valid"
	private static Object lastLevel;
	private static Method lightMethod;
	private static boolean resolved;
	private static String methodNote = "not looked for yet";
	private static double lastTickMs;
	private static boolean failedOnce;
	private static final BlockPos.MutableBlockPos POS = new BlockPos.MutableBlockPos();

	private EmissionGrid() {
	}

	public static boolean enabled() {
		return enabled;
	}

	public static void setEnabled(boolean on) {
		enabled = on;
	}

	/** What it is doing, for /ccemit status. */
	public static List<String> status() {
		int emissive = 0;
		int brightest = 0;
		for (byte b : grid) {
			if (b != 0) {
				emissive++;
				brightest = Math.max(brightest, b);
			}
		}
		List<String> lines = new ArrayList<>();
		lines.add("light emission: " + (enabled ? "on" : "off") + "; reading a block's light level with: " + methodNote);
		lines.add("the grid covers " + SX + " x " + SY + " x " + SZ + " blocks from (" + originX + ", " + originY + ", " + originZ + "); " + emissive + " blocks in it give light (the brightest is level " + brightest
			+ "); version " + generation + "; the last tick took " + String.format("%.2f", lastTickMs) + " ms");
		return lines;
	}

	public static void tick(Minecraft client) {
		long t0 = System.nanoTime();
		try {
			tickInner(client);
		} catch (RuntimeException e) {
			if (!failedOnce) {
				failedOnce = true;
				CyberCraft.LOG.warn("CyberCraft: light emission scan failed: {}", e.toString());
			}
		}
		lastTickMs = (System.nanoTime() - t0) / 1.0e6;
	}

	private static void tickInner(Minecraft client) {
		ClientLevel level = client.level;
		LocalPlayer player = client.player;
		if (!enabled || level == null || player == null || !CyberLink.active()) {
			if (published) {
				CyberLink.publishEmission(false, originX, originY, originZ, ++generation, null);
				published = false;
			}
			haveOrigin = false;
			lastLevel = null;
			return;
		}
		if (!resolved) {
			resolve();
		}
		if (lightMethod == null) {
			return;
		}
		if (level != lastLevel) {
			// A different world: start again.
			lastLevel = level;
			Arrays.fill(grid, (byte) 0);
			haveOrigin = false;
			dirty = true;
		}
		int newX = Math.floorDiv((int) Math.floor(player.getX()), CELL) * CELL - SX / 2;
		int newY = Math.floorDiv((int) Math.floor(player.getY()), CELL) * CELL - SY / 2;
		int newZ = Math.floorDiv((int) Math.floor(player.getZ()), CELL) * CELL - SZ / 2;
		if (!haveOrigin || newX != originX || newY != originY || newZ != originZ) {
			moveTo(newX, newY, newZ);
		}
		for (int i = 0; i < CELLS_PER_TICK; i++) {
			scanCell(level, ORDER[nextCell]);
			nextCell = (nextCell + 1) % CELL_COUNT;
		}
		if (dirty) {
			generation++;
			CyberLink.publishEmission(true, originX, originY, originZ, generation, grid);
			published = true;
			dirty = false;
		}
	}

	/** Moves the cube to a new corner (a multiple of 16 from the old one), keeping what it already knew of the blocks that are still inside. */
	private static void moveTo(int nx, int ny, int nz) {
		if (haveOrigin) {
			byte[] next = new byte[grid.length];
			int dx = nx - originX, dy = ny - originY, dz = nz - originZ;
			for (int z = 0; z < SZ; z++) {
				int oz = z + dz;
				if (oz < 0 || oz >= SZ) {
					continue;
				}
				for (int y = 0; y < SY; y++) {
					int oy = y + dy;
					if (oy < 0 || oy >= SY) {
						continue;
					}
					for (int x = 0; x < SX; x++) {
						int ox = x + dx;
						if (ox >= 0 && ox < SX) {
							next[(z * SY + y) * SX + x] = grid[(oz * SY + oy) * SX + ox];
						}
					}
				}
			}
			grid = next;
		}
		originX = nx;
		originY = ny;
		originZ = nz;
		haveOrigin = true;
		nextCell = 0;
		dirty = true;
	}

	private static void scanCell(ClientLevel level, int cell) {
		int cx = cell % CX, cz = (cell / CX) % CZ, cy = cell / (CX * CZ);
		int x0 = originX + cx * CELL, y0 = originY + cy * CELL, z0 = originZ + cz * CELL;
		POS.set(x0, y0, z0);
		if (!level.isLoaded(POS)) {
			return; // keep what we had
		}
		for (int y = 0; y < CELL; y++) {
			for (int z = 0; z < CELL; z++) {
				for (int x = 0; x < CELL; x++) {
					POS.set(x0 + x, y0 + y, z0 + z);
					BlockState state = level.getBlockState(POS);
					int level15 = 0;
					if (!state.isAir()) {
						level15 = emission(state);
					}
					int idx = ((cz * CELL + z) * SY + (cy * CELL + y)) * SX + (cx * CELL + x);
					if (grid[idx] != (byte) level15) {
						grid[idx] = (byte) level15;
						dirty = true;
					}
				}
			}
		}
	}

	private static int[] nearestFirst() {
		Integer[] cells = new Integer[CELL_COUNT];
		for (int i = 0; i < CELL_COUNT; i++) {
			cells[i] = i;
		}
		Arrays.sort(cells, (a, b) -> Double.compare(distanceToMiddle(a), distanceToMiddle(b)));
		int[] order = new int[CELL_COUNT];
		for (int i = 0; i < CELL_COUNT; i++) {
			order[i] = cells[i];
		}
		return order;
	}

	private static double distanceToMiddle(int cell) {
		int cx = cell % CX, cz = (cell / CX) % CZ, cy = cell / (CX * CZ);
		double dx = cx + 0.5 - CX / 2.0, dy = cy + 0.5 - CY / 2.0, dz = cz + 0.5 - CZ / 2.0;
		return dx * dx + dy * dy * 2.0 + dz * dz;
	}

	private static int emission(BlockState state) {
		try {
			int v = ((Number) lightMethod.invoke(state)).intValue();
			return v < 0 ? 0 : Math.min(v, 15);
		} catch (ReflectiveOperationException | RuntimeException e) {
			return 0;
		}
	}

	private static void resolve() {
		resolved = true;
		for (String name : new String[] { "getLightEmission", "getLightLevel", "lightEmission", "getLuminance" }) {
			try {
				Method m = BlockState.class.getMethod(name);
				if (m.getReturnType() == int.class) {
					m.setAccessible(true);
					lightMethod = m;
					methodNote = name + "()";
					CyberCraft.LOG.info("CyberCraft: light emission: reading a block's light level with {}", methodNote);
					return;
				}
			} catch (NoSuchMethodException | RuntimeException ignored) {
				// try the next name
			}
		}
		// Anything public taking nothing and giving an int whose name has light and emission or luminance in it.
		for (Method m : BlockState.class.getMethods()) {
			String n = m.getName().toLowerCase();
			if (m.getParameterCount() == 0 && m.getReturnType() == int.class && n.contains("light") && (n.contains("emi") || n.contains("lumin"))) {
				m.setAccessible(true);
				lightMethod = m;
				methodNote = m.getName() + "() (found by searching)";
				CyberCraft.LOG.info("CyberCraft: light emission: reading a block's light level with {}", methodNote);
				return;
			}
		}
		methodNote = "NOT FOUND (no method on the block state looks like its light emission)";
		CyberCraft.LOG.warn("CyberCraft: light emission: {}", methodNote);
	}
}
