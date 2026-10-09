package dev.cybercraft.client;

import com.mojang.brigadier.arguments.DoubleArgumentType;
import com.mojang.brigadier.arguments.BoolArgumentType;
import com.mojang.brigadier.arguments.IntegerArgumentType;
import com.mojang.brigadier.arguments.StringArgumentType;
import dev.cybercraft.CyberCraft;
import dev.cybercraft.link.CyberLink;
import dev.cybercraft.link.Proto;
import dev.cybercraft.world.GroundCollision;
import dev.cybercraft.world.TerrainPick;
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
			// /ccworld  turns drawing Minecraft's blocks over Cyberpunk off and on.
			dispatcher.register(ClientCommands.literal("ccworld").executes(c -> {
				worldVisible = !worldVisible;
				c.getSource().sendFeedback(Component.literal(worldVisible
					? "CyberCraft: Minecraft's blocks are drawn over Cyberpunk."
					: "CyberCraft: Minecraft's blocks are hidden (only the HUD is drawn)."));
				return 1;
			}));
			// /cccamsrc                          shows which camera the plugin publishes.
			// /cccamsrc transform|data|projected  chooses it. "projected" follows camera shake and hit reactions.
			dispatcher.register(ClientCommands.literal("cccamsrc")
				.executes(c -> {
					c.getSource().sendFeedback(Component.literal("CyberCraft: camera source is " + new String[] { "transform", "data", "projected", "projectedpos" }[camSource]));
					return 1;
				})
				.then(ClientCommands.literal("transform").executes(c -> {
					camSource = 0;
					c.getSource().sendFeedback(Component.literal("CyberCraft: camera source: transform"));
					return 1;
				}))
				.then(ClientCommands.literal("data").executes(c -> {
					camSource = 1;
					c.getSource().sendFeedback(Component.literal("CyberCraft: camera source: data"));
					return 1;
				}))
				.then(ClientCommands.literal("projected").executes(c -> {
					camSource = 2;
					c.getSource().sendFeedback(Component.literal("CyberCraft: camera source: projected"));
					return 1;
				}))
				.then(ClientCommands.literal("projectedpos").executes(c -> {
					camSource = 3;
					c.getSource().sendFeedback(Component.literal("CyberCraft: camera source: projected, with the position solved too"));
					return 1;
				})));
			// /ccroll  on|off|flip  applies the camera's sideways tilt (when it is knocked about); flip reverses its direction.
			dispatcher.register(ClientCommands.literal("ccroll")
				.executes(c -> {
					c.getSource().sendFeedback(Component.literal("CyberCraft: camera tilt is " + (rollMode == 0 ? "off" : rollMode > 0 ? "on" : "on, reversed")));
					return 1;
				})
				.then(ClientCommands.literal("on").executes(c -> {
					rollMode = 1;
					c.getSource().sendFeedback(Component.literal("CyberCraft: camera tilt on"));
					return 1;
				}))
				.then(ClientCommands.literal("off").executes(c -> {
					rollMode = 0;
					c.getSource().sendFeedback(Component.literal("CyberCraft: camera tilt off"));
					return 1;
				}))
				.then(ClientCommands.literal("flip").executes(c -> {
					rollMode = -1;
					c.getSource().sendFeedback(Component.literal("CyberCraft: camera tilt on, reversed"));
					return 1;
				})));
			// /ccpredict        shows how many milliseconds ahead of the camera's position Minecraft draws.
			// /ccpredict <ms>   sets it (0 = off). Raise it if the blocks slide the way you move, lower it if they slide against.
			dispatcher.register(ClientCommands.literal("ccpredict")
				.executes(c -> {
					c.getSource().sendFeedback(Component.literal(String.format("CyberCraft: drawing the camera %.0f ms ahead.", predictMs)));
					return 1;
				})
				.then(ClientCommands.argument("ms", DoubleArgumentType.doubleArg(0, 200)).executes(c -> {
					predictMs = DoubleArgumentType.getDouble(c, "ms");
					c.getSource().sendFeedback(Component.literal(String.format("CyberCraft: drawing the camera %.0f ms ahead.", predictMs)));
					return 1;
				})));
			// /ccfov           shows the field of view in use.
			// /ccfov <degrees> uses this vertical field of view instead of the game's (to fine-tune the alignment).
			// /ccfov auto      goes back to the game's.
			dispatcher.register(ClientCommands.literal("ccfov")
				.executes(c -> {
					c.getSource().sendFeedback(Component.literal(String.format(
						"CyberCraft: vertical field of view in use %.2f (the game's: %.2f%s), screen shape %.3f. Camera %s.",
						cameraFov(), CAMERA.vfov, fovOverride > 0 ? ", overridden" : "", CAMERA.aspect, cameraValid ? "linked" : "not available")));
					return 1;
				})
				.then(ClientCommands.literal("auto").executes(c -> {
					fovOverride = 0;
					c.getSource().sendFeedback(Component.literal("CyberCraft: using the game's field of view."));
					return 1;
				}))
				.then(ClientCommands.argument("degrees", DoubleArgumentType.doubleArg(10, 150)).executes(c -> {
					fovOverride = DoubleArgumentType.getDouble(c, "degrees");
					c.getSource().sendFeedback(Component.literal(String.format("CyberCraft: vertical field of view set to %.2f.", fovOverride)));
					return 1;
				})));
			// /ccdepthprobe  turns the depth-buffer probe on or off (it writes what it sees to Cyberpunk's CyberCraft.log).
			dispatcher.register(ClientCommands.literal("ccdepthprobe").executes(c -> {
				depthProbe = !depthProbe;
				c.getSource().sendFeedback(Component.literal(depthProbe
					? "CyberCraft: depth probe on. Look at Cyberpunk's CyberCraft.log in about 10 seconds; type /ccdepthprobe again to switch it off."
					: "CyberCraft: depth probe off."));
				return 1;
			}));
			// /ccdepthdebug  cycles a debug view of the depth test: 1 the game's depth, 2 the blocks' distance, 3 hidden (red) / shown (green), 0 off.
			dispatcher.register(ClientCommands.literal("ccdepthdebug").executes(c -> {
				depthDebug = (depthDebug + 1) % 4;
				String[] names = { "off", "the game's depth over the whole screen (grey: near dark, far light)", "the blocks' distance (grey)", "blocks hidden by the game's world in red, shown in green" };
				c.getSource().sendFeedback(Component.literal("CyberCraft: depth debug view " + depthDebug + ": " + names[depthDebug]));
				return 1;
			}));
			// /ccwarp  turns the re-aiming of the blocks at the game's current camera off and on (it hides the time the picture takes to arrive).
			dispatcher.register(ClientCommands.literal("ccwarp").executes(c -> {
				warp = !warp;
				c.getSource().sendFeedback(Component.literal(warp
					? "CyberCraft: blocks are re-aimed at the game's camera as it is when they are drawn."
					: "CyberCraft: blocks are drawn as they were rendered (no re-aiming)."));
				return 1;
			}));
			// /ccdelay          shows how far behind the newest camera the blocks are aimed (the game's own picture is a little behind its newest camera).
			// /ccdelay <ms>     sets it. If the blocks swing AHEAD of the world when you turn, raise it; if they trail behind, lower it (0 = the newest camera).
			dispatcher.register(ClientCommands.literal("ccdelay")
				.executes(c -> {
					c.getSource().sendFeedback(Component.literal(String.format("CyberCraft: aiming the blocks at the camera %.0f ms behind the newest.", warpDelayMs)));
					return 1;
				})
				.then(ClientCommands.argument("ms", DoubleArgumentType.doubleArg(0, 150)).executes(c -> {
					warpDelayMs = DoubleArgumentType.getDouble(c, "ms");
					c.getSource().sendFeedback(Component.literal(String.format("CyberCraft: aiming the blocks at the camera %.0f ms behind the newest.", warpDelayMs)));
					return 1;
				})));
			// /cccollide on|off|status|rebuild  collision for what you build: the blocks around you become invisible collision boxes in Night City, so cars and
			// people are stopped by your builds. Rebuilt from the world each time (nothing is saved), and gone when you switch worlds or close Minecraft.
			dispatcher.register(ClientCommands.literal("cccollide")
				.executes(c -> {
					c.getSource().sendFeedback(Component.literal("CyberCraft: block collision is " + BlockColliders.status() + ". /cccollide on|off|rebuild"));
					return 1;
				})
				.then(ClientCommands.literal("on").executes(c -> {
					if (!CyberLink.active()) {
						c.getSource().sendFeedback(Component.literal("CyberCraft: Cyberpunk isn't linked. Start the game with the plugin installed."));
						return 1;
					}
					BlockColliders.setEnabled(true);
					c.getSource().sendFeedback(Component.literal("CyberCraft: block collision on. What you build near you becomes solid for cars and people in Night City."));
					return 1;
				}))
				.then(ClientCommands.literal("off").executes(c -> {
					BlockColliders.setEnabled(false);
					c.getSource().sendFeedback(Component.literal("CyberCraft: block collision off. The boxes are being taken down."));
					return 1;
				}))
				.then(ClientCommands.literal("status").executes(c -> {
					c.getSource().sendFeedback(Component.literal("CyberCraft: block collision is " + BlockColliders.status()));
					return 1;
				}))
				.then(ClientCommands.literal("rebuild").executes(c -> {
					BlockColliders.rebuild();
					c.getSource().sendFeedback(Component.literal("CyberCraft: rebuilding every collision box."));
					return 1;
				})));
			// /ccdebug log on|off | dump | find <word>   tools for working on the mod (they write to Cyberpunk's CyberCraft.log):
			//   log: detailed statistics every few seconds (camera accuracy, depth capture, ground scan, input counts); the log is quiet without it
			//   dump: the classes behind the collision boxes, to check them after a game update
			//   uilayer <0-3>: draw the game's interface (HUD) back over the blocks so that it is in front of them (3: show the captured layer alone, to check it)
			//   ui [from]: record four frames of the game's drawing and summarise the last whole one, to find where its interface starts (for drawing blocks behind it);
			//       from = the number of the first command list to print in full
			//   find: every class, enum and global function in the game whose name contains the word (the first six letters count)
			dispatcher.register(ClientCommands.literal("ccdebug")
				.then(ClientCommands.literal("log")
					.then(ClientCommands.literal("on").executes(c -> {
						c.getSource().sendFeedback(Component.literal(sendDebug(Proto.DEBUG_LOG, 1, "Detailed logging on: statistics every few seconds in CyberCraft.log.")));
						return 1;
					}))
					.then(ClientCommands.literal("off").executes(c -> {
						c.getSource().sendFeedback(Component.literal(sendDebug(Proto.DEBUG_LOG, 0, "Detailed logging off.")));
						return 1;
					})))
				.then(ClientCommands.literal("ui")
					.executes(c -> {
						c.getSource().sendFeedback(Component.literal(sendDebug(Proto.DEBUG_UI, 0, "Capturing the next frames' drawing: the summary is in CyberCraft.log (lines starting with ui:).")));
						return 1;
					})
					.then(ClientCommands.argument("from", IntegerArgumentType.integer(0, 500)).executes(c -> {
						int from = IntegerArgumentType.getInteger(c, "from");
						c.getSource().sendFeedback(Component.literal(sendDebug(Proto.DEBUG_UI, from + 1, "Capturing the next frames' drawing; the command lists from number " + from + " on will be printed in full in CyberCraft.log (lines starting with ui:).")));
						return 1;
					})))
				// /ccdebug uilayer <0-3>: the game's own interface (health bar, minimap, quest text) drawn back over Minecraft's blocks, so it is in front of them:
				// 0 off, 1 on (the captured layer is premultiplied alpha), 2 on (straight alpha), 3 show the captured layer alone (transparent parts magenta)
				.then(ClientCommands.literal("uilayer").then(ClientCommands.argument("mode", IntegerArgumentType.integer(0, 4)).executes(c -> {
					int mode = IntegerArgumentType.getInteger(c, "mode");
					c.getSource().sendFeedback(Component.literal(sendDebug(Proto.DEBUG_UI_LAYER, mode, "The game's interface over the blocks: mode " + mode + (mode == 3 ? " (showing the captured layer alone)." : mode == 4 ? " (a magenta shadow of the layer over everything: use /ccdebug uiscale and uishift to line it up with the real interface)." : mode == 0 ? " (off)." : "."))));
					return 1;
				})))
				// /ccdebug uiscale <across%> [down%] and /ccdebug uishift <x> <y>: where the game's interface layer is laid over the blocks (the game stretches it about the middle of the screen,
				// by different amounts across and down: with one number both are the same).
				.then(ClientCommands.literal("uiscale").then(ClientCommands.argument("across", IntegerArgumentType.integer(50, 200)).executes(c -> {
					int across = IntegerArgumentType.getInteger(c, "across");
					c.getSource().sendFeedback(Component.literal(sendDebug(Proto.DEBUG_UI_SCALE, across, "The interface layer is stretched to " + across + "% across and " + across + "% down about the middle of the screen.")));
					return 1;
				}).then(ClientCommands.argument("down", IntegerArgumentType.integer(50, 200)).executes(c -> {
					int across = IntegerArgumentType.getInteger(c, "across"), down = IntegerArgumentType.getInteger(c, "down");
					if (!CyberLink.active() || CyberLink.sendDebug(Proto.DEBUG_UI_SCALE, across, down) == 0) {
						c.getSource().sendFeedback(Component.literal("CyberCraft: Cyberpunk isn't linked."));
					} else {
						c.getSource().sendFeedback(Component.literal("CyberCraft: the interface layer is stretched to " + across + "% across and " + down + "% down about the middle of the screen."));
					}
					return 1;
				}))))
				.then(ClientCommands.literal("uishift").then(ClientCommands.argument("x", IntegerArgumentType.integer(-400, 400)).then(ClientCommands.argument("y", IntegerArgumentType.integer(-400, 400)).executes(c -> {
					int x = IntegerArgumentType.getInteger(c, "x"), y = IntegerArgumentType.getInteger(c, "y");
					if (!CyberLink.active() || CyberLink.sendDebug(Proto.DEBUG_UI_SHIFT, x, y) == 0) {
						c.getSource().sendFeedback(Component.literal("CyberCraft: Cyberpunk isn't linked."));
					} else {
						c.getSource().sendFeedback(Component.literal("CyberCraft: the interface layer is shifted by (" + x + ", " + y + ") pixels."));
					}
					return 1;
				}))))
				// /ccdebug scene <0-3>: proof of concept for drawing blocks into the game's own HDR scene: a dim (1), bright (2) or very bright (3) square in the middle of the screen is
				// drawn into the scene just before the game's post-processing, so bloom, tone mapping and the game's interface apply to it. 0 off. (Needs DLSS off.)
				.then(ClientCommands.literal("scene").then(ClientCommands.argument("mode", IntegerArgumentType.integer(0, 4)).executes(c -> {
					int mode = IntegerArgumentType.getInteger(c, "mode");
					c.getSource().sendFeedback(Component.literal(sendDebug(Proto.DEBUG_SCENE, mode, mode == 0 ? "Drawing into the game's scene: off." : mode == 4 ? "Drawing Minecraft's blocks into the game's scene (they now get bloom, tone mapping, colour grading, and the game's interface over them)." : "Drawing a square into the game's scene (mode " + mode + "): look for it in the middle of the screen; see CyberCraft.log (lines starting with inscene:).")));
					return 1;
				})))
				// /ccdebug scenegain <percent>: how bright the blocks are in the game's scene (100: a Minecraft white is 1.0 in the scene's units; try 50 to 300).
				.then(ClientCommands.literal("scenegain").then(ClientCommands.argument("percent", IntegerArgumentType.integer(5, 2000)).executes(c -> {
					int percent = IntegerArgumentType.getInteger(c, "percent");
					c.getSource().sendFeedback(Component.literal(sendDebug(Proto.DEBUG_SCENE_GAIN, percent, "The blocks drawn into the game's scene are " + percent + "% as bright as before.")));
					return 1;
				})))
				// /ccdebug sceneao <percent> [radiusCm]: ambient occlusion where blocks meet the game's world (the foot of a wall, a corner, where a block sinks into the road), found from the game's depth;
				// 0 is off (the default), try 60 to 100; the radius is how far away something still counts, 60 cm by default.
				.then(ClientCommands.literal("sceneao").then(ClientCommands.argument("percent", IntegerArgumentType.integer(0, 100)).executes(c -> {
					int percent = IntegerArgumentType.getInteger(c, "percent");
					c.getSource().sendFeedback(Component.literal(sendDebug(Proto.DEBUG_SCENE_AO, percent, percent == 0 ? "Ambient occlusion on the blocks: off." : "Ambient occlusion on the blocks: " + percent + "%.")));
					return 1;
				}).then(ClientCommands.argument("radiusCm", IntegerArgumentType.integer(10, 300)).executes(c -> {
					int percent = IntegerArgumentType.getInteger(c, "percent"), radius = IntegerArgumentType.getInteger(c, "radiusCm");
					if (!CyberLink.active() || CyberLink.sendDebug(Proto.DEBUG_SCENE_AO, percent, radius) == 0) {
						c.getSource().sendFeedback(Component.literal("CyberCraft: Cyberpunk isn't linked."));
					} else {
						c.getSource().sendFeedback(Component.literal("CyberCraft: ambient occlusion on the blocks: " + percent + "%, radius " + radius + " cm."));
					}
					return 1;
				}))))
				// /ccdebug sceneterrainao <percent>: the shadow a block makes on the road and walls next to it (the game's own surfaces darkened where blocks are close), 0 is off; default 60.
				.then(ClientCommands.literal("sceneterrainao").then(ClientCommands.argument("percent", IntegerArgumentType.integer(0, 100)).executes(c -> {
					int percent = IntegerArgumentType.getInteger(c, "percent");
					c.getSource().sendFeedback(Component.literal(sendDebug(Proto.DEBUG_SCENE_TERRAIN_AO, percent, percent == 0 ? "Occlusion on the game's surfaces next to blocks: off." : "Occlusion on the game's surfaces next to blocks: " + percent + "%.")));
					return 1;
				})))
				// /ccdebug sceneaoview <0|1>: show only the ambient occlusion term (white where there is none, dark where there is a lot), to see what it does.
				.then(ClientCommands.literal("sceneaoview").then(ClientCommands.argument("on", IntegerArgumentType.integer(0, 1)).executes(c -> {
					int on = IntegerArgumentType.getInteger(c, "on");
					c.getSource().sendFeedback(Component.literal(sendDebug(Proto.DEBUG_SCENE_AO_VIEW, on, on == 1 ? "Showing only the ambient occlusion on the blocks (white: none, dark: a lot)." : "Ambient occlusion view off.")));
					return 1;
				})))
				// /ccdebug sceneglow <percent>: how much the brightest pixels (lit whites, glowstone, torch flames) are boosted in the game's scene so that they glow with the game's bloom (0 off; default 300: up to 4 times).
				.then(ClientCommands.literal("sceneglow").then(ClientCommands.argument("percent", IntegerArgumentType.integer(0, 3000)).executes(c -> {
					int percent = IntegerArgumentType.getInteger(c, "percent");
					c.getSource().sendFeedback(Component.literal(sendDebug(Proto.DEBUG_SCENE_GLOW, percent, "The brightest pixels of the blocks in the game's scene are boosted by up to " + percent + "%.")));
					return 1;
				})))
				// /ccdebug scenedelay <ms>: how far behind the newest published camera the blocks are aimed when drawn into the scene (if they swim when you turn, try 0 to 30).
				.then(ClientCommands.literal("scenedelay").then(ClientCommands.argument("ms", IntegerArgumentType.integer(0, 150)).executes(c -> {
					int ms = IntegerArgumentType.getInteger(c, "ms");
					c.getSource().sendFeedback(Component.literal(sendDebug(Proto.DEBUG_SCENE_DELAY, ms, "The blocks drawn into the game's scene are aimed " + ms + " ms behind the newest camera.")));
					return 1;
				})))
				.then(ClientCommands.literal("dump").executes(c -> {
					c.getSource().sendFeedback(Component.literal(sendDebug(Proto.DEBUG_DUMP, 0, "Writing the classes behind the collision boxes to CyberCraft.log.")));
					return 1;
				}))
				// /ccdebug class <word>: the methods and fields of the game's classes with <word> (up to six letters) in their name; the shortest names first, at most four classes.
				.then(ClientCommands.literal("class").then(ClientCommands.argument("word", StringArgumentType.word()).executes(c -> {
					String word = StringArgumentType.getString(c, "word").toLowerCase();
					long packed = 0;
					for (int i = 0; i < Math.min(6, word.length()); i++) {
						packed |= ((long) (word.charAt(i) & 0x7F)) << (8 * i);
					}
					c.getSource().sendFeedback(Component.literal(sendDebug(Proto.DEBUG_CLASS, (double) packed, "Describing the game's classes with \"" + word.substring(0, Math.min(6, word.length())) + "\" in their name: the result is in CyberCraft.log (lines starting with debug:).")));
					return 1;
				})))
				.then(ClientCommands.literal("find").then(ClientCommands.argument("word", StringArgumentType.word()).executes(c -> {
					String word = StringArgumentType.getString(c, "word").toLowerCase();
					long packed = 0;
					for (int i = 0; i < Math.min(6, word.length()); i++) {
						packed |= ((long) (word.charAt(i) & 0x7F)) << (8 * i);
					}
					c.getSource().sendFeedback(Component.literal(sendDebug(Proto.DEBUG_FIND, (double) packed, "Searching the game for \"" + word.substring(0, Math.min(6, word.length())) + "\": the result is in CyberCraft.log.")));
					return 1;
				}))));
			// /ccsync time <true|false> and /ccsync weather <true|false>: Night City's time of day and weather follow Minecraft's (so /time set day and /weather rain change the city).
			dispatcher.register(ClientCommands.literal("ccsync")
				.then(ClientCommands.literal("time").then(ClientCommands.argument("on", BoolArgumentType.bool()).executes(c -> {
					WorldSync.syncTime = BoolArgumentType.getBool(c, "on");
					c.getSource().sendFeedback(Component.literal(WorldSync.syncTime
						? "CyberCraft: Night City's time of day now follows Minecraft's (use /time set ...)."
						: "CyberCraft: Night City keeps its own time of day."));
					return 1;
				})))
				.then(ClientCommands.literal("weather").then(ClientCommands.argument("on", BoolArgumentType.bool()).executes(c -> {
					WorldSync.syncWeather = BoolArgumentType.getBool(c, "on");
					c.getSource().sendFeedback(Component.literal(WorldSync.syncWeather
						? "CyberCraft: Night City's weather now follows Minecraft's (use /weather clear|rain|thunder)."
						: "CyberCraft: Night City keeps its own weather."));
					return 1;
				}))));
			// /ccterrain  turns aiming at Night City itself (to build on its streets and walls) off and on.
			dispatcher.register(ClientCommands.literal("ccterrain").executes(c -> {
				TerrainPick.setEnabled(!TerrainPick.enabled());
				c.getSource().sendFeedback(Component.literal(TerrainPick.enabled()
					? "CyberCraft: you can aim at, and build on, Night City's streets and walls."
					: "CyberCraft: Night City can't be aimed at (only blocks)."));
				return 1;
			}));
			// /ccocclude  hides Minecraft's blocks behind the game's world (uses the game's depth). On by default; off = blocks are drawn over everything.
			dispatcher.register(ClientCommands.literal("ccocclude").executes(c -> {
				occlude = !occlude;
				c.getSource().sendFeedback(Component.literal(occlude
					? "CyberCraft: blocks are hidden behind the game's world."
					: "CyberCraft: blocks are drawn over everything (no depth)."));
				return 1;
			}));
			// /ccdepthcapture  turns the depth capture on or off: it copies the game's main depth texture once a frame and compares a few of its
			// texels with the game's own rays (look at Cyberpunk's CyberCraft.log). Stand still, looking at a street or a wall.
			dispatcher.register(ClientCommands.literal("ccdepthcapture").executes(c -> {
				depthCapture = !depthCapture;
				c.getSource().sendFeedback(Component.literal(depthCapture
					? "CyberCraft: depth capture on. Stand still, looking at a street or a wall, and look at Cyberpunk's CyberCraft.log in about 15 seconds; type /ccdepthcapture again to switch it off."
					: "CyberCraft: depth capture off."));
				return 1;
			}));
			// /ccalign  lines up the street where you stand with Minecraft's whole-number heights, so blocks built there sit exactly on it.
			dispatcher.register(ClientCommands.literal("ccalign").executes(c -> {
				c.getSource().sendFeedback(Component.literal(requestAlign()));
				return 1;
			}));
			// /ccdump  writes the names of Minecraft's camera and rendering methods to a file (groundwork for drawing blocks).
			dispatcher.register(ClientCommands.literal("ccdump").executes(c -> {
				c.getSource().sendFeedback(Component.literal(ApiDump.run()));
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

	private static String sendDebug(int action, double arg, String message) {
		if (!CyberLink.active()) {
			return "CyberCraft: Cyberpunk isn't linked. Start the game with the plugin installed.";
		}
		if (CyberLink.sendDebug(action, arg) == 0) {
			return "CyberCraft: couldn't send the command.";
		}
		return "CyberCraft: " + message;
	}

	/** Asks Cyberpunk to line the street under V up with a whole-number height. */
	private static String requestAlign() {
		if (!CyberLink.active()) {
			return "CyberCraft: Cyberpunk isn't linked. Start the game with the plugin installed.";
		}
		if (!CyberLink.readGameState(STATE) || !STATE.inGame()) {
			return "CyberCraft: Cyberpunk is linked, but no save is loaded.";
		}
		int seq = CyberLink.sendAlignGround();
		if (seq == 0) {
			return "CyberCraft: couldn't send the command.";
		}
		pendingSeq = seq;
		alignRequested = true;
		return "CyberCraft: lining the street up with Minecraft's heights. Stand where you want to build, on level ground; blocks you already built here will move up or down by under half a block.";
	}

	private static boolean alignRequested;

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

	// The game's camera, read at the start of every Minecraft frame. While drawWorld() is true, Minecraft's own camera
	// is replaced by it (see CameraMixin) and the world is drawn, without sky or clouds, on a transparent background.
	private static final CyberLink.CameraState CAMERA = new CyberLink.CameraState();
	private static volatile boolean cameraValid;
	private static volatile boolean worldVisible = true; // /ccworld
	private static volatile double fovOverride; // /ccfov <degrees>; 0 = use what the game reports
	// The picture takes a few tens of milliseconds to get from Minecraft to the screen (it is drawn, copied back from
	// the graphics card, shared, then drawn again by Cyberpunk), so blocks drawn from the camera's position at the start
	// of that trip end up slightly behind when the camera is moving. Minecraft draws the camera's position that far ahead,
	// using how fast the Minecraft player is moving (known exactly, unlike a speed worked out from the camera, which is
	// noisy). Only the position is moved ahead, never the direction. /ccpredict tunes it.
	private static volatile double predictMs = 0;
	private static volatile double playerVelX, playerVelY, playerVelZ; // blocks per second
	private static double lastPlayerX, lastPlayerY, lastPlayerZ;
	private static boolean havePlayerSample;
	// Which camera the plugin publishes (0 transform, 1 camera data, 2 projected, 3 projected with the position solved too) and whether the camera's tilt is applied (0 off, 1 on, -1 reversed).
	private static volatile int camSource = 0;
	private static volatile int rollMode = 1;
	private static volatile boolean depthProbe;
	private static volatile boolean depthCapture;
	private static volatile int depthDebug;
	private static volatile boolean warp = true; // /ccwarp
	private static volatile double warpDelayMs = 10; // /ccdelay
	private static boolean activeBefore;
	private static boolean savedBobView;
	private static Object savedDamageTilt;

	/** Is Minecraft drawing its blocks (and entities) over Cyberpunk, through Cyberpunk's camera? */
	public static boolean drawWorld() {
		return overlayActive() && worldVisible && cameraValid;
	}

	/** The camera Minecraft should use: x, y, z, yaw, pitch (valid while {@link #drawWorld()} is true). */
	/** The plugin's counter for the camera Minecraft read at the start of this frame. */
	public static long cameraFrame() {
		return CAMERA.frame;
	}

	// What Minecraft actually renders through, noted by LevelRendererMixin, and a once-a-second comparison with what was asked for.
	private static volatile double usedX, usedY, usedZ;
	private static volatile float usedYaw, usedPitch, usedProjX, usedProjY;
	private static volatile boolean usedValid;
	private static long reportAt;
	private static int framesSinceReport;

	private static volatile float usedM22, usedM32;
	private static volatile boolean occlude = true; // /ccocclude

	/** Is Minecraft's world being drawn over Cyberpunk with depth, so that the game's world can hide it? */
	public static boolean occludeActive() {
		return occlude && drawWorld() && !FrameExporter.depthUnsupported();
	}

	public static boolean occludeEnabled() {
		return occlude;
	}

	public static float projectionM22() {
		return usedM22;
	}

	public static float projectionM32() {
		return usedM32;
	}

	public static void noteRenderCamera(double x, double y, double z, float yaw, float pitch, float projX, float projY, float m22, float m32) {
		usedM22 = m22;
		usedM32 = m32;
		usedX = x;
		usedY = y;
		usedZ = z;
		usedYaw = yaw;
		usedPitch = pitch;
		usedProjX = projX;
		usedProjY = projY;
		usedValid = true;
	}

	private static void reportCamera(Minecraft client) {
		framesSinceReport++;
		long now = System.currentTimeMillis();
		if (now < reportAt) {
			return;
		}
		double seconds = reportAt == 0 ? 1.0 : 1.0 + (now - reportAt) / 1000.0;
		reportAt = now + 3000;
		if (!drawWorld() || !usedValid) {
			framesSinceReport = 0;
			return;
		}
		double[] asked = cameraPose();
		double yawDiff = usedYaw - asked[3];
		while (yawDiff > 180.0) yawDiff -= 360.0;
		while (yawDiff < -180.0) yawDiff += 360.0;
		double usedVfov = Math.toDegrees(2.0 * Math.atan(1.0 / usedProjY));
		double usedAspect = usedProjY / usedProjX;
		var target = client.gameRenderer.mainRenderTarget();
		CyberCraft.LOG.info(String.format(
			"CyberCraft: world drawing: %.0f frames a second; camera used vs asked for: position off by (%.3f, %.3f, %.3f) blocks, yaw off by %.3f deg, pitch off by %.3f deg; "
				+ "vertical field of view used %.2f (asked %.2f), picture shape used %.3f (the game's %.3f, Minecraft's window %dx%d = %.3f)",
			framesSinceReport / Math.max(0.5, 3.0), usedX - asked[0], usedY - asked[1], usedZ - asked[2], yawDiff, usedPitch - asked[4],
			usedVfov, cameraFov(), usedAspect, CAMERA.aspect, target.width, target.height, (double) target.width / Math.max(1, target.height)));
		// Where does the Minecraft player look, compared with where the picture's camera looks? (What you break is decided from the
		// player's view unless EntityPickMixin takes over; this line shows how far apart the two are.)
		LocalPlayer self = client.player;
		if (self != null) {
			double pitchDiff = asked[4] - self.getXRot();
			double yawDiffToPlayer = asked[3] - self.getYRot();
			while (yawDiffToPlayer > 180.0) yawDiffToPlayer -= 360.0;
			while (yawDiffToPlayer < -180.0) yawDiffToPlayer += 360.0;
			double horizontal = Math.hypot(asked[0] - self.getX(), asked[2] - self.getZ());
			CyberCraft.LOG.info(String.format(
				"CyberCraft: aim: the picture's camera looks %.2f deg lower (pitch %.2f vs the player's %.2f) and %.2f deg to the side than the Minecraft player; its eye is %.3f blocks higher and %.3f blocks away horizontally",
				pitchDiff, asked[4], (double) self.getXRot(), yawDiffToPlayer, asked[1] - self.getEyeY(), horizontal));
		}
		framesSinceReport = 0;
	}

	public static int rollMode() {
		return rollMode;
	}

	public static double[] cameraPose() {
		double s = predictMs / 1000.0;
		return new double[] { CAMERA.x + playerVelX * s, CAMERA.y + playerVelY * s, CAMERA.z + playerVelZ * s, CAMERA.yaw, CAMERA.pitch, CAMERA.roll };
	}

	/** The vertical field of view Minecraft should use, in degrees. */
	public static double cameraFov() {
		return fovOverride > 0 ? fovOverride : CAMERA.vfov;
	}

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
		boolean active = overlayActive();
		if (active && !activeBefore) {
			// Minecraft's own frame pacing would hold the picture back: no VSync, a high frame limit. (Same as SkyCraft.)
			client.options.enableVsync().set(false);
			client.options.framerateLimit().set(144);
			// Minecraft's walking sway moves the view, but the camera is Cyberpunk's, and V is not walking: it would
			// make the blocks bob. Off while this is on; put back afterwards.
			savedBobView = client.options.bobView().get();
			client.options.bobView().set(false);
			// The same for the shake Minecraft gives the view when its player is hurt.
			savedDamageTilt = OptionTweaks.get(client.options, "damageTiltStrength");
			if (savedDamageTilt != null) {
				OptionTweaks.set(client.options, "damageTiltStrength", 0.0);
			}
		} else if (!active && activeBefore) {
			client.options.bobView().set(savedBobView);
			if (savedDamageTilt != null) {
				OptionTweaks.set(client.options, "damageTiltStrength", savedDamageTilt);
			}
		}
		activeBefore = active;
		if (!CyberLink.active() || !CyberLink.readGameState(STATE)) {
			cameraValid = false;
			if (routedFlag) {
				routedFlag = false;
				InputBridge.releaseAll();
			}
			return;
		}
		cameraValid = CyberLink.readCamera(CAMERA) && CAMERA.valid;
		reportCamera(client);
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
		BlockColliders.tick(client);

		// How fast the Minecraft player is moving, from where it was a tick ago (one tick = 1/20 s). A jump of more than
		// a few blocks in one tick is a teleport, not walking: it doesn't count.
		LocalPlayer mover = client.player;
		if (mover != null) {
			double px = mover.getX();
			double py = mover.getY();
			double pz = mover.getZ();
			if (havePlayerSample) {
				double vx = (px - lastPlayerX) * 20.0;
				double vy = (py - lastPlayerY) * 20.0;
				double vz = (pz - lastPlayerZ) * 20.0;
				double speed = Math.sqrt(vx * vx + vy * vy + vz * vz);
				if (speed > 25.0) {
					vx = vy = vz = 0.0;
				}
				playerVelX = vx;
				playerVelY = vy;
				playerVelZ = vz;
			}
			lastPlayerX = px;
			lastPlayerY = py;
			lastPlayerZ = pz;
			havePlayerSample = true;
		} else {
			havePlayerSample = false;
			playerVelX = playerVelY = playerVelZ = 0.0;
		}

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
			CyberLink.publishMcState(false, false, false, camSource, depthProbe, depthCapture || occludeActive(), depthDebug, !warp, (float) warpDelayMs, sensitivity, 0, 0, 0, 0, 0);
		} else if (following) {
			// The ground Minecraft walks on is Night City's own, so the player's position is V's position.
			CyberLink.publishMcState(true, true, screenOpen, camSource, depthProbe, depthCapture || occludeActive(), depthDebug, !warp, (float) warpDelayMs, sensitivity, player.getX(), player.getY(), player.getZ(), player.getYRot(), player.getXRot());
		} else {
			CyberLink.publishMcState(true, false, screenOpen, camSource, depthProbe, depthCapture || occludeActive(), depthDebug, !warp, (float) warpDelayMs, sensitivity, 0, 0, 0, player.getYRot(), player.getXRot());
		}

		// Minecraft's time of day and weather: published every tick, and Cyberpunk sets the game's from them when the player has turned the sync on (/ccsync).
		WorldSync.read(client.level);
		CyberLink.publishMcWorld(client.level != null && WorldSync.ok, WorldSync.syncTime, WorldSync.syncWeather, WorldSync.dayTime, WorldSync.rain, WorldSync.thunder);

		if (!haveState) {
			return;
		}

		// Did Cyberpunk deal with our teleport?
		if (pendingSeq != 0 && STATE.cmdAck == pendingSeq) {
			if (alignRequested) {
				alignRequested = false;
				CyberCraft.LOG.info(STATE.cmdResult == Proto.RESULT_OK
					? String.format("CyberCraft: aligned: Minecraft's height is now the game's minus %.3f", STATE.verticalOffset)
					: "CyberCraft: aligning FAILED: see Cyberpunk's CyberCraft.log");
			} else {
				CyberCraft.LOG.info("CyberCraft: command {} {}", pendingSeq, STATE.cmdResult == Proto.RESULT_OK ? "done: Cyberpunk teleported V" : "FAILED: see Cyberpunk's CyberCraft.log");
			}
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
