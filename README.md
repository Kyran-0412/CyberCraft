# CyberCraft

Play Cyberpunk 2077 as a Minecraft player. A work in progress, modelled on
[SkyCraft](https://github.com/chasmlol/SkyCraft) (MIT, by chasmlol), which does the same for Skyrim.

The plan: a RED4ext plugin inside Cyberpunk talks to a hidden Minecraft Fabric mod through shared memory.
Minecraft runs the player physics, inventory and blocks; Cyberpunk draws everything.

Tested against Cyberpunk 2077 **game version 2.31** (Steam), RED4ext and Cyber Engine Tweaks installed.

## Phases

0. **Hello world**: a RED4ext plugin that logs V's position once a second. (done)
1. **Link**: shared memory between the plugin and a Fabric mod. 1a: V's position reaches Minecraft (done). 1b-i: a Minecraft command teleports V (done). 1b-ii: Minecraft's player position drives V (done).
2. **Walk**: Night City collision fed into Minecraft physics. 2a: the ground around V built as blocks from raycasts (done). 2b-i: the ground as smooth invisible collision instead of blocks (done). 2b-ii: walls, trees, lamp posts and other obstacles (current). 2b-iii: bridges and other layers.
3. **Input and display**: 3b: the Minecraft hotbar, hearts, held item and screens drawn over the game (done). 3a: keyboard and mouse forwarded to Minecraft so you play in the Cyberpunk window (current).
4. **Combat**: invisible Minecraft proxies for NPCs, damage both ways.
5. **Blocks**: place and break blocks in Night City, depth-composited.

## Building the plugin (Windows)

Needs Visual Studio with "Desktop development with C++", CMake and Git.

```
cmake -S . -B build -A x64 -DCYBERCRAFT_GAME_DIR="C:\path\to\Cyberpunk 2077"
cmake --build build --config Release
```

`CYBERCRAFT_GAME_DIR` is the folder containing `bin` and `r6`. Each build copies the plugin to
`<game>\red4ext\plugins\CyberCraft\CyberCraft.dll`. Then start the game and check
`<game>\red4ext\logs\CyberCraft.log`.

## Building the Minecraft mod (Windows)

Needs JDK 25.

```
cd fabric
.\gradlew runClient
```

This starts a development Minecraft with the mod. Start Cyberpunk (with the plugin) as well; the Minecraft
console prints "linked to Cyberpunk" and V's position once a second. Minecraft's log is also in
`fabric\run\logs\latest.log`.

### Trying the teleport (Phase 1b-i)

Start Cyberpunk and the development Minecraft, load a save in Cyberpunk, and in Minecraft open any world.
Press `T` and type `/cctp` (V moves 5 m up), or `/cctp <east> <north> <up>` for an offset in metres.
The result is in Minecraft's console and in `red4ext\logs\CyberCraft.log`.

### Drawing Minecraft's blocks over Cyberpunk (Phase 5a)

While `/ccfollow` is on, the plugin publishes the game's camera (position, direction and a measured field of view)
and Minecraft draws its blocks, entities and hand through that camera, without sky or clouds, on a transparent
background; the result is drawn over the game together with the HUD. This version does not hide blocks behind
buildings yet (no depth test), and blocks are lit by Minecraft's own light.

* Use a **Void world** (Create New World > World > World Type: Superflat > Customize > Presets > The Void), or Minecraft's
  own ground will be drawn too. Night City's ground is not made of blocks.
* Make the Minecraft window the same **shape** as the Cyberpunk screen (for 1440 x 900, any 16:10 size), since the
  picture is stretched over the screen. The plugin logs a warning if the shapes differ.
* Try `/fill ~-3 ~1 ~5 ~3 ~3 ~5 minecraft:stone_bricks` (a wall 5 blocks south of you) and walk and turn: the wall
  should stay where it is in Night City.
* The game's camera is worked out three ways (its transform, its camera data, and the direction the picture really looks in,
  measured through the game's own world-to-screen function) because only the last is known to follow camera shake, head bob and
  hit reactions. `/cccamsrc transform|data|projected|projectedpos` chooses which is published (default: transform, which follows shake and hits fine); the plugin logs how much
  they disagree every 2 seconds. `/ccroll on|off|flip` applies the camera's sideways tilt. `/ccpredict <ms>` draws the camera's *position* that many
  milliseconds ahead using how fast the Minecraft player is moving (off by default: it makes the blocks jitter).
  Every few seconds the plugin log says how old the camera was when the picture drawn through it reaches the screen, and
  Minecraft's console says whether the camera, field of view and picture shape it really renders through match what was asked. VSync, Minecraft's own view bobbing and its damage shake are switched off in Minecraft while this is on (and put back afterwards).
* **`/ccalign`** lines up the street where you stand with Minecraft's whole-number heights. Blocks sit on whole-number heights, but
  streets are at heights like 22.6, so a block built "on" the street really starts 0.6 m under it; since Minecraft's blocks are
  drawn over the game and not hidden by the ground, that sunken part shows through the road and makes the block seem to slide as you
  move. Stand where you want to build, on level ground, and type `/ccalign` once before building (it is remembered between runs).
* **Hiding blocks behind the game's world** (`/ccocclude`, on by default while blocks are drawn). Minecraft's frame is split into a world
  layer (colour and depth) and an overlay layer (hand, hotbar, screens); the plugin copies the game's own depth texture once a frame
  (see `/ccdepthcapture`) and draws a block pixel only where it is nearer than the game's world there (the game stores depth as
  0.02 / distance). Underground parts of blocks, blocks behind walls and cars, and blocks behind lamp posts are hidden. Turn it off
  with `/ccocclude` if anything goes wrong, and blocks are drawn over everything again. Needs a Minecraft window no bigger than
  3840 x 2160; a smaller window means fewer pixels to copy and a faster frame.
