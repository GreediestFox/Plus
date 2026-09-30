# Life is Feudal: Your Own - server hooks (engine extensions)

New in-memory hooks for `ddctd_cm_yo_server.exe`, all following the Plus rules (the server exe is **never** modified on disk; every change is a detour or a runtime memory patch that is verified byte by byte before it is applied, and skipped with a log line if the bytes do not match).

Each hook has its own header with the full reverse-engineering notes (RVAs, structures, how the engine wires the feature) and the exact config syntax. This page is the index and the how-to.

All RVAs are for the current Steam build of the YO dedicated server (AppID 1062390 depot) at the time of writing. If a hook logs a signature mismatch at boot it simply does not attach; it never patches blindly.

## Quick start

1. Build (same as every other Plus hook):
   ```
   msbuild win\LiFx.vcxproj /t:Build /p:Configuration=Release /p:Platform=x64
   ```
2. Copy the built DLL next to the server exe exactly like the rest of Plus (see the existing README).
3. Add the elements you want to `config/lifxpluss.xml` (full example: [`docs/examples/lifxpluss.yo-hooks.example.xml`](examples/lifxpluss.yo-hooks.example.xml)). Every hook is **off unless its element is present with `enabled="1"`**. Set `verbose="1"` the first time; the boot log then states exactly which sites were patched.
4. Start the server. Look for the hook's log lines (e.g. `gatherable type limit raised: valid types 0..224`).

The ids in the example config (object types 2829, 2894, ...) belong to a specific modpack; replace them with your own object/ability/recipe ids.

## Hooks

| Hook (source in `source/server/hooks/engine/` unless noted) | What it does | Config element |
|---|---|---|
| `hook_crop_types` | Extra farmable crops that reuse the engine's shared sow/grow/harvest code (new ability id + new 10-wide substance block per crop), plus `maxGatherableType` to raise the wild-plant gatherable type limit from 218 to up to 254 | `<cropTypes>` |
| `hook_datablock_range` | Widens the network datablock id range (0x402 -> 0x1002) so the number of movable object types is no longer capped around 220. **The client exe needs the same change** | `<datablockRange>` |
| `hook_greenhouse_alias` | A custom object type behaves as the vanilla Herbal Garden (1353), or keeps working state across restarts like a Drying Frame (118) / Tanning Tub (472) | `<greenhouseAlias>` |
| `hook_herb_garden_gate` | "Plant now, collect later" timer for crafting-based gardens, and instant-with-fixed-quality collect gates | `<herbGardenGate>` |
| `hook_stable_alias` | A custom building behaves as Coop / Barn / Stable, with per-building capacity and an allowed-animals / single-item-store rule | `<stableAlias>` |
| `hook_workshop_buff` | The x1.2 workshop crafting-quality buff for any configured workshop type and ability (vanilla has it for six hard-coded buildings only) | `<workshopBuff>` |
| `hook_well_water` | A well gives N water per "Get Water" action | `<wellWater>` |
| `hook_cart_places` | Per-cart-type capacity for "put movable in cart" (vanilla: 12 for every cart) | `<cartPlaces>` |
| `ability/hook_register_perform`, `hook_light_working_object`, `hook_resolve_light_object` | Observation-only probes used to trace the "Light the Fire" ability | (diagnostic) |
| `api/lifx_geo.*`, `api/lifx_effects.cpp`, `cm_offsets.h`, `engine_internals.h` | Supporting helpers and offsets for the hooks above | - |

## Crops (the most involved one)

`hook_crop_types` only does the server-native part. A new crop also needs:

* a server **seed/harvest item** type and a mod `dbChanges()` row for it,
* a **client Sow ability** in `skill_types.xml` mirroring an existing crop's ability block under the new id (server and client copy),
* one **substance + terrain material pair per growth stage** on the client (script data),
* for wild gathering: rows in `gatherables.xml` (server and client), the new gatherable `type` ids, and the ability 68 `ent_req` description word list.

`aliasBase` (optional) makes the new crop's substances behave like another vanilla crop's stage ids instead of Wheat's (for example carrots or grapes), which is what decides the look until the crop gets its own art.

## Things to know

* The engine rewrites `objects_types` / `recipe` at boot from the seed SQL, then runs mod `dbChanges()` callbacks. New object types must be registered in a mod `dbChanges()` or baked into the seed SQL; a one-off manual INSERT is wiped at the next restart.
* `sp_checkForeignKeys` runs **before** mod `dbChanges()`; a dangling reference to a mod-only type is a fatal boot error.
* Steam file verification/updates restore the stock server exe and any patched client files. Re-apply the client-side patches after a verify.
* `lfxe_key_data.h` is a local placeholder and is intentionally not part of this change (it is gitignored upstream).

## Testing done

Each hook was verified in a running server with players: log lines at boot, in-game behaviour (sowing/harvesting, carrying movables with >1024 datablocks, cart capacity 30, well yield, workshop quality, stable restrictions, restart persistence of drying frames/tanning tubs), and a clean restart afterwards.
