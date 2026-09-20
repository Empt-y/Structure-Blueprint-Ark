# StructureBlueprint



Admin area-select / save / paste for structures in **ARK: Survival Evolved**, built as a

server-side [ArkServerAPI](https://github.com/ArkServerApi/AseApi) plugin.



Saved builds live on disk outside the map's save data, so a library survives map changes,

world wipes, reboots, and can be copied between servers.



---



## The one thing that matters most



There are two ways to put a structure into the world, and only one of them produces a

structure that behaves like a structure.



`UVictoryCore::SpawnActorInWorld(...)` on its own creates and renders the actor, but nothing

runs ARK's placement finalizer. The piece keeps `TargetingTeam = 0`, `OwningPlayerID = 0`,

an empty `OwnerName`, and no entry in the structure link graph. A team-0 structure fails the

ownership checks inside `APrimalStructure::TakeDamage`, so it behaves like world geometry:



> **no damage, no demolish, no decay, no tribe ownership**



That is the usual cause of "my pasted base is indestructible". The fix is to call the

engine's own non-player placement entry point immediately after spawning:



```cpp

structure->NonPlayerFinalStructurePlacement(team, owningPlayerId, &ownerName, forcePrimaryParent);

```



This plugin always does that by default. Skipping it is available as an explicit

`--invuln` flag, because indestructible showcase builds are genuinely useful - but it

should be a decision, not an accident.



That is only half of it, though, and the second half took measurement rather than reading.



## The support graph



A finalized structure is damageable and demolishable, but it will still not **collapse** when

its supports are destroyed. ARK propagates collapse through `APrimalStructure::ReprocessTree`,

which walks a link graph - and that graph has to be rebuilt too.



The field that holds it is `LinkedStructures`. This was measured, not assumed: on a

hand-built 3x3 cube, `/sb links` reported



```

Link graph over 62 structures:

  PlacedOnFloorStructure set : 0

  LinkedStructures non-empty : 62 (160 refs)

  StructuresPlacedOnFloor    : 0 (0 refs)

  PrimarySnappedStructChild  : 0

```



`PlacedOnFloorStructure` - the obvious-looking child-to-parent pointer, and this plugin's

first guess - is never populated. Capturing it produced empty graphs on every real build.



Two consequences shape the implementation:



- **It is a graph, not a tree.** Each piece links to several neighbours and every edge is

  mirrored, so `Piece` stores a list of links rather than one parent. On a 62-piece cube

  that is 158 refs, 79 real edges, zero asymmetric.

- **Linking cannot happen during placement.** Because the graph is bidirectional there is no

  placement order in which a piece's neighbours all already exist. Paste therefore runs two

  throttled phases: place everything, then walk the graph calling `LinkStructure`.



`ForcePrimaryParent` is passed `nullptr` for ordinary structural pieces, and the floor
structure for mounted ones.



Verified end to end: a pasted base takes damage, offers demolish, carries the right tribe,

and falls when it is no longer supported by the ground.



---



## Commands



All commands are admin-gated (`APlayerController.bIsAdmin`).



| Command | Effect |

| --- | --- |

| `/sb pos1` / `/sb pos2` | Set the two selection corners at your feet |

| `/sb clear` | Clear your selection |

| `/sb save <name> [--margin N]` | Capture every structure inside the selection |

| `/sb preview <name> [yaw]` | Dry run: corner markers + collision and ground check |

| `/sb confirm` | Build the placement that was previewed |

| `/sb paste <name> [yaw] [--invuln] [--team N] [--rate N]` | Paste at your feet, no preview |

| `/sb list` | List saved builds |

| `/sb info <name>` | Piece count, extent, capture metadata |

| `/sb delete <name>` | Delete a save |

| `/sb undo` | Destroy the most recent paste |

| `/sb cancel` | Abort a running paste and clear preview markers |



**Paste flags**



- `yaw` - rotation about the origin in degrees. Stick to multiples of 90 to keep snap

  geometry aligned; arbitrary angles place fine but look wrong against a grid.

- `--team N` - targeting team to own the base. Defaults to your tribe.

- `--invuln` - skip finalization. Indestructible, unowned, no decay. See above.

- `--rate N` - pieces placed per tick (default 25).



Fly to opposite corners of the build with `cheat fly` / `cheat ghost`, `pos1`, `pos2`, save.



## Selection margin

`InBox` tests a structure's **origin**, not its footprint, and origins sit at tile centres.
The foundation you stand on to set a corner therefore usually has its origin just outside the
box, and gets dropped - which showed up as missing pieces at both the start corner and the far
edge of a large selection.

Capture grows the box by 150 units (half a foundation) on every axis before testing.
`--margin N` overrides it; `--margin 0` is exact. Anything found just outside even the grown
box still raises the clipping warning.

## Preview



`/sb preview` is a dry run. It reports what would happen and drops four marker actors on

the footprint corners, then `/sb confirm` builds at exactly that transform - not wherever

you happen to be standing by then.



It answers three questions:



- **Where and which way.** Footprint extent after rotation, with physical corner markers.

- **Is anything already there.** Counts existing structures inside the target volume.

- **Is the ground actually flat.** Samples a 5x5 grid with `UVictoryCore::GetGroundLocation`

  and reports the spread, warning if the variation exceeds 300 units or the base would sit

  below ground.



ARK's usual translucent placement ghost is **client-rendered** - it is drawn locally by the

client from the structure in your hands. A server-side plugin cannot make a client render a

phantom of a 100k-piece build, so this is markers and numbers rather than a see-through

model. For picking a spot on open ground that is the information that matters anyway; a

ghost would not have told you the back third was in a hillside, and the sample grid does.



Markers are spawned deliberately *without* finalization - they are scaffolding, so being

unowned and indestructible is correct for them. `/sb confirm` and `/sb cancel` both remove

them, and each removal re-queries the world first rather than trusting stored pointers.



---



## Scale



A 16x16 footprint at 32 high with interior rooms runs well past 100k pieces. Three things

keep that workable:



- **Class paths are interned.** Each piece stores an index into a dictionary, not a path.

- **Saves are gzipped.** The coordinate lattice of a large base is extremely repetitive, so

  files land at a few percent of raw JSON. The Poco bundled with the ArkApi SDK is a trimmed

  subset with no zlib, so compression uses vendored [miniz](third_party/miniz) under a real

  gzip container - a `.sbp` is an ordinary gzip file that `gunzip` or 7-Zip will open.

- **Pastes are throttled across frames.** Placing 100k actors in one tick would hang the

  server long enough to drop every client. At the default 25/tick and 30fps that is roughly

  two minutes for a 100k-piece build, and the server stays responsive throughout. Raise

  `--rate` on an empty server, lower it on a populated one.



Only one paste runs at a time, deliberately.



`/sb undo` re-queries the world rather than trusting the pointers it stored, and only

destroys actors that are demonstrably still alive - otherwise a piece destroyed by a player,

decay or a save cycle between paste and undo would leave a dangling pointer to dereference.



---



## Storage



```

<server>/ShooterGame/Binaries/Win64/ArkApi/Plugins/StructureBlueprint/saves/<name>.sbp

```



`.sbp` is gzipped JSON - `gunzip` one to read it. Schema is in `src/Model.h`.

Copy the `saves/` directory to move a library to another server.



---



## Building



Requires Visual Studio with the C++ toolchain (x64) and CMake. The SDK headers and import

libraries are vendored under `sdk/`; miniz under `third_party/`.



The SDK only compiles as a Unicode target (it defines `TCHAR` as `WIDECHAR` and calls the

`_tcs*` macros), so `UNICODE`/`_UNICODE` are required - a narrow build collides on `TCHAR`

itself. It is also `/MD`, so the plugin must match.



```bash

cmake -S . -B build -A x64

cmake --build build --config Release

```



Output: `dist/StructureBlueprint/StructureBlueprint.dll`



## Installing

Prebuilt: grab `StructureBlueprint-v1.0.zip` from the [releases page](https://github.com/Empt-y/Structure-Blueprint-Ark/releases) and follow `INSTALL.txt`.




Copy the `dist/StructureBlueprint/` folder (DLL + `PluginInfo.json`) into:



```

<server>/ShooterGame/Binaries/Win64/ArkApi/Plugins/

```



ArkServerAPI must already be installed on the server. Restart the server.



---



## Current scope



Geometry only: class, position, rotation, and the support graph. **Not** captured:

container inventories, paint regions, PIN codes, or electrical/pipe connection graphs.

Each of those is a self-contained addition to `Piece` in `src/Model.h` plus a capture and

an apply step; the format version field is there to keep old saves loadable when they land.

