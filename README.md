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
3. **HUD and hand**: the Minecraft hotbar and held item drawn over the game.
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