* **Re-aiming the blocks** (`/ccwarp`, on by default). The picture of the blocks takes a few tens of milliseconds to get from Minecraft to the
  screen, so it is drawn through a camera that is a little out of date. Because the world is its own layer with its own depth, the plugin can
  re-aim it at the game's camera as it is when the blocks are drawn: for each pixel it finds where in Minecraft's picture the same point of the
  world is, first from the change in view direction, then correcting for the camera having moved using the picture's depth. The hand, hotbar and
  screens are a separate layer and are not moved. `/ccdelay <ms>` sets how far behind the newest published camera the game's own picture is (what the
  blocks are aimed at): if the blocks swing ahead of the world when you turn, raise it; if they trail, lower it. Needs prediction (`/ccpredict`) at 0.
* **Collision for what you build** (`/cccollide on`). The blocks around you become invisible collision boxes in Night City, so cars are stopped by your builds.
  `BlockColliders.java` reads the real blocks near the player (80 blocks wide, from 16 below to 64 above) every few ticks, turns each block's actual collision shape
  into boxes (full blocks are merged into as few boxes as possible; slabs, stairs, fences and doors give a box per part) and publishes the *complete* list to the plugin
  through shared memory. The plugin (`Collision.cpp`) makes Night City match it: boxes that are new get spawned, boxes that are gone get removed, a few per frame. Because it
  is the complete list every time, a new world, a closed Minecraft or a missed update all fix themselves; nothing is saved, so each world only puts up its own boxes.
  `/cccollide off` takes them down, `/cccollide rebuild` makes everything again, `/cccollide status` says how many there are.
  * *How a box is made.* An empty entity with a collider component. A component can only be added while the game is setting an entity up, so a small script
    (`cyberpunk\scripts\Colliders.reds`, which the build copies to `<game>\r6\scripts\CyberCraft\`) hooks that moment through Codeware's callbacks; adding it to a finished
    entity makes the game crash when the entity is attached. The plugin asks the script for each box and removes boxes through Codeware's `StaticEntitySystem`.
  * *Needs* Codeware (which needs RED4ext and redscript), and `base\spawner\empty_entity.ent`, the empty entity that comes with World Builder (Nexus Mods). That file is
    the only thing taken from World Builder; it should be replaced by one of our own (or used with its author's permission) before anyone else uses this.
  * *Cost.* A frame costs a little more with a few thousand boxes alive (about 10 to 20 percent at 3400 boxes in testing), and a house is a few dozen boxes once merged.
  * *What it does and doesn't stop.* Cars and V's body are stopped (cars crumple when they hit a wall at speed). **NPCs on foot are not**: they walk through low walls
    or over them, and through high ones. Tried and ruled out, so nobody has to try them again: the collision filter numbers (including setting every one to "everything"),
    the collision preset names (the game ignores them here; the numbers decide), the collider's "is obstacle" flag (it switches the collider off), and the game's runtime
    navigation obstacle (`worldNavigationScriptInterface.AddObstacle`: it is accepted but the game's route-finding does not change at all, measured with
    `CalculatePathOnlyHumanNavmesh` before and after). World Builder's own live collision boxes behave exactly the same. The likely reason is that characters collide with a
    separate kind of physical shape, `physicsProxyType.CharacterObstacle`, which World Builder only writes into permanent exported world objects; nothing found lets a
    runtime component make one. The one idea left: push NPCs that end up inside a box back out to its surface (not built).
  * *Where the filter numbers come from.* The query mask 2 = 70107400 and simulation masks 114696 and 23627 in the script are the ones World Builder's live collision shapes use
    (a solid static box that blocks vehicles, the player and bullets); we found they are what decides behaviour, not the preset's name.
* **Logging and the debug tools.** `CyberCraft.log` (next to the plugin) is quiet by default: it records what happens once (loading, Minecraft linking, hooks found, V following,
  how many collision boxes exist) and every warning and error. The statistics that used to print every few seconds (camera accuracy, depth capture and its reports, the ground
  scan, input counts, the overlay's frame delay, V's position) only print while detailed logging is on: `/ccdebug log on` (and `off`) in Minecraft, or a file called
  `verbose-log.txt` next to the plugin for the plugin to start with it on. The measuring keeps running either way. Also: `/ccdebug find <word>` lists every class, enum and
  global function in the game whose name contains the word (the first six letters count), which is how most of the game's inner workings were found, and `/ccdebug dump`
  writes the classes behind the collision boxes to the log, to check them after a game update.
  `/ccdebug ui [from]` (the start of the work on drawing blocks *behind* the game's interface) records every command list the game records over four frames, with the order the queues
  run them in, and writes a summary of the last whole frame between two presents to the log (with `from`, the command lists from that position on are printed in full and the earlier ones only get their line) (lines starting with `ui:`; run it twice, the second time the render targets are resolved to textures, because the first run is what starts watching them): one line per command list (draws, pipelines, dispatches, copies, viewports),
  and for the lists that matter the screen-sized textures they move, the copies between them and their draws grouped into runs with the same pipeline. The first capture showed that
  the list that puts the picture on the back buffer is tiny (one full-screen triangle), so the interface is drawn earlier, into another texture; this finds it, since the blocks will
  have to be drawn into the game's own command list at the point where the interface starts.
* **The game's interface in front of the blocks** (`/ccdebug uilayer <0-4>`, off by default; **unfinished**: the copy of the layer did not line up with the game's own HUD (top elements lower and bottom ones higher by some offset, not a stretch), and the work stopped there because drawing into the game's scene (below) would make it unnecessary). The depth hook also copies the game's HUD layer each frame
  (the screen-sized `R8G8B8A8_UNORM_SRGB` texture with a chain of smaller levels, copied when its full-size level goes from render target to shader resource, which is after the HUD is drawn
  and before the blur passes read it). The overlay draws that copy back over the pixels where there are blocks, so the interface is in front of them: mode 1 treats the layer as
  premultiplied alpha, mode 2 as straight alpha, mode 3 shows the captured layer alone (transparent parts magenta) to check that it is the right texture. Pixels without blocks are left as the
  game drew them, and partly transparent block pixels get the interface applied twice, which is the one inexactness. Needs the depth capture (it uses the same hook).
  The game seems to stretch this layer a little about the middle of the screen when it lays it over the picture (our copy, drawn 1:1, looked smaller and pulled in toward the middle):
  `/ccdebug uilayer 4` shows a magenta shadow of the captured layer over everything, and `/ccdebug uiscale <across%> [down%]` (the game stretches it by different amounts across and down) and `/ccdebug uishift <x> <y>` place it, so it can be lined up by eye
  with the real interface; the numbers that line up go into the code as the defaults.
* **Drawing into the game's own scene** (`/ccdebug scene <0-3>`, proof of concept; DLSS off and path tracing off). The aim: draw the blocks into the HDR scene texture just before the game's
  post-processing, so bloom, exposure, tone mapping, colour grading and the game's own interface (laid over them by the game) apply to the blocks too. The captures show the list that follows the
  last scene drawing begins by copying the scene's HDR texture (a screen-sized R16G16B16A16_FLOAT) into a second one and does no drawing itself. `InScene.cpp` notices that list while the game
  records it (and the state the scene texture is in when it begins), and when the game hands it to the queue, slots a small list of our own in just before it: it moves the scene to render target,
  draws, and moves it back. The proof of concept (modes 1 to 3: a dim, bright and very bright square) worked: it glowed, was tone mapped, lit the wet road in the game's reflections, and had the game's HUD over it.
  Mode 4 draws Minecraft's actual blocks the same way: the overlay's own pipeline (depth test against the game's depth, re-aiming at the camera) with a second pipeline for the HDR scene format and a shader
  output that turns Minecraft's gamma-encoded premultiplied colour into linear scene units (`/ccdebug scenegain <percent>` sets the brightness, `/ccdebug scenedelay <ms>` the aim). The draw at Present then leaves
  the blocks out and draws only the hand, hotbar and screens.
  The brightest pixels (lit whites, glowstone, torch flames) are boosted in the shader (`/ccdebug sceneglow <percent>`, default 300) because a Minecraft white is only 1.0 in scene units while the city's lights are many times that. Minecraft dims faces by direction (top 100%, north and south 80%, east and west 60%, bottom 50%), so the shader works out which way each face points (from the slope of the block layer's depth and the camera's axes in Minecraft's world) and undoes that shading before deciding what glows; otherwise only the tops would. The log says
  `inscene: STOPPED drawing into the game's scene: <why>` if drawing into the scene stops (and `drawing ... again` when it resumes), with how many copies of the scene and how many scene-copy lists without a
  known state were seen since, and then describes, for the first batch of command lists that touches the scene texture, what each list does with it. (First real-world finding: after about 6 minutes the game stopped
  being seen copying the scene; the likely cause is that the copy, which exists for glass and refraction, is not done every frame. The scene texture's state when the copy list begins (0x8C0) is now remembered, so a
  missing barrier is survived, but a missing copy would need a different anchor, which the report is meant to reveal.)
  Known limitation of this first version: the game's depth copy that hides the blocks behind the world is taken at the end of the *previous* frame's depth passes (the main depth is still
  written to by later lists, so its final copy comes after the point where the blocks are drawn), so while turning fast, silhouettes of hidden blocks can be a frame late. The fix is to copy the
  main depth inside the in-scene list, which needs the depth's state at that point (to be learnt by replaying the barriers in the order the lists are run).
* **Ambient occlusion where blocks meet the game's world** (`/ccdebug sceneao <percent> [radiusCm]`, off by default). In the scene shader, each block pixel's place in 3D (from the block layer's depth) and the way its surface faces
  (from the slope of that depth, turned into the current camera's space) are compared with a dozen points of the *game's* depth sampled around it on the screen (a golden-angle pattern turned a little at each
  pixel); a point counts if it is within the radius and in front of the block's surface (on the side it faces), so the road beside or below a surface doesn't darken it but the road in front of the foot of a wall does.
  The result darkens the block's colour (the first attempt exempted bright pixels so that glowing blocks wouldn't go dark, which made the bright wooden blocks show almost nothing, so it now applies to everything; the weight is flat near and fades at the radius, 16 samples, default radius 0.8 m). `/ccdebug sceneaoview 1` shows only the occlusion term, white for none and dark for a lot.
  The first sampling scattered points over a disk of screen pixels, which missed the road in front of a wall's foot at a distance (0.8 m of road seen at a shallow angle is a few pixels tall), so the AO faded with distance.
  It now takes 16 points in 3D over the hemisphere on the side the surface faces, projects each onto the screen, and counts it as hidden if the game's depth there is in front of it, which doesn't depend on distance. This is the first half: blocks darkened by terrain. The second half (terrain darkened by blocks, the contact shadow on the road) needs a depth image of the
  blocks and a second pass, and the one inexactness to expect is that the game's depth is a frame behind, which makes the darkening swim a little when turning fast.
* **Ambient occlusion on the game's own surfaces next to blocks** (`/ccdebug sceneterrainao <percent>`, default 60): the shadow a block makes on the road and walls at its foot. The scene draw now has three passes:
  (1) the blocks' depth as seen from the current camera, into a screen-sized R32_FLOAT texture (0 where there is no block), whether or not the block is hidden behind the game's world; (2) a full-screen pass over the scene that, for
  each pixel of the game's own surface, takes 16 points in 3D over the hemisphere the surface faces (its normal from the game's depth), projects each, and counts it as hidden if a block's depth there is in front of it
  (with a range check so a block far in front doesn't count); the result is multiplied onto the scene (blend: destination colour times the result, alpha kept); (3) the blocks, over all of that. `/ccdebug sceneaoview 1` shows
  the occlusion alone for both. Only the blocks' front surface is known, so each block is taken to be a metre thick (a sample point counts as inside it if it is behind the front surface by less than 1 m); the first version counted any point behind a block's
  silhouette, which darkened the floor at the back and sides of a block far more than in front of it. The defaults are now the values found by playing: scene gain 300%, glow 100%, block occlusion 100%.
* **Direction: Iris/Sodium shaders are retired.** With blocks drawn into the game's own scene (bloom, tone mapping, colour grading, reflections on wet roads, the game's HUD over them) and our own occlusion, the
  look comes from Cyberpunk's renderer; a shader pack would only shade a picture the game covers. What the blocks still don't get from the game: the city's light on them, shadows (both ways), the game's fog and rain
  (applied before the point where blocks go in), reflections on the blocks themselves, and water or glass looking right. Next: sun and weather sync (time of day maps to Minecraft's day, with its own day cycle off;
  weather: Cyberpunk's states mapped onto Minecraft's clear, rain and thunder, with Minecraft's rain drawing kept off). To read the game's time and weather safely, `/ccdebug find <word>` lists classes, enums and global
  functions by name and `/ccdebug class <word>` lists the methods and fields of matching classes, so no script calls are guessed.
* **Minecraft as the clock and weather of Night City (groundwork, protocol 23).** Minecraft publishes its time of day (ticks; 0 is 6:00) and rain and thunder levels in a small seqlocked block (`McWorld`, at 0x240), read
  by name from the client's world (the names differ between mappings; the log says which were found), and `/ccsync time <true|false>` and `/ccsync weather <true|false>` say whether the game's should follow them. The plugin
  reads and logs them (`world:` lines); setting the game's time and weather comes once `/ccdebug class` has shown the calls. Minecraft's three weather states map into Cyberpunk's longer list, and a command of our own
  will pick any of the game's weathers directly.
* **Surviving the game remaking its textures.** Switching path tracing on and off made the game free and remake its render textures; the depth module's table (keyed by address) then kept the old depth texture's
  numbers for whatever new texture got that address, kept choosing it, and captured garbage (a 520 x 512 and then a 960 x 540 colour texture), so the blocks lost their depth and the in-scene drawing stopped
  (`there is no copy of the game's depth`). Now, when a depth barrier names a texture whose size, format or flags differ from the entry's, the entry is started again, and if it was the one being copied the
  capture is stopped and chosen again at once.
* **Where the game draws its interface (found with `/ccdebug ui`).** The last command list of the frame does bloom and post-processing, then moves a *separate* 1920 x 1080
  `R8G8B8A8_UNORM_SRGB` texture into the render target state, clears it, and draws the interface into it (quads, text and icons, about 40 draws). A short list after it
  reads one finished `R8G8B8A8_TYPELESS` 1920 x 1080 texture with a full-screen triangle and writes it to the back buffer. So the scene exists without the interface until a compose step
  after those draws, and the blocks can be added to the scene just before it. (The game re-uses about 60 command lists and runs about 65 per frame; some are not recorded each frame.)
* **Building on Night City** (`/ccterrain`, on by default). Night City isn't made of blocks, so Minecraft can't aim at it. `TerrainPick` marches the
  camera's ray over the same smooth ground surface and obstacle squares that the player collides with, and answers with the hit Minecraft would
  get from a real block: a point, a face, and the empty cell just outside the surface, which Minecraft then fills when you place a block. Nothing
  is added to the world. A real block closer than the street or wall still wins. Limits: there is no block outline on a street or wall, blocks that
  need something to stand on (torches, flowers) won't place on bare ground, and the street can't be broken. The new block goes in the cell whose
  bottom is at the whole-number height at or just below the street (`/ccalign` makes that exact on level road).
* **Aiming.** What you break, place and use is now worked out along the picture's own camera ray, through the middle of the screen where the
  crosshair is, instead of from the Minecraft player's eyes (`EntityPickMixin`). Every few seconds Minecraft's console also prints an `aim:` line saying
  how far the picture's camera is from the player's view, which shows where a mismatch came from.
* **`/ccdepthdebug`** cycles a debug view of the depth test: 1 shows the game's depth over the whole screen (near dark, far light), 2 shows
  the blocks' distance as grey, 3 colours block pixels red where the game's world hides them and green where they show. The capture also
  checks its copy of the game's depth against the game's own rays and switches to another depth texture if the values don't fit.
* **`/ccdepthprobe`** (probe, changes nothing visible; its reports need `/ccdebug log on`) watches the game's depth textures and writes what it sees to `CyberCraft.log` every
  4 seconds: which textures are used as depth buffers, how big they are, what happens to them afterwards, and whether their memory is
  reused. This is groundwork for hiding blocks behind buildings and the ground, which needs a copy of the game's depth buffer.
* **`/ccdepthcapture`** (probe; its readings need `/ccdebug log on`) copies the game's main depth texture right after its last depth pass each frame (it works with DLSS on or off), and compares five
  texels of it with how far the game's own rays say the world is at the same places on the screen. The log shows whether the numbers
  agree and how depth values relate to distance. It records commands into the game's own command list, so if the game misbehaves with it
  on, switch it off and report. Stand still, looking at a street or a wall, for the clearest readings.
* `/ccfov` shows the vertical field of view in use, `/ccfov <degrees>` overrides it to fine-tune, `/ccfov auto` goes
  back. `/ccworld` hides or shows the blocks. `/ccdump` writes the names of Minecraft's rendering methods to a file.

### Playing in the Cyberpunk window (Phase 3a)

With `/ccfollow` on, the keyboard and mouse of the **Cyberpunk window** go to Minecraft: WASD, space, shift, the mouse
to look, clicks to attack and place, the wheel and number keys for the hotbar, `E` for the inventory, `T` to chat.
The game doesn't get them, so V doesn't also run or shoot. With a Minecraft screen open the mouse moves a cursor
that is drawn over the game, and typing goes to Minecraft.

* **Esc** opens Cyberpunk's own pause menu (or closes an open Minecraft screen). Routing pauses by itself while the
  game is paused and comes back when you leave the menu.
* **F9** switches routing off and on by hand, e.g. to use CET's window or a cutscene.
* Alt+F4 still closes the game.

### Seeing Minecraft's HUD over Cyberpunk (Phase 3b)

While `/ccfollow` is on, Minecraft stops drawing the world (you still see it in Cyberpunk) and draws only its hotbar,
hearts, held item and screens on a transparent background. The plugin hooks the game's swapchain and draws those over
every frame. For the first 30 seconds, until the first HUD frame arrives, a small cyan square in the top-left corner
of the Cyberpunk screen shows that the drawing itself works. Make the Minecraft window about as big as the Cyberpunk
screen (the HUD is drawn at the Minecraft window's size, then stretched to the screen). Cyberpunk should be in
borderless or windowed mode with DLSS frame generation off while testing.

### Trying follow mode and the ground (Phase 2b-ii)

Open a **Superflat** Creative world in Minecraft and type `/ccfollow`. The plugin looks at the 1 m cells around
V (as many per frame as fit in 2.5 ms): a ray down finds the ground, and a box test finds anything standing on
it, such as walls, trees and lamp posts. The mod turns both into invisible collision, so you walk on Night
City's real roads and terrain with smooth slopes and are stopped by obstacles. You are moved to V's spot and
V copies your movement and the way you look; click the Minecraft window and use WASD and the mouse.
`/ccstop` ends it. `/ccblocks` also shows the ground as real blocks, a debugging view of what the scan found.

Obstacles are found in 0.125 m squares, so a thin pole is a thin pillar and diagonal walls are fine staircases;
anything blocked is solid up to 2.5 m, and tree leaves (which have no collision in the game) are not solid. If the
player ever ends up inside the ground or an obstacle, they are moved back out.

Players collide with the ground through a smooth collider (a port of the idea in SkyCraft's TriCollider), so slopes
are followed exactly and walls are slid along. `/cccollider` switches to the older block-style collision and back. Bridges and other levels are only seen at V's own height.

## Credits

Architecture and ideas from [SkyCraft](https://github.com/chasmlol/SkyCraft) (MIT); see NOTICE.md. Plugin built on
[RED4ext](https://github.com/WopsS/RED4ext) and [RED4ext.SDK](https://github.com/WopsS/RED4ext.SDK).
Not affiliated with CD PROJEKT RED, Mojang or Microsoft.
