package dev.cybercraft.client;

import com.mojang.brigadier.arguments.DoubleArgumentType;
import dev.cybercraft.CyberCraft;
import dev.cybercraft.link.CyberLink;
import dev.cybercraft.link.Proto;
import dev.cybercraft.world.GroundCollision;
import dev.cybercraft.world.SmoothCollider;
import net.fabricmc.api.ClientModInitializer;
import net.fabricmc.fabric.api.client.command.v2.ClientCommandRegistrationCallback;
import net.fabricmc.fabric.api.client.command.v2.ClientCommands;
import net.fabricmc.fabric.api.client.event.lifecycle.v1.ClientTickEvents;
import net.minecraft.client.Minecraft;
import net.minecraft.client.player.LocalPlayer;
import net.minecraft.core.BlockPos;
import net.minecraft.network.chat.Component;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.phys.AABB;
import net.minecraft.world.phys.Vec3;

public final class CyberCraftClient implements ClientModInitializer {
	private static final CyberLink.GameState STATE = new CyberLink.GameState();
	private static final long SYNC_TIMEOUT_MS = 20_000;

	private static int ticks;
	private static boolean wasActive;
	// The teleport we asked for and haven't heard back about yet (0: none).
	private static int pendingSeq;

	// /ccfollow, step 1 ("syncing"): the player is moved to V's spot and held there until the ground
	// under it has been built. Step 2 ("following"): V copies the player's movement.
	private static boolean syncing;
	private static boolean following;
	private static double holdX, holdY, holdZ;
	private static long syncStartedMs;

	@Override
	public void onInitializeClient() {
		CyberCraft.LOG.info("CyberCraft: client started; waiting for Cyberpunk");
		ClientTickEvents.END_CLIENT_TICK.register(client -> tick(client));

		ClientCommandRegistrationCallback.EVENT.register((dispatcher, context) -> {
			// /cctp                      moves V 5 m straight up
			// /cctp <east> <north> <up>  moves V by that many metres (Cyberpunk's axes; negative goes the other way)
			dispatcher.register(
				ClientCommands.literal("cctp")
					.executes(c -> {
						c.getSource().sendFeedback(Component.literal(requestTeleport(0, 0, 5)));
						return 1;
					})
					.then(ClientCommands.argument("east", DoubleArgumentType.doubleArg(-200, 200))
						.then(ClientCommands.argument("north", DoubleArgumentType.doubleArg(-200, 200))
							.then(ClientCommands.argument("up", DoubleArgumentType.doubleArg(-50, 200))
								.executes(c -> {
									double east = DoubleArgumentType.getDouble(c, "east");
									double north = DoubleArgumentType.getDouble(c, "north");
									double up = DoubleArgumentType.getDouble(c, "up");
									c.getSource().sendFeedback(Component.literal(requestTeleport(east, north, up)));
									return 1;
								}))))
			);

			// /ccfollow  makes Night City's ground solid around V, moves you onto it, and V starts copying you.
			// /ccstop    V stops following, and the ground stops being solid.
			dispatcher.register(ClientCommands.literal("ccfollow").executes(c -> {
				c.getSource().sendFeedback(Component.literal(startFollowing()));
				return 1;
			}));
			dispatcher.register(ClientCommands.literal("ccstop").executes(c -> {
				c.getSource().sendFeedback(Component.literal(stopFollowing()));
				return 1;
			}));
			// /cccollider  switches between smooth collision (the default) and block-style collision (the old way).
			dispatcher.register(ClientCommands.literal("cccollider").executes(c -> {
				boolean smooth = !GroundCollision.smoothPlayers();
				GroundCollision.setSmoothPlayers(smooth);
				c.getSource().sendFeedback(Component.literal(smooth
					? "CyberCraft: smooth collision on (the default)."
					: "CyberCraft: block-style collision on (the old way)."));
				return 1;
			}));
			// /ccblocks  shows the ground as real blocks too (a debugging view of what Cyberpunk found).
			dispatcher.register(ClientCommands.literal("ccblocks").executes(c -> {
				if (GroundMirror.enabled()) {
					GroundMirror.stop();
					c.getSource().sendFeedback(Component.literal("CyberCraft: ground blocks off (the ground still collides)."));
				} else {
					GroundMirror.start();
					c.getSource().sendFeedback(Component.literal("CyberCraft: building the ground as blocks as well."));
				}
				return 1;
			}));
		});
	}

