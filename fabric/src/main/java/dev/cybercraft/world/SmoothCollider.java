package dev.cybercraft.world;

import static dev.cybercraft.link.Proto.OBSTACLE_HEIGHT;
import static dev.cybercraft.link.Proto.OBSTACLE_SUB;

import dev.cybercraft.link.CyberLink;
import java.util.ArrayList;
import java.util.List;
import org.jspecify.annotations.Nullable;

/**
 * Smooth collision for the local player against Night City's ground and obstacles. The same idea as
 * SkyCraft's TriCollider (MIT, by chasmlol), but for the heightfield and obstacle squares the plugin finds.
 *
 * Minecraft still works out every velocity (walking, sprinting, jumping, gravity, friction). This only
 * replaces how that movement is stopped, because vanilla collision can only use axis-aligned boxes, which
 * turn slopes into steps and make a diagonal wall catch on every corner:
 *  - the ground is followed exactly (no 1/8-block steps), stepping up to Minecraft's step height;
 *  - while on the ground and not jumping, the player sticks to it going downhill;
 *  - obstacles are boxes; the player is a circle, so it slides along them (a circle meeting the corners of a
 *    staircase is pushed along the staircase's overall direction, not into each notch);
 *  - ground that rises more than the step height is a ledge/wall and is slid along too.
 * The resulting movement is handed back to Minecraft, which works out onGround, fall damage and so on from it
 * exactly as it would from block collisions.
 */
public final class SmoothCollider {
	private static final double FLOOR_RADIUS = 0.15;  // the ground is sampled under a small footprint
	private static final double SUBSTEP = 0.1;        // horizontal sub-steps so walls can't be tunnelled
	private static final double AIR_STEP = 0.3;       // walkable ground this far above the feet catches you mid-air
	private static final double EPS = 1.0e-4;
	private static final int PASSES = 4;
	private static final double[][] FLOOR_SAMPLES = buildFloorSamples();
	private static final double[][] RING = buildRing(12);

	private record Box(double minX, double maxX, double minZ, double maxZ, double low, double high) {
	}

	private SmoothCollider() {
	}

	private static double[][] buildFloorSamples() {
		List<double[]> samples = new ArrayList<>();
		samples.add(new double[] { 0, 0 });
		for (int i = 0; i < 8; i++) {
			double a = i * Math.PI / 4;
			samples.add(new double[] { Math.cos(a) * FLOOR_RADIUS, Math.sin(a) * FLOOR_RADIUS });
		}
		for (int i = 0; i < 4; i++) {
			double a = Math.PI / 4 + i * Math.PI / 2;
			samples.add(new double[] { Math.cos(a) * FLOOR_RADIUS * 0.5, Math.sin(a) * FLOOR_RADIUS * 0.5 });
		}
		return samples.toArray(new double[0][]);
	}

	private static double[][] buildRing(int n) {
		double[][] ring = new double[n][];
		for (int i = 0; i < n; i++) {
			double a = i * 2 * Math.PI / n;
			ring[i] = new double[] { Math.cos(a), Math.sin(a) };
		}
		return ring;
	}

	/**
	 * Resolves one tick of movement {@code (mx, my, mz)} for a player whose feet are centred at
	 * {@code (x0, y0, z0)}. Returns the allowed movement.
	 */
	public static double[] resolve(
		double x0, double y0, double z0, double radius, double height, double step, boolean wasOnGround, double mx, double my, double mz
	) {
		double x = x0;
		double z = z0;
		double y = y0;

		// 1) Horizontal, in sub-steps, sliding out of obstacles and ledges after each.
		double horizontal = Math.hypot(mx, mz);
		int steps = Math.max(1, (int) Math.ceil(horizontal / SUBSTEP));
		double wallFrom = wasOnGround ? step : 0.02;
		boolean hitWall = false;
		for (int i = 0; i < steps; i++) {
			x += mx / steps;
			z += mz / steps;
			double[] out = pushOut(x, y, z, radius, height, wallFrom, step);
			hitWall |= out[0] != x || out[1] != z;
			x = out[0];
			z = out[1];
		}

		// 2) Vertical.
		double dy = my;
		double walkUp = wasOnGround ? step : AIR_STEP;
		double floorWalk = floor(x, z, y + walkUp);
		double floorAny = floor(x, z, y + EPS);
		double floor = Math.max(floorWalk, floorAny);
		double targetY = y + dy;
		double outY;
		if (targetY <= floor) {
			outY = floor - y0; // land / stand / walk up a slope or small ledge
		} else if (wasOnGround && dy <= 0 && floorWalk > Double.NEGATIVE_INFINITY && y - floorWalk <= Math.max(step, horizontal * 1.5)) {
			outY = floorWalk - y0; // stick to the ground going downhill instead of hopping
		} else {
			outY = dy; // free movement
		}
		// Minecraft decides "did I collide?" by exact equality against what it asked for, so an axis we didn't
		// change must come back bit-for-bit identical.
		return new double[] { hitWall ? x - x0 : mx, outY, hitWall ? z - z0 : mz };
	}

	/**
	 * If the player is inside an obstacle or has sunk into the ground, how far to move them to get out
	 * ({@code {dx, dy, dz}}); otherwise null.
	 */
	public static double @Nullable [] depenetrate(double x, double y, double z, double radius, double height, double step) {
		double[] out = pushOut(x, y, z, radius, height, 0.02, step);
		double ny = y;
		double ground = GroundCollision.surfaceAt(out[0], out[1]);
		// Only a modest distance below: further down is a different level (under a bridge, say), not a clip.
		if (!Double.isNaN(ground) && y < ground - 0.05 && y > ground - 3.0) {
			ny = ground;
		}
		if (out[0] == x && out[1] == z && ny == y) {
			return null;
		}
		return new double[] { out[0] - x, ny - y, out[1] - z };
	}

