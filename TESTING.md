# Local testing

## Layout

| Thing | Path |
| --- | --- |
| SteamCMD | `C:\steamcmd` |
| Test server | `C:\ARKServer` |
| ArkServerAPI | `C:\ARKServer\ShooterGame\Binaries\Win64\` (`version.dll`) |
| Plugin | `...\Win64\ArkApi\Plugins\StructureBlueprint\` |
| Saves | `...\ArkApi\Plugins\StructureBlueprint\saves\` |

The test server is a **separate install** from the Steam client. ArkServerAPI works by
loading a proxy `version.dll` next to the executable, and putting that in the client folder
would inject it into `ShooterGame.exe` alongside BattlEye. Keep them apart.

## Run

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\start-server.ps1
```

First boot takes a few minutes (it generates the world). It is ready when the log stops
scrolling and reports the session is registered.

Join from the ARK client — a local unlisted server usually will not appear in the browser,
so use the console (`Tab`) on the main menu:

```
open 127.0.0.1:7777
```

Then in-game, open the console and become admin:

```
enablecheats test123
```

## Deploy a code change

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\deploy.ps1
```

ArkApi polls for changed plugins every 5 seconds (`AutomaticPluginReloading` in
`Win64\config.json`) and saves the world before reloading, so in most cases you do **not**
need to restart the server to pick up a rebuild.

## Test plan

Work up in scale. The failure modes at 20 pieces and 100k pieces are completely different.

**1. Smoke test — does it load at all**

Check `...\Win64\ArkApi\Logs\StructureBlueprint.log` for the load line, then in chat:

```
/sb
```

Expect the usage list. If nothing happens, the plugin did not load — check the log.

**2. Tiny build — correctness**

Build a 2x2 foundation with a few walls and a ceiling by hand. Then:

```
/sb pos1          (stand at one corner)
/sb pos2          (opposite corner, above the roof — the box must enclose it vertically)
/sb save hut
/sb info hut
```

Fly somewhere empty and:

```
/sb paste hut
```

Now the checks that actually matter:

- **Hit it with a pick.** It must take damage. If it is immune, finalization did not run.
- **Hold `E` on a piece.** A demolish option must appear.
- **Check the tribe name** on the piece matches yours.
- `/sb undo` should remove exactly what was pasted.

Then compare against the deliberate opposite:

```
/sb paste hut --invuln
```

That one *should* be indestructible and unowned. If both behave the same, something is wrong.

**3. Preview and collision check**

Stand in the open and:

```
/sb preview hut
```

Expect four corner markers, a footprint readout, "Clear: no existing structures in the way",
and a ground variation figure. Then `/sb confirm` - it must build at the **markers**, not at
wherever you have wandered to since.

Now the negative cases, which are the point of the feature:

- Run `/sb preview hut` standing inside an existing base. It must report a non-zero
  "existing structures are inside this volume" warning.
- Run it on a hillside. Ground variation should exceed 300 and warn about uneven ground.
- Run `/sb preview hut` then `/sb cancel`. All four markers must disappear.

**4. Rotation**

```
/sb paste hut 90
```

Walls should stay square to the foundations. Use multiples of 90.

**5. Scale**

Only after the above pass. Build or spawn something large, save it, and watch server
responsiveness during the paste. Tune `--rate`:

```
/sb paste bigbase --rate 10     (gentler, slower)
/sb paste bigbase --rate 100    (faster, heavier)
```

Watch for frame hitching in-game while it streams in. The default is 25/tick.

## Things to watch for

- **Pieces missing after paste** — likely a class that failed to resolve. The completion
  message reports a failed count; a non-zero value means `BPLoadClass` returned null for
  something, usually a modded structure absent from the test server.
- **Floating pieces / collapse on damage** — the support graph did not survive. Check that
  capture recorded parents (`f` fields in the JSON) by gunzipping a save.
- **Server hitch during paste** — lower `--rate`.