	/** Asks Cyberpunk to move V by an offset from where V is now. Returns a message for the chat. */
	private static String requestTeleport(double east, double north, double up) {
		if (!CyberLink.active()) {
			return "CyberCraft: Cyberpunk isn't linked. Start the game with the plugin installed.";
		}
		if (!CyberLink.readGameState(STATE) || !STATE.inGame()) {
			return "CyberCraft: Cyberpunk is linked, but no save is loaded.";
		}
		// Cyberpunk: X east, Y north, Z up.  Minecraft: X east, Y up, -Z north.
		double x = STATE.x + east;
		double y = STATE.y + up;
		double z = STATE.z - north;
		int seq = CyberLink.sendTeleport(x, y, z);
		if (seq == 0) {
			return "CyberCraft: couldn't send the command.";
		}
		pendingSeq = seq;
		CyberCraft.LOG.info("CyberCraft: asked Cyberpunk to teleport V by east={} north={} up={} (command {})", east, north, up, seq);
		return String.format("CyberCraft: asked Cyberpunk to move V by east %.1f, north %.1f, up %.1f m. Check the log for the result.", east, north, up);
	}

	private static String startFollowing() {
		Minecraft client = Minecraft.getInstance();
		LocalPlayer player = client.player;
		if (player == null) {
			return "CyberCraft: open a world first.";
		}
		if (client.getSingleplayerServer() == null) {
			return "CyberCraft: this needs a singleplayer world.";
		}
		if (!CyberLink.active()) {
			return "CyberCraft: Cyberpunk isn't linked. Start the game with the plugin installed.";
		}
		if (!CyberLink.readGameState(STATE) || !STATE.inGame()) {
			return "CyberCraft: Cyberpunk is linked, but no save is loaded.";
		}

		// Make the ground solid, and put the player at V's spot (just above the ground if we already know it).
		GroundCollision.setEnabled(true);
		float ground = CyberLink.groundHeight((int) Math.floor(STATE.x), (int) Math.floor(STATE.z));
		holdX = STATE.x;
		holdZ = STATE.z;
		holdY = Float.isNaN(ground) || Float.isInfinite(ground) ? STATE.y + 1.0 : ground + 0.05;
		teleportPlayer(client, holdX, holdY, holdZ);
		following = false;
		syncing = true;
		syncStartedMs = System.currentTimeMillis();
		CyberCraft.LOG.info("CyberCraft: moving the player to V's spot ({}, {}, {}) and making the ground solid",
			String.format("%.2f", holdX), String.format("%.2f", holdY), String.format("%.2f", holdZ));
		return "CyberCraft: making Night City's ground solid around V. V will follow you once it's ready. /ccstop to stop.";
	}

	private static String stopFollowing() {
		if (!following && !syncing && !GroundCollision.enabled() && !GroundMirror.enabled()) {
			return "CyberCraft: nothing to stop.";
		}
		following = false;
		syncing = false;
		GroundCollision.setEnabled(false);
		GroundMirror.stop();
		CyberCraft.LOG.info("CyberCraft: follow off");
		if (routedFlag) {
			routedFlag = false;
			InputBridge.releaseAll();
		}
		return "CyberCraft: V stopped following.";
	}