	/** Highest ground under the footprint at most up to {@code limit} (or -inf). */
	private static double floor(double x, double z, double limit) {
		double best = Double.NEGATIVE_INFINITY;
		for (double[] s : FLOOR_SAMPLES) {
			double h = GroundCollision.surfaceAt(x + s[0], z + s[1]);
			if (!Double.isNaN(h) && h <= limit && h > best) {
				best = h;
			}
		}
		return best;
	}

	/** Moves a circle at (x, z) out of obstacles and ledges. Returns the new {x, z}. */
	private static double[] pushOut(double x, double y, double z, double radius, double height, double wallFrom, double step) {
		for (int pass = 0; pass < PASSES; pass++) {
			boolean moved = false;

			List<Box> boxes = new ArrayList<>();
			gatherBoxes(x, z, radius, boxes);
			for (Box b : boxes) {
				if (b.high <= y + wallFrom || b.low >= y + height) {
					continue; // entirely below the feet (steppable) or above the head
				}
				double cx = Math.max(b.minX, Math.min(x, b.maxX));
				double cz = Math.max(b.minZ, Math.min(z, b.maxZ));
				double dx = x - cx;
				double dz = z - cz;
				double dist = Math.hypot(dx, dz);
				if (dist >= radius) {
					continue;
				}
				if (dist > 1.0e-9) {
					double push = (radius - dist) / dist;
					x += dx * push;
					z += dz * push;
				} else {
					// The centre is inside the box: leave by the nearest side.
					double left = x - b.minX;
					double right = b.maxX - x;
					double near = z - b.minZ;
					double far = b.maxZ - z;
					double m = Math.min(Math.min(left, right), Math.min(near, far));
					if (m == left) {
						x = b.minX - radius;
					} else if (m == right) {
						x = b.maxX + radius;
					} else if (m == near) {
						z = b.minZ - radius;
					} else {
						z = b.maxZ + radius;
					}
				}
				moved = true;
			}

			double[] ledge = ledgePush(x, y, z, radius, step);
			if (ledge != null) {
				x += ledge[0];
				z += ledge[1];
				moved = true;
			}
			if (!moved) {
				break;
			}
		}
		return new double[] { x, z };
	}

	/** If ground that is too high to step up onto reaches into the circle, how far to move away from it (else null). */
	private static double @Nullable [] ledgePush(double x, double y, double z, double radius, double step) {
		double limit = y + step + 0.02;
		double bestPenetration = 0;
		double pushX = 0;
		double pushZ = 0;
		for (double[] d : RING) {
			double s = GroundCollision.surfaceAt(x + d[0] * radius, z + d[1] * radius);
			if (Double.isNaN(s) || s <= limit) {
				continue;
			}
			// Where along the way out does the ground get too high?
			double t = radius;
			for (double u = 0; u < radius; u += 0.05) {
				double su = GroundCollision.surfaceAt(x + d[0] * u, z + d[1] * u);
				if (!Double.isNaN(su) && su > limit) {
					t = u;
					break;
				}
			}
			double penetration = radius - t;
			if (penetration > bestPenetration) {
				bestPenetration = penetration;
				pushX = -d[0] * penetration;
				pushZ = -d[1] * penetration;
			}
		}
		return bestPenetration > 1.0e-6 ? new double[] { pushX, pushZ } : null;
	}

	/** The blocked sub-squares of the cells near (x, z), as boxes (runs along X are merged). */
	private static void gatherBoxes(double x, double z, double radius, List<Box> out) {
		int bx0 = (int) Math.floor(x - radius);
		int bx1 = (int) Math.floor(x + radius);
		int bz0 = (int) Math.floor(z - radius);
		int bz1 = (int) Math.floor(z + radius);
		double size = 1.0 / OBSTACLE_SUB;
		for (int bx = bx0; bx <= bx1; bx++) {
			for (int bz = bz0; bz <= bz1; bz++) {
				long mask = CyberLink.obstacleMask(bx, bz);
				if (mask == 0) {
					continue;
				}
				float ground = CyberLink.groundHeight(bx, bz);
				if (Float.isNaN(ground) || Float.isInfinite(ground)) {
					continue;
				}
				double low = ground - 0.5;
				double high = ground + OBSTACLE_HEIGHT;
				for (int sz = 0; sz < OBSTACLE_SUB; sz++) {
					int sx = 0;
					while (sx < OBSTACLE_SUB) {
						if ((mask & (1L << (sx + OBSTACLE_SUB * sz))) == 0) {
							sx++;
							continue;
						}
						int end = sx + 1;
						while (end < OBSTACLE_SUB && (mask & (1L << (end + OBSTACLE_SUB * sz))) != 0) {
							end++;
						}
						double minX = bx + sx * size;
						double maxX = bx + end * size;
						double minZ = bz + sz * size;
						double maxZ = minZ + size;
						if (maxX >= x - radius && minX <= x + radius && maxZ >= z - radius && minZ <= z + radius) {
							out.add(new Box(minX, maxX, minZ, maxZ, low, high));
						}
						sx = end;
					}
				}
			}
		}
	}
}
