package dev.cybercraft.client;

import com.mojang.brigadier.arguments.DoubleArgumentType;
import dev.cybercraft.CyberCraft;
import dev.cybercraft.link.Proto;
import net.fabricmc.api.ClientModInitializer;
import net.fabricmc.fabric.api.client.command.v2.ClientCommandRegistrationCallback;
import net.fabricmc.fabric.api.client.command.v2.ClientCommands;
import net.fabricmc.fabric.api.client.event.lifecycle.v1.ClientTickEvents;
import net.minecraft.network.chat.Component;

public final class CyberCraftClient implements ClientModInitializer {
	private static final CyberLink.GameState STATE = new CyberLink.GameState();
	private static int ticks;
	private static boolean wasActive;
	// The teleport we asked for and haven't heard back about yet (0: none).
	private static int pendingSeq;

	@Override
	public void onInitializeClient() {
		CyberCraft.LOG.info("CyberCraft: client started; waiting for Cyberpunk");
		ClientTickEvents.END_CLIENT_TICK.register(client -> tick());

		// /cctp                      moves V 5 m straight up
		// /cctp <east> <north> <up>  moves V by that many metres (Cyberpunk's axes; negative goes the other way)
		ClientCommandRegistrationCallback.EVENT.register((dispatcher, context) -> dispatcher.register(
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
		));
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

	private static void tick() {
		CyberLink.poll();

		boolean active = CyberLink.active();
		if (active != wasActive) {
			wasActive = active;
			CyberCraft.LOG.info(active ? "CyberCraft: Cyberpunk link is up (pid {})" : "CyberCraft: Cyberpunk link lost", CyberLink.gamePid());
			if (!active) {
				pendingSeq = 0;
			}
		}
		if (!active) {
			return;
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
