package dev.cybercraft;

import net.fabricmc.api.ModInitializer;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

public final class CyberCraft implements ModInitializer {
	public static final String MOD_ID = "cybercraft";
	public static final Logger LOG = LoggerFactory.getLogger(MOD_ID);

	@Override
	public void onInitialize() {
		LOG.info("CyberCraft: loaded");
	}
}
