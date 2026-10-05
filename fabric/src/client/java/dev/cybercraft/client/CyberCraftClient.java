package dev.cybercraft.client;

import com.mojang.brigadier.arguments.DoubleArgumentType;
import dev.cybercraft.CyberCraft;
import dev.cybercraft.link.Proto;
import net.fabricmc.api.ClientModInitializer;
import net.fabricmc.fabric.api.client.command.v2.ClientCommandRegistrationCallback;
import net.fabricmc.fabric.api.client.command.v2.ClientCommands;
import net.fabricmc.fabric.api.client.event.lifecycle.v1.ClientTickEvents;
import net.minecraft.client.Minecraft;
import net.minecraft.client.player.LocalPlayer;
import net.minecraft.core.BlockPos;
import net.minecraft.network.chat.Component;
import net.minecraft.server.level.ServerPlayer;
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

			// /ccfollow  builds Night City's ground around V, moves you onto it, and V starts copying you.
			// /ccstop    V stops following, and the ground stops being updated.
			dispatcher.register(ClientCommands.literal("ccfollow").executes(c -> {
				c.getSource().sendFeedback(Component.literal(startFollowing()));
				return 1;
			}));
			dispatcher.register(ClientCommands.literal("ccstop").executes(c -> {
				c.getSource().sendFeedback(Component.literal(stopFollowing()));
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

		// Start building the ground, and put the player at V's spot (above the ground if we already know it).
		GroundMirror.start();
		double surface = GroundMirror.surfaceAt(STATE.x, STATE.z);
		holdX = STATE.x;
		holdZ = STATE.z;
		holdY = Double.isNaN(surface) ? STATE.y + 2.0 : surface;
		teleportPlayer(client, holdX, holdY, holdZ);
		following = false;
		syncing = true;
		syncStartedMs = System.currentTimeMillis();
		CyberCraft.LOG.info("CyberCraft: moving the player to V's spot ({}, {}, {}) and building the ground",
			String.format("%.2f", holdX), String.format("%.2f", holdY), String.format("%.2f", holdZ));
		return "CyberCraft: building Night City's ground around V. V will follow you once it's ready. /ccstop to stop.";
	}

	private static String stopFollowing() {
		if (!following && !syncing && !GroundMirror.enabled()) {
			return "CyberCraft: nothing to stop.";
		}
		following = false;
		syncing = false;
		GroundMirror.stop();
		CyberCraft.LOG.info("CyberCraft: follow off");
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
			GroundMirror.stop();
		}

		boolean second = ++ticks % 20 == 0;
		boolean needState = second || pendingSeq != 0 || following || syncing || GroundMirror.enabled();
		boolean haveState = needState && CyberLink.readGameState(STATE);

		if (haveState) {
			GroundMirror.tick(client, STATE);
		}

		// Step 1: hold the player at V's spot until the ground has been built under them.
		if (syncing && player != null) {
			// Stand on the ground as soon as Cyberpunk has told us where it is; until then hover near V.
			double surface = GroundMirror.surfaceAt(holdX, holdZ);
			boolean known = !Double.isNaN(surface);
			if (known) {
				holdY = surface;
			}
			player.setPos(holdX, holdY, holdZ);
			player.setDeltaMovement(Vec3.ZERO);
			player.resetFallDistance();
			BlockPos below = new BlockPos((int) Math.floor(holdX), (int) Math.floor(holdY) - 1, (int) Math.floor(holdZ));
			if (known && client.level != null && !client.level.getBlockState(below).isAir()) {
				syncing = false;
				following = true;
				CyberCraft.LOG.info("CyberCraft: ground is ready, V now follows the player");
			} else if (System.currentTimeMillis() - syncStartedMs > SYNC_TIMEOUT_MS) {
				syncing = false;
				GroundMirror.stop();
				CyberCraft.LOG.warn("CyberCraft: gave up waiting for the ground under V ({}; ground under V known: {})", GroundMirror.describe(), known);
			} else if (second) {
				CyberCraft.LOG.info("CyberCraft: waiting for the ground under V ({}; ground under V known: {})", GroundMirror.describe(), known);
			}
		}

		// Tell Cyberpunk where Minecraft's player is (every tick). With follow off it just says "don't move V".
		if (player == null) {
			CyberLink.publishMcState(false, false, 0, 0, 0, 0, 0);
		} else if (following) {
			// V's height is the player's, corrected for the gap between the block tops and the real street.
			double x = player.getX();
			double z = player.getZ();
			double y = player.getY() + GroundMirror.groundError(x, z);
			CyberLink.publishMcState(true, true, x, y, z, player.getYRot(), player.getXRot());
		} else {
			CyberLink.publishMcState(true, false, 0, 0, 0, player.getYRot(), player.getXRot());
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
