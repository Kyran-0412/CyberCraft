package dev.cybercraft.world;

import static dev.cybercraft.link.Proto.OBSTACLE_HEIGHT;
import static dev.cybercraft.link.Proto.OBSTACLE_SUB;

import dev.cybercraft.link.CyberLink;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.ConcurrentHashMap;
import net.minecraft.core.BlockPos;
import net.minecraft.world.phys.AABB;
import net.minecraft.world.phys.Vec3;
import net.minecraft.world.phys.shapes.Shapes;
import net.minecraft.world.phys.shapes.VoxelShape;
import org.jspecify.annotations.Nullable;

/**
 * Night City's ground as Minecraft sees it: an invisible collision shape for each block position, made
 * from the ground heights and obstacles Cyberpunk's plugin finds (see Ground.cpp). These are not blocks.
 * They are merged into Minecraft's block-collision queries (see BlockCollisionsMixin), so vanilla
 * movement, step-up and falling run unchanged against them.
 *
 * Each 1 m cell has one height, measured at its centre. Between cell centres the surface is blended
 * smoothly, and every block position gets an 8 x 8 grid of columns whose heights are rounded to 1/8 of a
 * block, so a slope is made of steps far below Minecraft's 0.6 step height and feels like a ramp.
 * Where neighbouring cells differ by more than MAX_STEP the surface is not blended: the jump stays a
 * sharp edge, which Minecraft treats as a step or a wall.
 *
 * Cells where the plugin found something in the way (a wall, tree or lamp post) are also solid. The plugin
 * reports them as 0.125 m sub-squares, so a thin pole is a thin pillar and a diagonal wall is a fine
 * staircase rather than a few big blocks; each is solid from the ground up to head height.
 *
 * Minecraft's collision only stops an entity from moving *into* a shape. An entity that is already inside
 * one (an obstacle that was only found after the player had walked into it, say) can move freely through
 * everything, which looks like clipping through walls. {@link #pushOut} finds the nearest free spot so the
 * player can be moved back out.
 */
public final class GroundCollision {
	/** Biggest height difference between neighbouring cells that is still treated as a slope. */
	private static final double MAX_STEP = 0.7;
	/** Blocks below the lowest ground that are still solid. */
	private static final int DEPTH = 3;
	private static final int CACHE_LIMIT = 60_000;

	/** The shape of the ground in one block position, and the same thing as plain boxes (for overlap tests). */
	private record Entry(long hash, @Nullable VoxelShape shape, List<AABB> boxes) {
	}

	private static final ConcurrentHashMap<Long, Entry> CACHE = new ConcurrentHashMap<>();
	private static volatile boolean enabled;
	// Players use the smooth collider (SmoothCollider) instead of these block-style shapes. Like SkyCraft, the shapes
	// are skipped for players on both sides, so the integrated server's re-check of the player's movement agrees.
	private static volatile boolean smoothPlayers = true;

	private GroundCollision() {
	}

	public static boolean smoothPlayers() {
		return smoothPlayers;
	}

	public static void setSmoothPlayers(boolean on) {
		smoothPlayers = on;
	}

	/** The exact (smooth, not rounded to blocks) ground height at a point, or NaN if no ground is known there. */
	public static double surfaceAt(double x, double z) {
		int bx = (int) Math.floor(x);
		int bz = (int) Math.floor(z);
		double[][] h = new double[3][3];
		for (int i = 0; i < 3; i++) {
			for (int j = 0; j < 3; j++) {
				float f = CyberLink.groundHeight(bx - 1 + i, bz - 1 + j);
				h[i][j] = Float.isNaN(f) || Float.isInfinite(f) ? Double.NaN : f;
			}
		}
		fillPinhole(h);
		if (Double.isNaN(h[1][1])) {
			return Double.NaN;
		}
		return surface(h, x - (bx - 0.5), z - (bz - 0.5));
	}

	/** The ground only collides while this is on (while following). */
	public static void setEnabled(boolean on) {
		enabled = on;
		CACHE.clear();
	}

	public static boolean enabled() {
		return enabled;
	}

	/** The collision shape of the ground inside the block at {@code pos}, or null for none. */
	public static @Nullable VoxelShape shapeAt(BlockPos pos) {
		Entry entry = entryAt(pos.getX(), pos.getY(), pos.getZ());
		return entry == null ? null : entry.shape;
	}

