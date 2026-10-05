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
* **Aiming.** What you break, place and use is now worked out along the picture's own camera ray, through the middle of the screen where the
  crosshair is, instead of from the Minecraft player's eyes (`EntityPickMixin`). Every few seconds Minecraft's console also prints an `aim:` line saying
  how far the picture's camera is from the player's view, which shows where a mismatch came from.
* **`/ccdepthdebug`** cycles a debug view of the depth test: 1 shows the game's depth over the whole screen (near dark, far light), 2 shows
  the blocks' distance as grey, 3 colours block pixels red where the game's world hides them and green where they show. The capture also
  checks its copy of the game's depth against the game's own rays and switches to another depth texture if the values don't fit.
* **`/ccdepthprobe`** (probe, changes nothing visible) watches the game's depth textures and writes what it sees to `CyberCraft.log` every
  4 seconds: which textures are used as depth buffers, how big they are, what happens to them afterwards, and whether their memory is
  reused. This is groundwork for hiding blocks behind buildings and the ground, which needs a copy of the game's depth buffer.
* **`/ccdepthcapture`** (probe) copies the game's main depth texture right after its last depth pass each frame (it works with DLSS on or off), and compares five
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
