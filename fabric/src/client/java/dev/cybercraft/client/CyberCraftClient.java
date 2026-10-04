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
import net.minecraft.network.chat.Component;

public final class CyberCraftClient implements ClientModInitializer {
	private static final CyberLink.GameState STATE = new CyberLink.GameState();
	private static int ticks;
	private static boolean wasActive;
	// The teleport we asked for and haven't heard back about yet (0: none).
	private static int pendingSeq;

	// Follow mode: V is moved to where Minecraft's player is, plus a fixed offset. The offset is worked out
	// when /ccfollow is typed, so V starts exactly where V is and moves by whatever Minecraft's player moves.
	private static boolean following;
	private static double offsetX, offsetY, offsetZ;

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

			// /ccfollow  V starts copying this Minecraft player's movement.   /ccstop  V stops.
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
		LocalPlayer player = Minecraft.getInstance().player;
		if (player == null) {
			return "CyberCraft: open a world first.";
		}
		if (!CyberLink.active()) {
			return "CyberCraft: Cyberpunk isn't linked. Start the game with the plugin installed.";
		}
		if (!CyberLink.readGameState(STATE) || !STATE.inGame()) {
			return "CyberCraft: Cyberpunk is linked, but no save is loaded.";
		}
		offsetX = STATE.x - player.getX();
		offsetY = STATE.y - player.getY();
		offsetZ = STATE.z - player.getZ();
		following = true;
		CyberCraft.LOG.info("CyberCraft: follow on (offset {}, {}, {})", String.format("%.2f", offsetX), String.format("%.2f", offsetY), String.format("%.2f", offsetZ));
		return "CyberCraft: V now follows you. Walk around in this window. /ccstop to stop.";
	}

	private static String stopFollowing() {
		if (!following) {
			return "CyberCraft: V wasn't following.";
		}
		following = false;
		CyberCraft.LOG.info("CyberCraft: follow off");
		return "CyberCraft: V stopped following.";
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
			}
		}
		if (!active) {
			return;
		}

		// Tell Cyberpunk where Minecraft's player is (every tick). With follow off it just says "don't move V".
		LocalPlayer player = client.player;
		if (player == null) {
			following = false;
			CyberLink.publishMcState(false, false, 0, 0, 0, 0, 0);
		} else if (following) {
			CyberLink.publishMcState(true, true, player.getX() + offsetX, player.getY() + offsetY, player.getZ() + offsetZ, player.getYRot(), player.getXRot());
		} else {
			CyberLink.publishMcState(true, false, 0, 0, 0, player.getYRot(), player.getXRot());
		}

		boolean second = ++ticks % 20 == 0;
		if (!second && pendingSeq == 0) {
			return;
		}
		if (!CyberLink.readGameState(STATE)) {
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