	private static @Nullable Entry entryAt(int bx, int by, int bz) {
		if (!enabled) {
			return null;
		}

		// The 3 x 3 cells around this block's cell: [i][j] is the cell (bx - 1 + i, bz - 1 + j).
		double[][] h = new double[3][3];
		long hash = 1125899906842597L;
		for (int i = 0; i < 3; i++) {
			for (int j = 0; j < 3; j++) {
				float f = CyberLink.groundHeight(bx - 1 + i, bz - 1 + j);
				hash = 31 * hash + Float.floatToRawIntBits(f);
				h[i][j] = Float.isNaN(f) || Float.isInfinite(f) ? Double.NaN : f;
			}
		}
		fillPinhole(h);
		if (Double.isNaN(h[1][1])) {
			return null; // no ground known in this block's own cell
		}
		double lowest = Double.POSITIVE_INFINITY;
		double highest = Double.NEGATIVE_INFINITY;
		for (double[] row : h) {
			for (double v : row) {
				if (!Double.isNaN(v)) {
					lowest = Math.min(lowest, v);
					highest = Math.max(highest, v);
				}
			}
		}
		// Anything standing in the way in this block's own cell?
		long obstacles = CyberLink.obstacleMask(bx, bz);
		hash = 31 * hash + Long.hashCode(obstacles);
		if (obstacles != 0) {
			highest = Math.max(highest, h[1][1] + OBSTACLE_HEIGHT);
		}
		// Far above the highest ground or obstacle, or well under the lowest ground: nothing here.
		if (by > Math.ceil(highest) + 0.5 || by + 1 < Math.floor(lowest) - DEPTH) {
			return null;
		}

		long key = BlockPos.asLong(bx, by, bz);
		Entry cached = CACHE.get(key);
		if (cached != null && cached.hash == hash) {
			return cached;
		}

		List<AABB> boxes = new ArrayList<>();
		buildGround(h, by, boxes);
		if (obstacles != 0) {
			// The obstacles: from a little under the ground at this cell's centre up to head height.
			double low = Math.max(0.0, h[1][1] - 0.5 - by);
			double high = Math.min(1.0, h[1][1] + OBSTACLE_HEIGHT - by);
			if (high - low > 1.0e-3) {
				buildObstacles(obstacles, low, high, boxes);
			}
		}
		VoxelShape shape = null;
		for (AABB box : boxes) {
			VoxelShape part = Shapes.box(box.minX, box.minY, box.minZ, box.maxX, box.maxY, box.maxZ);
			shape = shape == null ? part : Shapes.or(shape, part);
		}
		Entry entry = new Entry(hash, shape, boxes);
		if (CACHE.size() > CACHE_LIMIT) {
			CACHE.clear();
		}
		CACHE.put(key, entry);
		return entry;
	}

	/**
	 * A cell the plugin couldn't find ground for, with ground all around it, is almost certainly a gap in the
	 * game's collision mesh that a ray slipped through. Fill it in from its four neighbours so there is no hole
	 * to fall or walk through.
	 */
	private static void fillPinhole(double[][] h) {
		if (!Double.isNaN(h[1][1])) {
			return;
		}
		double[] around = { h[0][1], h[2][1], h[1][0], h[1][2] };
		double sum = 0;
		double min = Double.POSITIVE_INFINITY;
		double max = Double.NEGATIVE_INFINITY;
		int count = 0;
		for (double v : around) {
			if (!Double.isNaN(v)) {
				sum += v;
				min = Math.min(min, v);
				max = Math.max(max, v);
				count++;
			}
		}
		if (count >= 3 && max - min <= MAX_STEP) {
			h[1][1] = sum / count;
		}
	}

	/** The ground surface height at a point inside the cell [1][1] of {@code h}. */
	private static double surface(double[][] h, double u, double v) {
		// u, v: position measured from the centre of cell [0][*] / [*][0]; the point is inside cell 1, so 0.5 <= u, v < 1.5.
		int i0 = (int) Math.floor(u);
		int j0 = (int) Math.floor(v);
		double tu = u - i0;
		double tv = v - j0;
		double own = h[1][1];
		double sum = 0;
		double weights = 0;
		for (int di = 0; di <= 1; di++) {
			for (int dj = 0; dj <= 1; dj++) {
				double height = h[i0 + di][j0 + dj];
				if (Double.isNaN(height) || Math.abs(height - own) > MAX_STEP) {
					continue; // no ground there, or too big a jump to blend across
				}
				double w = (di == 0 ? 1 - tu : tu) * (dj == 0 ? 1 - tv : tv);
				sum += w * height;
				weights += w;
			}
		}
		return weights > 0 ? sum / weights : own;
	}

