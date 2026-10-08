package dev.cybercraft.client;

import dev.cybercraft.CyberCraft;
import dev.cybercraft.link.CyberLink;
import dev.cybercraft.link.Proto;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import net.minecraft.client.Minecraft;
import net.minecraft.client.multiplayer.ClientLevel;
import net.minecraft.client.player.LocalPlayer;
import net.minecraft.core.BlockPos;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.phys.AABB;
import net.minecraft.world.phys.shapes.VoxelShape;

/**
 * Collision for what you build: looks at the real blocks around the player, turns them into boxes, and publishes the complete list
 * of boxes to the plugin, which keeps invisible collision boxes in Night City exactly there (so cars and people are stopped by your
 * builds).
 *
 * The world is cut into cells of 16 x 16 x 16 blocks. Cells near the player are re-read every few ticks, cells further away more
 * slowly, so a block you place or break shows up within a fraction of a second. Each cell's solid full blocks are merged into as few
 * boxes as possible; blocks that aren't full cubes (slabs, stairs, fences, doors) give a box for each part of their collision shape.
 *
 * Nothing here changes the Minecraft world, and nothing is saved: the list is rebuilt from the world whenever it is loaded, so each
 * world only ever puts up its own boxes. This is off until {@code /cccollide on}.
 */
public final class BlockColliders {
	private static final int CELL = 16;
	private static final int RADIUS_XZ = 2; // cells each side of the player's cell: 5 x 5 columns, 80 blocks wide
	private static final int BELOW = 1; // cells below the player's cell
	private static final int ABOVE = 3; // cells above it
	private static final int NEAR_PER_TICK = 8;
	private static final int FAR_PER_TICK = 3;
	private static final int NEAR_RADIUS = 1; // cells: the 3 x 3 x 3 around the player are "near"
	private static final int MAX_BOXES = Proto.BOX_TABLE_MAX;
	private static final int MIN_PART = 2; // parts of non-full shapes thinner than this many sixteenths are left out (carpets and the like)

	private static final class Cell {
		final int cx, cy, cz;
		long[] full = new long[64]; // one bit for each block of the cell that is a full solid cube
		int[] partial = new int[0]; // boxes (six ints each, sixteenths of a block, Minecraft coordinates) of the other solid shapes
		int[] merged = new int[0]; // the full blocks, merged into boxes
		boolean scanned;

		Cell(int cx, int cy, int cz) {
			this.cx = cx;
			this.cy = cy;
			this.cz = cz;
		}
	}

	private static final Map<Long, Cell> CELLS = new HashMap<>();
	private static List<Cell> near = new ArrayList<>();
	private static List<Cell> far = new ArrayList<>();
	private static int nearIndex, farIndex;
	private static int windowCx = Integer.MIN_VALUE, windowCy = Integer.MIN_VALUE, windowCz = Integer.MIN_VALUE;

	private static boolean enabled;
	private static boolean published; // the plugin was last told "enabled"
	private static boolean dirty;
	private static Object lastLevel;
	private static int epoch;
	private static final int[] OUT = new int[MAX_BOXES * 6];
	private static int outCount;
	private static boolean truncated;
	private static boolean failedOnce;
	private static double lastTickMs;
	private static final BlockPos.MutableBlockPos POS = new BlockPos.MutableBlockPos();

	private BlockColliders() {
	}

	public static boolean enabled() {
		return enabled;
	}

	public static void setEnabled(boolean on) {
		enabled = on;
		dirty = true;
	}

	/** Makes the plugin throw away every box and build them again (and this scanner re-read everything). */
	public static void rebuild() {
		CELLS.clear();
		windowCx = Integer.MIN_VALUE;
		epoch++;
		dirty = true;
	}

	public static String status() {
		return (enabled ? "on" : "off") + ": " + outCount + " boxes from " + CELLS.size() + " cells" + (truncated ? " (more than " + MAX_BOXES + ": the farthest are left out)" : "")
			+ "; the last tick took " + String.format("%.2f", lastTickMs) + " ms";
	}

	private static long key(int cx, int cy, int cz) {
		return ((long) (cx & 0x3FFFFF) << 42) | ((long) (cy & 0xFFFFF) << 22) | (cz & 0x3FFFFF);
	}

	public static void tick(Minecraft client) {
		long t0 = System.nanoTime();
		try {
			tickInner(client);
		} catch (RuntimeException e) {
			if (!failedOnce) {
				failedOnce = true;
				CyberCraft.LOG.error("CyberCraft: block colliders failed (switching them off)", e);
			}
			enabled = false;
		}
		lastTickMs = (System.nanoTime() - t0) / 1.0e6;
	}

