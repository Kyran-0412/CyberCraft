package dev.cybercraft.client;

import dev.cybercraft.CyberCraft;
import java.io.IOException;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Locale;
import java.util.stream.Collectors;

/**
 * Writes the names and signatures of Minecraft's camera and level-rendering classes to a text file, using
 * reflection on the Minecraft that is actually running. This is groundwork for drawing Minecraft's blocks
 * inside Cyberpunk: to make Minecraft look through Cyberpunk's camera and draw only blocks, I need the exact
 * method names of this version, which I can't see from outside. The command is /ccdump.
 */
public final class ApiDump {
	/** Class name, then a comma-separated list of words; only members whose names contain one are listed ("" = all). */
	private static final String[][] TARGETS = {
		{ "net.minecraft.client.Camera", "" },
		{ "net.minecraft.client.renderer.GameRenderer", "fov,camera,project,matrix,render,fog,sky,cloud,extract,depth,target,hand,resize,window" },
		{ "net.minecraft.client.renderer.LevelRenderer", "render,sky,cloud,fog,weather,target,setup,extract,depth,clear,visible,section,chunk,entit,particle" },
		{ "net.minecraft.client.renderer.state.level.CameraRenderState", "" },
		{ "net.minecraft.client.renderer.state.level.LevelRenderState", "" },
		{ "net.minecraft.client.renderer.state.level.SkyRenderState", "" },
		{ "net.minecraft.client.renderer.SkyRenderer", "" },
		{ "net.minecraft.client.renderer.CloudRenderer", "" },
		{ "net.minecraft.client.renderer.fog.FogRenderer", "" },
		{ "net.minecraft.client.renderer.LevelTargetBundle", "" },
		{ "com.mojang.blaze3d.pipeline.RenderTarget", "" },
		{ "com.mojang.blaze3d.pipeline.MainTarget", "" },
		{ "net.minecraft.client.renderer.LightTexture", "" },
		{ "net.minecraft.client.Minecraft", "render,frame,target,window,pause,focus,camera,level" },
		{ "com.mojang.renderpearl.api.commands.CommandEncoder", "" },
		{ "com.mojang.renderpearl.api.device.GpuDevice", "" },
		{ "com.mojang.blaze3d.systems.RenderSystem", "device,encoder,clear,target,viewport,scissor" },
		{ "net.minecraft.client.renderer.FirstPersonHandsAndItemsRenderer", "render,hand,item,submit" },
		{ "net.minecraft.client.gui.render.GuiRenderer", "render,clear,target,depth,submit" },
	};

	private ApiDump() {
	}

	public static String run() {
		Path out = Path.of("cybercraft-api-dump.txt");
		List<String> lines = new ArrayList<>();
		lines.add("CyberCraft API dump");
		lines.add("Java " + System.getProperty("java.version"));
		for (String[] target : TARGETS) {
			try {
				dump(target[0], target[1], lines);
			} catch (Throwable t) {
				lines.add("");
				lines.add("!! " + target[0] + ": " + t);
			}
		}
		try {
			Files.write(out, lines);
			CyberCraft.LOG.info("CyberCraft: wrote {} lines to {}", lines.size(), out.toAbsolutePath());
			return "CyberCraft: wrote " + out.toAbsolutePath();
		} catch (IOException e) {
			CyberCraft.LOG.error("CyberCraft: could not write the API dump", e);
			return "CyberCraft: could not write the dump (see the console)";
		}
	}

	private static void dump(String className, String filter, List<String> lines) throws ClassNotFoundException {
		lines.add("");
		Class<?> cls;
		try {
			cls = Class.forName(className, false, ApiDump.class.getClassLoader());
		} catch (ClassNotFoundException e) {
			lines.add("NOT FOUND: " + className);
			return;
		}
		List<String> words = filter.isEmpty() ? List.of() : Arrays.asList(filter.split(","));
		StringBuilder header = new StringBuilder("== ").append(cls.getName());
		if (cls.getSuperclass() != null && cls.getSuperclass() != Object.class) {
			header.append(" extends ").append(shorten(cls.getSuperclass().getName()));
		}
		lines.add(header.toString());

		for (Field f : cls.getDeclaredFields()) {
			if (matches(f.getName(), words)) {
				lines.add("  field  " + modifiers(f.getModifiers()) + shorten(f.getType().getTypeName()) + " " + f.getName());
			}
		}
		Method[] methods = cls.getDeclaredMethods();
		Arrays.sort(methods, (a, b) -> a.getName().compareTo(b.getName()));
		for (Method m : methods) {
			if (m.isSynthetic() || !matches(m.getName(), words)) {
				continue;
			}
			String params = Arrays.stream(m.getParameterTypes()).map(p -> shorten(p.getTypeName())).collect(Collectors.joining(", "));
			lines.add("  method " + modifiers(m.getModifiers()) + shorten(m.getReturnType().getTypeName()) + " " + m.getName() + "(" + params + ")");
		}
	}

	private static boolean matches(String name, List<String> words) {
		if (words.isEmpty()) {
			return true;
		}
		String lower = name.toLowerCase(Locale.ROOT);
		for (String w : words) {
			if (lower.contains(w)) {
				return true;
			}
		}
		return false;
	}

	private static String modifiers(int m) {
		return (Modifier.isPrivate(m) ? "private " : Modifier.isPublic(m) ? "public " : Modifier.isProtected(m) ? "protected " : "")
			+ (Modifier.isStatic(m) ? "static " : "");
	}

	private static String shorten(String name) {
		return name.replace("net.minecraft.client.renderer.", "~r.").replace("net.minecraft.client.", "~c.").replace("net.minecraft.", "~m.")
			.replace("com.mojang.blaze3d.", "~b.").replace("com.mojang.renderpearl.", "~p.").replace("java.lang.", "");
	}
}
