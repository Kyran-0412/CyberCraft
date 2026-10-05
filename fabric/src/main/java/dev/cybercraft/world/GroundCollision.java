package dev.cybercraft.world;

import dev.cybercraft.link.CyberLink;
import java.util.concurrent.ConcurrentHashMap;
import net.minecraft.core.BlockPos;
import net.minecraft.world.phys.shapes.Shapes;
import net.minecraft.world.phys.shapes.VoxelShape;
import org.jspecify.annotations.Nullable;

/**
 * Night City's ground as Minecraft sees it: an invisible collision shape for each block position, made
 * from the ground heights Cyberpunk's plugin finds (see Ground.cpp). These are not blocks. They are
 * merged into Minecraft's block-collision queries (see BlockCollisionsMixin), so vanilla movement,
 * step-up and falling run unchanged against them.
 *
 * Each 1 m cell has one height, measured at its centre. Between cell centres the surface is blended
 * smoothly, and every block position gets an 8 x 8 grid of columns whose heights are rounded to 1/8 of a
 * block, so a slope is made of steps far below Minecraft's 0.6 step height and feels like a ramp.
 * Where neighbouring cells differ by more than MAX_STEP the surface is not blended: the jump stays a
 * sharp edge, which Minecraft treats as a step or a wall.
 *
 * Cells where the plugin found something in the way (a wall, tree or lamp post) are also solid: a full
 * 1 m x 1 m column from the ground up to the obstacle's top.
 */
public final class GroundCollision {
	/** Biggest height difference between neighbouring cells that is still treated as a slope. */
	private static final double MAX_STEP = 0.7;
	/** Blocks below the lowest ground that are still solid. */
	private static final int DEPTH = 3;
	private static final int CACHE_LIMIT = 60_000;

	private record Entry(long hash, @Nullable VoxelShape shape) {
	}

	private static final ConcurrentHashMap<Long, Entry> CACHE = new ConcurrentHashMap<>();
	private static volatile boolean enabled;

	private GroundCollision() {
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
		if (!enabled) {
			return null;
		}
		int bx = pos.getX();
		int by = pos.getY();
		int bz = pos.getZ();

		// The 3 x 3 cells around this block's cell: [i][j] is the cell (bx - 1 + i, bz - 1 + j).
		double[][] h = new double[3][3];
		long hash = 1125899906842597L;
		double lowest = Double.POSITIVE_INFINITY;
		double highest = Double.NEGATIVE_INFINITY;
		for (int i = 0; i < 3; i++) {
			for (int j = 0; j < 3; j++) {
				float f = CyberLink.groundHeight(bx - 1 + i, bz - 1 + j);
				hash = 31 * hash + Float.floatToRawIntBits(f);
				if (Float.isNaN(f) || Float.isInfinite(f)) {
					h[i][j] = Double.NaN;
				} else {
					h[i][j] = f;
					lowest = Math.min(lowest, f);
					highest = Math.max(highest, f);
				}
			}
		}
		if (Double.isNaN(h[1][1])) {
			return null; // no ground known in this block's own cell
		}
		// Anything standing in the way in this block's own cell?
		float obstacle = CyberLink.obstacleTop(bx, bz);
		hash = 31 * hash + Float.floatToRawIntBits(obstacle);
		boolean blocked = !Float.isNaN(obstacle) && !Float.isInfinite(obstacle);
		if (blocked) {
			highest = Math.max(highest, obstacle);
		}
		// Far above the highest ground or obstacle, or well under the lowest ground: nothing here.
		if (by > Math.ceil(highest) + 0.5 || by + 1 < Math.floor(lowest) - DEPTH) {
			return null;
		}

		long key = pos.asLong();
		Entry cached = CACHE.get(key);
		if (cached != null && cached.hash == hash) {
			return cached.shape;
		}
		VoxelShape shape = build(h, by, bx, bz);
		if (blocked) {
			// The obstacle: the whole column from the ground up to its top.
			double low = Math.max(0.0, h[1][1] - by);
			double high = Math.min(1.0, obstacle - by);
			if (high - low > 1.0e-3) {
				VoxelShape column = Shapes.box(0, low, 0, 1, high, 1);
				shape = shape == null ? column : Shapes.or(shape, column);
			}
		}
		if (CACHE.size() > CACHE_LIMIT) {
			CACHE.clear();
		}
		CACHE.put(key, new Entry(hash, shape));
		return shape;
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

	private static @Nullable VoxelShape build(double[][] h, int by, int bx, int bz) {
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
			return null;
		}
		if (allFull) {
			return Shapes.block();
		}
		if (allSame) {
			return Shapes.box(0, 0, 0, 1, eighths[0][0] / 8.0, 1);
		}
		VoxelShape shape = Shapes.empty();
		for (int j = 0; j < 8; j++) {
			int i = 0;
			while (i < 8) {
				int f = eighths[i][j];
				int end = i + 1;
				while (end < 8 && eighths[end][j] == f) {
					end++;
				}
				if (f > 0) {
					shape = Shapes.or(shape, Shapes.box(i / 8.0, 0, j / 8.0, end / 8.0, f / 8.0, (j + 1) / 8.0));
				}
				i = end;
			}
		}
		return shape;
	}
}