	private static void tickInner(Minecraft client) {
		ClientLevel level = client.level;
		LocalPlayer player = client.player;
		boolean usable = enabled && level != null && player != null && CyberLink.active() && !GroundMirror.enabled();
		if (!usable) {
			if (published) {
				// Tell the plugin to take the boxes down.
				CyberLink.publishBoxes(false, epoch, OUT, 0);
				published = false;
				outCount = 0;
			}
			CELLS.clear();
			windowCx = Integer.MIN_VALUE;
			lastLevel = null;
			return;
		}
		if (level != lastLevel) {
			// A different world: forget everything, the plugin's boxes follow the new list.
			lastLevel = level;
			CELLS.clear();
			windowCx = Integer.MIN_VALUE;
			dirty = true;
		}

		BlockPos here = player.blockPosition();
		int pcx = Math.floorDiv(here.getX(), CELL);
		int pcy = Math.floorDiv(here.getY(), CELL);
		int pcz = Math.floorDiv(here.getZ(), CELL);
		if (pcx != windowCx || pcy != windowCy || pcz != windowCz) {
			windowCx = pcx;
			windowCy = pcy;
			windowCz = pcz;
			buildWindow();
			dirty = true;
		}

		boolean changed = false;
		for (int i = 0; i < NEAR_PER_TICK && !near.isEmpty(); i++) {
			changed |= scan(level, near.get(nearIndex++ % near.size()));
		}
		for (int i = 0; i < FAR_PER_TICK && !far.isEmpty(); i++) {
			changed |= scan(level, far.get(farIndex++ % far.size()));
		}
		if (changed) {
			dirty = true;
		}
		if (dirty || !published) {
			publish();
		}
	}

	/** The cells around the player, nearest first: the 3 x 3 x 3 around it are scanned often, the rest more slowly. */
	private static void buildWindow() {
		Map<Long, Cell> keep = new HashMap<>();
		List<Cell> all = new ArrayList<>();
		for (int dx = -RADIUS_XZ; dx <= RADIUS_XZ; dx++) {
			for (int dz = -RADIUS_XZ; dz <= RADIUS_XZ; dz++) {
				for (int dy = -BELOW; dy <= ABOVE; dy++) {
					int cx = windowCx + dx, cy = windowCy + dy, cz = windowCz + dz;
					long k = key(cx, cy, cz);
					Cell c = CELLS.get(k);
					if (c == null) {
						c = new Cell(cx, cy, cz);
					}
					keep.put(k, c);
					all.add(c);
				}
			}
		}
		CELLS.clear();
		CELLS.putAll(keep);
		all.sort((a, b) -> Integer.compare(distance2(a), distance2(b)));
		near = new ArrayList<>();
		far = new ArrayList<>();
		for (Cell c : all) {
			if (Math.abs(c.cx - windowCx) <= NEAR_RADIUS && Math.abs(c.cy - windowCy) <= NEAR_RADIUS && Math.abs(c.cz - windowCz) <= NEAR_RADIUS) {
				near.add(c);
			} else {
				far.add(c);
			}
		}
		nearIndex = 0;
		farIndex = 0;
	}

	private static int distance2(Cell c) {
		int dx = c.cx - windowCx, dy = c.cy - windowCy, dz = c.cz - windowCz;
		return dx * dx + dy * dy + dz * dz;
	}

