package dev.cybercraft.world;

import static dev.cybercraft.link.Proto.OBSTACLE_HEIGHT;
import static dev.cybercraft.link.Proto.OBSTACLE_SUB;

import dev.cybercraft.link.CyberLink;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.world.phys.BlockHitResult;
import net.minecraft.world.phys.Vec3;
import org.jspecify.annotations.Nullable;

/**
 * Where does a ray from the camera first meet Night City itself (its streets, and the walls, poles and cars the plugin found)?
 *
 * Night City is not made of blocks, so Minecraft's own targeting can't see it, and there is nothing to click on to build. This marches
 * a ray over the same smooth ground surface and obstacle squares the player collides with (see {@link GroundCollision} and
 * {@link SmoothCollider}) and answers with the kind of hit Minecraft would get from a real block: a point, a face, and the block cell the
 * new block goes into. Minecraft places a block in the clicked cell itself when that cell is empty, so the cell is the empty one just
 * outside the surface that was hit. Nothing is added to the world.
 */
public final class TerrainPick {
	private static final double STEP = 0.1; // marching step, metres
	private static final int REFINE = 8; // bisection rounds once the surface has been passed
	private static final double NUDGE = 0.02; // how far outside the surface the new block's cell is looked for

	private static volatile boolean enabled = true;

	private TerrainPick() {
	}

	public static boolean enabled() {
		return enabled;
	}

	public static void setEnabled(boolean on) {
		enabled = on;
	}

	/** Is this point inside the ground or one of the obstacles Cyberpunk reported? */
	private static boolean inside(double x, double y, double z) {
		int bx = (int) Math.floor(x);
		int bz = (int) Math.floor(z);
		float cellGround = CyberLink.groundHeight(bx, bz);
		boolean haveCell = !Float.isNaN(cellGround) && !Float.isInfinite(cellGround);
		double ground = GroundCollision.surfaceAt(x, z);
		if (!Double.isNaN(ground) && y <= ground) {
			return true;
		}
		if (!haveCell) {
			return false;
		}
		long mask = CyberLink.obstacleMask(bx, bz);
		if (mask == 0) {
			return false;
		}
		int sx = Math.min(OBSTACLE_SUB - 1, (int) ((x - bx) * OBSTACLE_SUB));
		int sz = Math.min(OBSTACLE_SUB - 1, (int) ((z - bz) * OBSTACLE_SUB));
		if ((mask & (1L << (sx + OBSTACLE_SUB * sz))) == 0) {
			return false;
		}
		return y >= cellGround && y <= cellGround + OBSTACLE_HEIGHT;
	}

	/**
	 * The first point along the ray (from {@code from}, direction {@code dir}, at most {@code range} long) that is inside Night City, as a hit
	 * Minecraft can use, or null if the ray meets nothing.
	 */
	public static @Nullable BlockHitResult hit(Vec3 from, Vec3 dir, double range) {
		if (!enabled || !GroundCollision.enabled()) {
			return null;
		}
		if (inside(from.x, from.y, from.z)) {
			return null; // the camera is inside something: nothing to aim at from in here
		}
		double lo = 0.0;
		double hi = -1.0;
		for (double t = STEP; t <= range + STEP; t += STEP) {
			double d = Math.min(t, range);
			if (inside(from.x + dir.x * d, from.y + dir.y * d, from.z + dir.z * d)) {
				hi = d;
				break;
			}
			lo = d;
			if (d >= range) {
				break;
			}
		}
		if (hi < 0) {
			return null;
		}
		for (int i = 0; i < REFINE; i++) {
			double mid = (lo + hi) * 0.5;
			if (inside(from.x + dir.x * mid, from.y + dir.y * mid, from.z + dir.z * mid)) {
				hi = mid;
			} else {
				lo = mid;
			}
		}
		Vec3 before = new Vec3(from.x + dir.x * lo, from.y + dir.y * lo, from.z + dir.z * lo);
		Vec3 after = new Vec3(from.x + dir.x * hi, from.y + dir.y * hi, from.z + dir.z * hi);
		Vec3 point = new Vec3((before.x + after.x) * 0.5, (before.y + after.y) * 0.5, (before.z + after.z) * 0.5);

		// Which face was it? A step across the edge of an obstacle square is a side; otherwise the ray came down onto the top (or the street).
		int dx = (int) Math.floor(after.x * OBSTACLE_SUB) - (int) Math.floor(before.x * OBSTACLE_SUB);
		int dz = (int) Math.floor(after.z * OBSTACLE_SUB) - (int) Math.floor(before.z * OBSTACLE_SUB);
		boolean obstacleHere = !Double.isNaN(GroundCollision.surfaceAt(after.x, after.z)) ? after.y > GroundCollision.surfaceAt(after.x, after.z) + 1.0e-3 : true;
		Direction face;
		if (obstacleHere && (dx != 0 || dz != 0)) {
			if (Math.abs(dir.x) * Math.abs(dx) >= Math.abs(dir.z) * Math.abs(dz) && dx != 0) {
				face = dir.x > 0 ? Direction.WEST : Direction.EAST; // moving +x meets a face that looks towards -x
			} else {
				face = dir.z > 0 ? Direction.NORTH : Direction.SOUTH;
			}
		} else {
			face = dir.y > 0 ? Direction.DOWN : Direction.UP;
		}

		// The empty cell just outside the surface is where a block would go.
		int cx = (int) Math.floor(point.x + face.getStepX() * NUDGE);
		int cy = (int) Math.floor(point.y + face.getStepY() * NUDGE + (face == Direction.UP ? NUDGE : 0.0));
		int cz = (int) Math.floor(point.z + face.getStepZ() * NUDGE);
		return new BlockHitResult(point, face, new BlockPos(cx, cy, cz), false);
	}
}