	/** Moves the Minecraft player (and its copy on the integrated server) to a position. Same approach as SkyCraft. */
	private static void teleportPlayer(Minecraft client, double x, double y, double z) {
		LocalPlayer player = client.player;
		player.setPos(x, y, z);
		player.setDeltaMovement(Vec3.ZERO);
		player.resetFallDistance();
		var server = client.getSingleplayerServer();
		if (server != null) {
			var uuid = player.getUUID();
			server.execute(() -> {
				ServerPlayer sp = server.getPlayerList().getPlayer(uuid);
				if (sp != null) {
					sp.teleportTo(x, y, z);
					sp.resetFallDistance();
				}
			});
		}
	}

	private static int pushCount;
	private static volatile boolean routedFlag;

	/** True while Cyberpunk is sending its keyboard and mouse to Minecraft: Minecraft then ignores its own window's. */
	public static boolean inputRouted() {
		return routedFlag;
	}

	/**
	 * Start of every Minecraft frame (see MinecraftMixin): replay the keyboard and mouse Cyberpunk captured since
	 * last frame, and take the look direction from it, so looking around has no tick-rate delay.
	 */
	public static void beginFrame() {
		CyberLink.poll();
		Minecraft client = Minecraft.getInstance();
		if (!CyberLink.active() || !CyberLink.readGameState(STATE)) {
			if (routedFlag) {
				routedFlag = false;
				InputBridge.releaseAll();
			}
			return;
		}
		boolean routing = STATE.routing() && (following || syncing);
		if (routing != routedFlag) {
			routedFlag = routing;
			if (!routing) {
				InputBridge.releaseAll();
			}
			CyberCraft.LOG.info(routing ? "CyberCraft: Cyberpunk's keyboard and mouse now control Minecraft" : "CyberCraft: Minecraft's keyboard and mouse are back to normal");
		}
		if (!routing) {
			return;
		}
		InputBridge.drain(client);
		LocalPlayer player = client.player;
		if (player != null && client.gui.screen() == null) {
			player.setYRot(STATE.lookYaw);
			player.setXRot(STATE.lookPitch);
			player.yRotO = STATE.lookYaw;
			player.xRotO = STATE.lookPitch;
		}
	}

	/**
	 * True while Cyberpunk is the one showing the world (following, or about to): Minecraft then draws only its
	 * hand, HUD and screens on a transparent background and sends them to Cyberpunk to draw over the game.
	 */
	public static boolean overlayActive() {
		return (following || syncing) && CyberLink.active();
	}

	private static void keepOutOfGround(Minecraft client, LocalPlayer player) {
		double x = player.getX();
		double y = player.getY();
		double z = player.getZ();
		if (GroundCollision.smoothPlayers()) {
			AABB box = player.getBoundingBox();
			double[] fix = SmoothCollider.depenetrate(
				(box.minX + box.maxX) * 0.5, box.minY, (box.minZ + box.maxZ) * 0.5, box.getXsize() * 0.5, box.getYsize(), player.maxUpStep()
			);
			if (fix == null) {
				return;
			}
			x += fix[0];
			y += fix[1];
			z += fix[2];
		} else {
			Vec3 push = GroundCollision.pushOut(player.getBoundingBox());
			if (push != null) {
				x += push.x;
				y += push.y;
				z += push.z;
			} else {
				// Fell through the ground without touching anything (no ground shape deep enough)? Lift back onto it.
				// Only a modest distance below: further down is a different level (under a bridge, say), not a clip.
				float ground = CyberLink.groundHeight((int) Math.floor(x), (int) Math.floor(z));
				if (Float.isNaN(ground) || Float.isInfinite(ground) || y > ground - 0.6 || y < ground - 3.0) {
					return;
				}
				y = ground + 0.05;
			}
		}
		if (pushCount++ % 40 == 0) {
			CyberCraft.LOG.info("CyberCraft: moved the player out of the ground or an obstacle ({} times so far)", pushCount);
		}
		teleportPlayer(client, x, y, z);
	}

