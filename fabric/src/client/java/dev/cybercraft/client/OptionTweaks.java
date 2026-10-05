package dev.cybercraft.client;

import dev.cybercraft.CyberCraft;
import java.lang.reflect.Method;

/**
 * Reads and sets Minecraft options by the name of their accessor, through reflection, so that an option that is
 * missing or renamed in some version only produces a log line instead of stopping the game from starting.
 */
final class OptionTweaks {
	private OptionTweaks() {
	}

	/** The value of the option whose accessor is {@code name}() (e.g. "damageTiltStrength"), or null if there is none. */
	static Object get(Object options, String name) {
		try {
			Object option = options.getClass().getMethod(name).invoke(options);
			return option.getClass().getMethod("get").invoke(option);
		} catch (ReflectiveOperationException | RuntimeException e) {
			CyberCraft.LOG.warn("CyberCraft: can't read the Minecraft option {} ({})", name, e.toString());
			return null;
		}
	}

	/** Sets the option whose accessor is {@code name}() to {@code value}. Returns false if that wasn't possible. */
	static boolean set(Object options, String name, Object value) {
		try {
			Object option = options.getClass().getMethod(name).invoke(options);
			Method set = null;
			for (Method m : option.getClass().getMethods()) {
				if (m.getName().equals("set") && m.getParameterCount() == 1) {
					set = m;
					break;
				}
			}
			if (set == null) {
				return false;
			}
			set.invoke(option, value);
			return true;
		} catch (ReflectiveOperationException | RuntimeException e) {
			CyberCraft.LOG.warn("CyberCraft: can't set the Minecraft option {} ({})", name, e.toString());
			return false;
		}
	}
}
