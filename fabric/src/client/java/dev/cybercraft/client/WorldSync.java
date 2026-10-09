package dev.cybercraft.client;

import dev.cybercraft.CyberCraft;
import java.lang.reflect.Method;

/**
 * Reads Minecraft's time of day and weather from the client's world for CyberLink, and holds the two switches of /ccsync.
 *
 * The methods are looked up by name when the world's class first shows up (their names differ between mappings), and the log says which were found, so if one is missing the
 * values that need it just stay at zero instead of the mod failing.
 */
final class WorldSync {
	/** The game's time of day follows Minecraft's (/ccsync time). */
	static volatile boolean syncTime = false;
	/** The game's weather follows Minecraft's (/ccsync weather). */
	static volatile boolean syncWeather = false;

	static long dayTime;
	static float rain;
	static float thunder;
	/** True if the day time could be read from the world this tick. */
	static boolean ok;

	private static Class<?> resolvedFor;
	private static Method dayMethod;
	private static Method rainMethod;
	private static Method thunderMethod;
	private static boolean failureLogged;

	private WorldSync() {
	}

	static void read(Object level) {
		ok = false;
		if (level == null) {
			return;
		}
		if (resolvedFor != level.getClass()) {
			resolvedFor = level.getClass();
			dayMethod = find(resolvedFor, new String[] { "getDayTime", "dayTime" });
			rainMethod = find(resolvedFor, new String[] { "getRainLevel", "rainLevel" }, float.class);
			thunderMethod = find(resolvedFor, new String[] { "getThunderLevel", "thunderLevel" }, float.class);
			CyberCraft.LOG.info("CyberCraft: world sync: Minecraft's time of day {}, rain {}, thunder {}", dayMethod != null ? "found (" + dayMethod.getName() + ")" : "NOT FOUND",
				rainMethod != null ? "found (" + rainMethod.getName() + ")" : "NOT FOUND", thunderMethod != null ? "found (" + thunderMethod.getName() + ")" : "NOT FOUND");
		}
		try {
			if (dayMethod != null) {
				dayTime = ((Number) dayMethod.invoke(level)).longValue();
			}
			if (rainMethod != null) {
				rain = ((Number) rainMethod.invoke(level, 1.0f)).floatValue();
			}
			if (thunderMethod != null) {
				thunder = ((Number) thunderMethod.invoke(level, 1.0f)).floatValue();
			}
			ok = dayMethod != null;
		} catch (ReflectiveOperationException | RuntimeException e) {
			if (!failureLogged) {
				failureLogged = true;
				CyberCraft.LOG.warn("CyberCraft: world sync: could not read the world's time or weather: {}", e.toString());
			}
		}
	}

	private static Method find(Class<?> type, String[] names, Class<?>... args) {
		for (String name : names) {
			try {
				Method m = type.getMethod(name, args);
				m.setAccessible(true);
				return m;
			} catch (NoSuchMethodException | RuntimeException ignored) {
				// try the next name, and the non-public methods below
			}
			for (Class<?> k = type; k != null; k = k.getSuperclass()) {
				try {
					Method m = k.getDeclaredMethod(name, args);
					m.setAccessible(true);
					return m;
				} catch (NoSuchMethodException | RuntimeException ignored) {
					// keep going up
				}
			}
		}
		return null;
	}
}