	/** Adds the boxes that make up the ground surface inside one block (local coordinates, 0 to 1). */
	private static void buildGround(double[][] h, int by, List<AABB> out) {
		int[][] eighths = new int[8][8]; // [i along X][j along Z]
		boolean allEmpty = true;
		boolean allFull = true;
		boolean allSame = true;
		for (int i = 0; i < 8; i++) {
			for (int j = 0; j < 8; j++) {
				// Position of this sub-column's centre, from the centre of the cell at [0][0]. The block starts at
				// bx, and that cell's centre is at bx - 0.5, so u = (i + 0.5) / 8 + 0.5.
				double u = (i + 0.5) / 8.0 + 0.5;
				double v = (j + 0.5) / 8.0 + 0.5;
				double s = surface(h, u, v);
				// Round up, so the player never stands lower than the real ground.
				int f = (int) Math.ceil((s - by) * 8.0 - 1.0e-4);
				f = Math.max(0, Math.min(8, f));
				eighths[i][j] = f;
				allEmpty &= f == 0;
				allFull &= f == 8;
				allSame &= f == eighths[0][0];
			}
		}
		if (allEmpty) {
			return;
		}
		if (allFull) {
			out.add(new AABB(0, 0, 0, 1, 1, 1));
			return;
		}
		if (allSame) {
			out.add(new AABB(0, 0, 0, 1, eighths[0][0] / 8.0, 1));
			return;
		}
		for (int j = 0; j < 8; j++) {
			int i = 0;
			while (i < 8) {
				int f = eighths[i][j];
				int end = i + 1;
				while (end < 8 && eighths[end][j] == f) {
					end++;
				}
				if (f > 0) {
					out.add(new AABB(i / 8.0, 0, j / 8.0, end / 8.0, f / 8.0, (j + 1) / 8.0));
				}
				i = end;
			}
		}
	}

	/** Adds the blocked sub-squares in {@code mask} as boxes between two heights (in blocks, within the block). */
	private static void buildObstacles(long mask, double low, double high, List<AABB> out) {
		double size = 1.0 / OBSTACLE_SUB;
		for (int sz = 0; sz < OBSTACLE_SUB; sz++) {
			int sx = 0;
			while (sx < OBSTACLE_SUB) {
				if ((mask & (1L << (sx + OBSTACLE_SUB * sz))) == 0) {
					sx++;
					continue;
				}
				int end = sx + 1; // merge a run of blocked squares along X into one box
				while (end < OBSTACLE_SUB && (mask & (1L << (end + OBSTACLE_SUB * sz))) != 0) {
					end++;
				}
				out.add(new AABB(sx * size, low, sz * size, end * size, high, (sz + 1) * size));
				sx = end;
			}
		}
	}

	/** True if {@code box} overlaps the ground or an obstacle. */
	public static boolean overlaps(AABB box) {
		if (!enabled) {
			return false;
		}
		AABB b = box.deflate(1.0E-4);
		int x0 = (int) Math.floor(b.minX);
		int x1 = (int) Math.floor(b.maxX);
		int y0 = (int) Math.floor(b.minY);
		int y1 = (int) Math.floor(b.maxY);
		int z0 = (int) Math.floor(b.minZ);
		int z1 = (int) Math.floor(b.maxZ);
		for (int x = x0; x <= x1; x++) {
			for (int y = y0; y <= y1; y++) {
				for (int z = z0; z <= z1; z++) {
					Entry entry = entryAt(x, y, z);
					if (entry == null) {
						continue;
					}
					for (AABB local : entry.boxes) {
						if (local.move(x, y, z).intersects(b)) {
							return true;
						}
					}
				}
			}
		}
		return false;
	}

	private static final double[] PUSH_DISTANCES = { 0.05, 0.1, 0.15, 0.2, 0.3, 0.4, 0.5, 0.6, 0.8, 1.0, 1.25, 1.5, 2.0, 2.5, 3.0 };

	/**
	 * If {@code box} is inside the ground or an obstacle, the smallest move (up, or sideways in one of 16
	 * directions) that gets it out; otherwise null.
	 */
	public static @Nullable Vec3 pushOut(AABB box) {
		if (!overlaps(box)) {
			return null;
		}
		for (double d : PUSH_DISTANCES) {
			if (!overlaps(box.move(0, d, 0))) {
				return new Vec3(0, d, 0);
			}
			for (int k = 0; k < 16; k++) {
				double angle = k * Math.PI / 8.0;
				double dx = Math.cos(angle) * d;
				double dz = Math.sin(angle) * d;
				if (!overlaps(box.move(dx, 0, dz))) {
					return new Vec3(dx, 0, dz);
				}
			}
		}
		return null;
	}
}