	private static void tick(Minecraft client) {
		CyberLink.poll();

		boolean active = CyberLink.active();
		if (active != wasActive) {
			wasActive = active;
			CyberCraft.LOG.info(active ? "CyberCraft: Cyberpunk link is up (pid {})" : "CyberCraft: Cyberpunk link lost", CyberLink.gamePid());
			if (!active) {
				pendingSeq = 0;
				following = false;
				syncing = false;
				GroundCollision.setEnabled(false);
				GroundMirror.stop();
			}
		}
		if (!active) {
			return;
		}

		LocalPlayer player = client.player;
		if (player == null) {
			following = false;
			syncing = false;
			GroundCollision.setEnabled(false);
			GroundMirror.stop();
		}

		boolean second = ++ticks % 20 == 0;
		boolean needState = second || pendingSeq != 0 || following || syncing || GroundMirror.enabled();
		boolean haveState = needState && CyberLink.readGameState(STATE);

		if (haveState) {
			GroundMirror.tick(client, STATE);
		}

		// Step 1: hold the player at V's spot until the ground under them is known and their part of the world is loaded.
		if (syncing && player != null) {
			player.setPos(holdX, holdY, holdZ);
			player.setDeltaMovement(Vec3.ZERO);
			player.resetFallDistance();
			float ground = CyberLink.groundHeight((int) Math.floor(holdX), (int) Math.floor(holdZ));
			boolean groundKnown = !Float.isNaN(ground) && !Float.isInfinite(ground);
			BlockPos here = new BlockPos((int) Math.floor(holdX), (int) Math.floor(holdY), (int) Math.floor(holdZ));
			if (groundKnown && client.level != null && client.level.isLoaded(here)) {
				holdY = ground + 0.05;
				player.setPos(holdX, holdY, holdZ);
				syncing = false;
				following = true;
				CyberCraft.LOG.info("CyberCraft: ground is ready, V now follows the player");
			} else if (System.currentTimeMillis() - syncStartedMs > SYNC_TIMEOUT_MS) {
				syncing = false;
				GroundCollision.setEnabled(false);
				CyberCraft.LOG.warn("CyberCraft: gave up waiting for the ground under V. Is V standing on ground, and is the plugin's log showing ground rays?");
			}
		}

		// Safety net: Minecraft only stops the player moving *into* the ground and obstacles. If the player ends up
		// inside one anyway (an obstacle found after they walked into it, a gap in the scan), they would walk
		// through everything, so move them back out.
		if (following && player != null) {
			keepOutOfGround(client, player);
		}

		// Tell Cyberpunk where Minecraft's player is (every tick). With follow off it just says "don't move V".
		boolean screenOpen = client.gui.screen() != null;
		float sensitivity = client.options.sensitivity().get().floatValue();
		if (player == null) {
			CyberLink.publishMcState(false, false, false, sensitivity, 0, 0, 0, 0, 0);
		} else if (following) {
			// The ground Minecraft walks on is Night City's own, so the player's position is V's position.
			CyberLink.publishMcState(true, true, screenOpen, sensitivity, player.getX(), player.getY(), player.getZ(), player.getYRot(), player.getXRot());
		} else {
			CyberLink.publishMcState(true, false, screenOpen, sensitivity, 0, 0, 0, player.getYRot(), player.getXRot());
		}

		if (!haveState) {
			return;
		}

		// Did Cyberpunk deal with our teleport?
		if (pendingSeq != 0 && STATE.cmdAck == pendingSeq) {
			CyberCraft.LOG.info("CyberCraft: command {} {}", pendingSeq, STATE.cmdResult == Proto.RESULT_OK ? "done: Cyberpunk teleported V" : "FAILED: see Cyberpunk's CyberCraft.log");
			pendingSeq = 0;
		}

		// Once a second: say where V is.
		if (second) {
			if (STATE.inGame()) {
				CyberCraft.LOG.info("CyberCraft: V is at x={} y={} z={} (Minecraft coordinates, frame {})",
					String.format("%.2f", STATE.x), String.format("%.2f", STATE.y), String.format("%.2f", STATE.z), STATE.frame);
			} else {
				CyberCraft.LOG.info("CyberCraft: linked, but no save is loaded yet");
			}
		}
	}
}
