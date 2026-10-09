package dev.cybercraft.client;

import dev.cybercraft.CyberCraft;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.util.ArrayList;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;

/**
 * Reads Minecraft's time of day and weather from the client's world for CyberLink, and holds the two switches of /ccsync.
 *
 * The names of the methods differ between Minecraft versions and mappings (and recent versions have changed how the clock works), so they are not fixed in the code: when the
 * world's class first shows up, its methods (and those of its level data) are scanned for the ones that look like the time of day and the rain and thunder levels, the best
 * are chosen, and everything that was considered is kept for /ccsync status, which prints it in chat. If nothing fits, the values stay at zero and nothing else breaks.
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
	private static Method dataGetter; // the world's level data, when the time is read from there
	private static Method rainMethod;
	private static Method thunderMethod;
	private static final List<String> notes = new ArrayList<>();
	private static boolean failureLogged;

	private WorldSync() {
	}

	/** What was found, one line each, for /ccsync status and the log. */
	static List<String> status() {
		List<String> lines = new ArrayList<>();
		lines.add("world sync: time sync " + (syncTime ? "ON" : "off") + ", weather sync " + (syncWeather ? "ON" : "off"));
		lines.add("time of day: " + (dayMethod != null ? dayMethod.getName() + (dataGetter != null ? " (on the level data)" : "") : "NOT FOUND") + "; reading it now: "
			+ (ok ? dayTime + " ticks" : "no") + "; rain: " + (rainMethod != null ? rainMethod.getName() : "NOT FOUND") + " = " + rain + "; thunder: "
			+ (thunderMethod != null ? thunderMethod.getName() : "NOT FOUND") + " = " + thunder);
		lines.addAll(notes);
		return lines;
	}

	static void read(Object level) {
		ok = false;
		if (level == null) {
			return;
		}
		if (resolvedFor != level.getClass()) {
			resolvedFor = level.getClass();
			resolve(level);
		}
		try {
			if (dayMethod != null) {
				Object target = level;
				if (dataGetter != null) {
					target = dataGetter.invoke(level);
				}
				if (target != null) {
					dayTime = ((Number) dayMethod.invoke(target)).longValue();
					ok = true;
				}
			}
			if (rainMethod != null) {
				rain = ((Number) rainMethod.invoke(level, 1.0f)).floatValue();
			}
			if (thunderMethod != null) {
				thunder = ((Number) thunderMethod.invoke(level, 1.0f)).floatValue();
			}
		} catch (ReflectiveOperationException | RuntimeException e) {
			if (!failureLogged) {
				failureLogged = true;
				CyberCraft.LOG.warn("CyberCraft: world sync: could not read the world's time or weather: {}", e.toString());
			}
		}
	}

	private static void resolve(Object level) {
		notes.clear();
		Class<?> type = level.getClass();
		dayMethod = pickTime(type, "the world");
		dataGetter = null;
		if (dayMethod == null) {
			// Some versions keep the time in the world's level data.
			Method getData = noArg(type, "getLevelData");
			if (getData != null) {
				try {
					Object data = getData.invoke(level);
					if (data != null) {
						Method m = pickTime(data.getClass(), "the level data");
						if (m != null) {
							dayMethod = m;
							dataGetter = getData;
						}
					}
				} catch (ReflectiveOperationException | RuntimeException ignored) {
					// reported below as not found
				}
			}
		}
		rainMethod = pickLevel(type, "rain");
		thunderMethod = pickLevel(type, "thunder");
		for (String line : status()) {
			CyberCraft.LOG.info("CyberCraft: {}", line);
		}
	}

	private static List<Method> methodsOf(Class<?> type) {
		Set<Method> all = new LinkedHashSet<>();
		for (Method m : type.getMethods()) {
			all.add(m);
		}
		for (Class<?> k = type; k != null; k = k.getSuperclass()) {
			for (Method m : k.getDeclaredMethods()) {
				all.add(m);
			}
		}
		return new ArrayList<>(all);
	}

	private static Method noArg(Class<?> type, String name) {
		for (Method m : methodsOf(type)) {
			if (m.getParameterCount() == 0 && m.getName().equals(name)) {
				m.setAccessible(true);
				return m;
			}
		}
		return null;
	}

	/** The no-argument method returning a long that looks most like "the time of day"; every candidate is noted. */
	private static Method pickTime(Class<?> type, String where) {
		Method best = null;
		int bestScore = 0;
		for (Method m : methodsOf(type)) {
			if (m.getParameterCount() != 0 || Modifier.isStatic(m.getModifiers())) {
				continue;
			}
			Class<?> r = m.getReturnType();
			if (r != long.class && r != Long.class) {
				continue;
			}
			String n = m.getName().toLowerCase();
			if (!n.contains("time") && !n.contains("clock")) {
				continue;
			}
			int score = n.equals("getdaytime") || n.equals("daytime") ? 100 : n.contains("daytime") ? 90 : n.contains("clocktime") ? 85 : n.contains("clock") ? 60 : 0;
			notes.add("candidate on " + where + ": " + m.getName() + "() -> long, score " + score);
			if (score > bestScore) {
				bestScore = score;
				best = m;
			}
		}
		if (best != null) {
			best.setAccessible(true);
		}
		return best;
	}

	/** The method taking a float (the partial tick) and returning a float whose name has "rain" or "thunder" (preferably with "level"). */
	private static Method pickLevel(Class<?> type, String key) {
		Method best = null;
		int bestScore = 0;
		for (Method m : methodsOf(type)) {
			if (m.getParameterCount() != 1 || m.getParameterTypes()[0] != float.class || Modifier.isStatic(m.getModifiers())) {
				continue;
			}
			Class<?> r = m.getReturnType();
			if (r != float.class && r != Float.class) {
				continue;
			}
			String n = m.getName().toLowerCase();
			if (!n.contains(key)) {
				continue;
			}
			int score = n.contains("level") ? 100 : 50;
			notes.add("candidate: " + m.getName() + "(float) -> float, score " + score);
			if (score > bestScore) {
				bestScore = score;
				best = m;
			}
		}
		if (best != null) {
			best.setAccessible(true);
		}
		return best;
	}
}