	/** Reads the cell's blocks again. Returns true if its boxes are different from before. */
	private static boolean scan(ClientLevel level, Cell cell) {
		int x0 = cell.cx * CELL, y0 = cell.cy * CELL, z0 = cell.cz * CELL;
		POS.set(x0, y0, z0);
		if (!level.isLoaded(POS)) {
			return false; // keep what we had
		}
		long[] full = new long[64];
		int[] part = new int[48];
		int np = 0;
		for (int y = 0; y < CELL; y++) {
			for (int z = 0; z < CELL; z++) {
				for (int x = 0; x < CELL; x++) {
					POS.set(x0 + x, y0 + y, z0 + z);
					BlockState state = level.getBlockState(POS);
					if (state.isAir()) {
						continue;
					}
					VoxelShape shape = state.getCollisionShape(level, POS);
					if (shape.isEmpty()) {
						continue;
					}
					if (Block.isShapeFullBlock(shape)) {
						int idx = (y * CELL + z) * CELL + x;
						full[idx >> 6] |= 1L << (idx & 63);
					} else {
						for (AABB b : shape.toAabbs()) {
							int minX = (int) Math.round((x0 + x + b.minX) * 16.0), maxX = (int) Math.round((x0 + x + b.maxX) * 16.0);
							int minY = (int) Math.round((y0 + y + b.minY) * 16.0), maxY = (int) Math.round((y0 + y + b.maxY) * 16.0);
							int minZ = (int) Math.round((z0 + z + b.minZ) * 16.0), maxZ = (int) Math.round((z0 + z + b.maxZ) * 16.0);
							if (maxX - minX < MIN_PART || maxY - minY < MIN_PART || maxZ - minZ < MIN_PART) {
								continue;
							}
							if (np + 6 > part.length) {
								part = Arrays.copyOf(part, part.length * 2);
							}
							part[np++] = minX;
							part[np++] = minY;
							part[np++] = minZ;
							part[np++] = maxX;
							part[np++] = maxY;
							part[np++] = maxZ;
						}
					}
				}
			}
		}
		part = Arrays.copyOf(part, np);
		boolean same = cell.scanned && Arrays.equals(full, cell.full) && Arrays.equals(part, cell.partial);
		cell.scanned = true;
		if (same) {
			return false;
		}
		cell.full = full;
		cell.partial = part;
		cell.merged = merge(full, x0, y0, z0);
		return true;
	}

	private static boolean bit(long[] bits, int x, int y, int z) {
		int idx = (y * CELL + z) * CELL + x;
		return (bits[idx >> 6] & (1L << (idx & 63))) != 0;
	}

	private static void clear(long[] bits, int x, int y, int z) {
		int idx = (y * CELL + z) * CELL + x;
		bits[idx >> 6] &= ~(1L << (idx & 63));
	}

	/** Greedy merge: the largest box from each not-yet-used block, growing along x, then z, then y. */
	private static int[] merge(long[] full, int x0, int y0, int z0) {
		long[] left = full.clone();
		int[] out = new int[24];
		int n = 0;
		for (int y = 0; y < CELL; y++) {
			for (int z = 0; z < CELL; z++) {
				for (int x = 0; x < CELL; x++) {
					if (!bit(left, x, y, z)) {
						continue;
					}
					int w = 1;
					while (x + w < CELL && bit(left, x + w, y, z)) {
						w++;
					}
					int d = 1;
					grow:
					while (z + d < CELL) {
						for (int i = 0; i < w; i++) {
							if (!bit(left, x + i, y, z + d)) {
								break grow;
							}
						}
						d++;
					}
					int h = 1;
					grow2:
					while (y + h < CELL) {
						for (int k = 0; k < d; k++) {
							for (int i = 0; i < w; i++) {
								if (!bit(left, x + i, y + h, z + k)) {
									break grow2;
								}
							}
						}
						h++;
					}
					for (int j = 0; j < h; j++) {
						for (int k = 0; k < d; k++) {
							for (int i = 0; i < w; i++) {
								clear(left, x + i, y + j, z + k);
							}
						}
					}
					if (n + 6 > out.length) {
						out = Arrays.copyOf(out, out.length * 2);
					}
					out[n++] = (x0 + x) * 16;
					out[n++] = (y0 + y) * 16;
					out[n++] = (z0 + z) * 16;
					out[n++] = (x0 + x + w) * 16;
					out[n++] = (y0 + y + h) * 16;
					out[n++] = (z0 + z + d) * 16;
				}
			}
		}
		return Arrays.copyOf(out, n);
	}

	/** Collects every cell's boxes (nearest cells first, so the farthest are the ones left out if there are too many) and hands them to the plugin. */
	private static void publish() {
		List<Cell> cells = new ArrayList<>(CELLS.values());
		cells.sort((a, b) -> Integer.compare(distance2(a), distance2(b)));
		int n = 0;
		truncated = false;
		outer:
		for (Cell c : cells) {
			for (int[] list : new int[][] {c.merged, c.partial}) {
				for (int i = 0; i + 5 < list.length; i += 6) {
					if (n >= MAX_BOXES) {
						truncated = true;
						break outer;
					}
					System.arraycopy(list, i, OUT, n * 6, 6);
					n++;
				}
			}
		}
		outCount = n;
		CyberLink.publishBoxes(true, epoch, OUT, n);
		published = true;
		dirty = false;
	}
}
