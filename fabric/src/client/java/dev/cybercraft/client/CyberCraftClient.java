package dev.cybercraft.client;

import dev.cybercraft.CyberCraft;
import net.fabricmc.api.ClientModInitializer;
import net.fabricmc.fabric.api.client.event.lifecycle.v1.ClientTickEvents;

public final class CyberCraftClient implements ClientModInitializer {
	private final CyberLink.GameState state = new CyberLink.GameState();
	private int ticks;
	private boolean wasActive;

	@Override
	public void onInitializeClient() {
		CyberCraft.LOG.info("CyberCraft: client started; waiting for Cyberpunk");
		ClientTickEvents.END_CLIENT_TICK.register(client -> this.tick());
	}

	private void tick() {
		CyberLink.poll();

		boolean active = CyberLink.active();
		if (active != this.wasActive) {
			this.wasActive = active;
			CyberCraft.LOG.info(active ? "CyberCraft: Cyberpunk link is up (pid {})" : "CyberCraft: Cyberpunk link lost", CyberLink.gamePid());
		}

		// Once a second (20 ticks): say where V is.
		if (++this.ticks % 20 != 0 || !active) {
			return;
		}
		if (CyberLink.readGameState(this.state)) {
			if (this.state.inGame()) {
				CyberCraft.LOG.info("CyberCraft: V is at x={} y={} z={} (Minecraft coordinates, frame {})",
					String.format("%.2f", this.state.x), String.format("%.2f", this.state.y), String.format("%.2f", this.state.z), this.state.frame);
			} else {
				CyberCraft.LOG.info("CyberCraft: linked, but no save is loaded yet");
			}
		}
	}
}
