---
title: Craftwork working-containers (tanning tub)
status: re
domain: reverse-engineering
tags: [craftwork, working-container, tanning-tub, hooks, objects-types]
related: [bloomery.md, conventions.md, reverse_engineering.md]
sources: [bloomery.md, conventions.md, reverse_engineering.md, ../source/server/cm_offsets.h]
updated: 2026-06-26
---

# Craftwork working-containers (tanning tub)

The tanning tub is the **last unhooked leaf** of the craftwork subsystem. This page records what its C++ class is, why its conversion can't be configured from data, and what RE remains before LiFx can control it — so the next person doesn't re-derive it.

## TL;DR

- The tanning tub is **object type `472`**, driven by the compiled C++ class **`WorkingContainer`** (a sibling of the furnace family, both managed by **`CmCraftworkManager`**). Its player verb is the **`UseTanningTube`** ability (skill **id `85`**).
- **It is the only device of the `WorkingContainer` class** — there are no sibling tub/vat/soak/ret/dry/press devices. The furnace family (`AbstractCraftworkFurnace`: bloomery, kiln, brewing tank, fire, greenhouse, trap, windmill) is **already hooked** (see [`bloomery.md`](bloomery.md)); `WorkingContainer` is **not**.
- **The conversion — output item type *and* quantity — is computed in the binary, not in data.** No XML/SQL field expresses an input→output ratio for the tub. The DB table `working_containers` holds only *runtime state* (which tub is busy, when it finishes, what it will yield).
- The ability is gated on the **literal** `object_type_id 472` (no `parent="1"` hierarchy walk), so a new object type is not recognized as a tub without a hook. Two co-existing tub variants (e.g. a 5→5 and a 50→50 processor) therefore need RE, not data.
- **Net value of doing the RE:** `WorkingContainer` is single-type today (not a multiplexer like the furnace proc-descriptor lookup), so a hook buys exactly the tanning tub — but `CmCraftworkManager` is a *generic, type-dispatched* loader, so the hook becomes a reusable seam for new timed put-in/wait/get-out devices you design.
- **No LiFx hook and no RVAs exist yet.** Next step: locate `WorkingContainer`'s tick/finalize routine, the `UseTanningTube` handler, and `CmCraftworkManager::loadObjects` via the [`reverse_engineering.md`](reverse_engineering.md) Ghidra workflow, then author a hook per [`conventions.md`](conventions.md).

## The craftwork class family

`CmCraftworkManager` owns a family of `Working*` object classes (RTTI strings recovered from `ddctd_cm_yo_server.exe`; module source path baked in as `x:\dev\cm_clone\cm_yo_release\engine\source\app\craftwork\cmcraftworkmanager.cpp`):

```
WorkingObject                       // .?AVWorkingObject@@  (base)
   ├── AbstractCraftworkFurnace      // furnace family — HOOKED (see bloomery.md)
   │      ├── WorkingFurnace         // bloomery / kiln / smelter
   │      ├── BrewingTankFurnace     // brewing tank / fermenter
   │      ├── WorkingFire            // campfire / hearth
   │      ├── WorkingGreenhouse      // greenhouse
   │      ├── WorkingTrap            // animal traps
   │      └── WorkingWindmill        // windmill
   └── WorkingContainer              // .?AVWorkingContainer@@  — tanning tub — NOT HOOKED
```

The furnace branch is hooked through a shared **process-descriptor lookup at RVA `0x1DB7C0`** plus per-class `recalcTick` slots — that lookup is a *multiplexer* across many furnace object types ([`bloomery.md`](bloomery.md)). `WorkingContainer` sits on the **other** branch under `WorkingObject` and has its own, undocumented, unhooked processing path.

RTTI / string anchors for the container side:

| Symbol (string in the exe) | What it is |
|---|---|
| `.?AVWorkingContainer@@` | the tanning-tub runtime class |
| `.?AVCmCraftworkManager@@` | the manager that owns `working_containers` |
| `.?AVUseTanningTube@AbilityImp@@`, `.?AVUseTanningTube_Ability@@` | the "Use Tanning Tub" ability handler |
| `CmCraftworkManager::loadObjects() - db 'working_containers' error` | loader error string |
| `Bad object type, %u` | loader's per-type gate (proves it dispatches by `ObjectTypeID`) |
| `WorkingContainerBaseTimeMs` | base processing-time tuning (compiled, not in a config file) |

## The tanning tub in data

**Object type** (`objects_types.xml`, ID `472`):

```xml
<ID>472</ID>  <ParentID>64</ParentID>  <Name>Tanning Tub</Name>
<IsContainer>1</IsContainer>  <IsDevice>1</IsDevice>  <IsMovableObject>1</IsMovableObject>
<MaxContSize>25000</MaxContSize>  <Length>6</Length>  <UnitWeight>10000</UnitWeight>
```

`ParentID 64` is the generic "Crafting" abstract category — **not** a "tanning tub" superclass, so hierarchy matching can't be used to enrol a second type.

**Abilities** (`skill_types.xml`):

- `Use Tanning Tub`, **id `85`** — `<ent_req type="object_type_id">472</ent_req>`, **no `parent="1"` attribute** → exact-id match only.
- `Pick Up`, **id `116`** — also gated on the literal `472` (for picking up a *working* tub).

Contrast: elsewhere in the same file `ent_req type="object_type_id"` is written *with* `parent="1"` for hierarchical matching (the engine's `Type::hasParent` walk, RVA `0x27EB30` — see [`bloomery.md`](bloomery.md)). Its absence here is the exact-id gate.

**Build recipe** (`recipe.xml`, row `593`): builds type `472` (skill type `8`, level `30`). This is only the recipe to *construct* the tub — it is not the processing conversion.

**Runtime-state DB table** (`sql/patch.sql`, `working_containers`):

```sql
CREATE TABLE `working_containers` (
  `ID` INT UNSIGNED AUTO_INCREMENT,
  `MovableObjectID`   INT UNSIGNED NULL,   -- the busy tub (movable OR unmovable, not both)
  `UnmovableObjectID` INT UNSIGNED NULL,
  `FinishTime`  TIMESTAMP NOT NULL,        -- when processing completes
  `ResultItemID` INT UNSIGNED NULL,        -- output type, set by the engine at runtime
  `InputSkillValue` FLOAT NULL,            -- quality/skill snapshot
  ...);
```

The engine derives `ResultItemID`, `FinishTime`, and `InputSkillValue` at runtime and persists them with an `INSERT ... ON DUPLICATE KEY UPDATE`. **`ResultItemID` is never read from XML/SQL as a tub-config field** — it is a runtime `%u` the compiled code computes. There is no input-quantity or ratio column anywhere.

## Why two tubs of different batch size need RE

The data lets you change the tub's **capacity** (`MaxContSize`, `Length`), its **build cost** (recipe `593`), its **art**, and **which buildings the ability targets** (the `object_type_id` id-list in `skill_types.xml`). It does **not** let you:

1. Define a second co-existing tub type — the exact-`472` ability gate (and the compiled `Bad object type, %u` gate in `CmCraftworkManager`) reject any new type id; and
2. Set an input→output **ratio/quantity** for any tub — that logic lives in `WorkingContainer`/`UseTanningTube`/`CmCraftworkManager`, in `.text`, not data.

So a "5 flax-stems → 5 flax" tub alongside a "50 → 50" tub (both 1:1, differing only in batch size) is **not** a data-only change. Note the stock tub's real conversion is **hides → dried hides**; a flax processor is new conversion behaviour the engine doesn't currently expose at all, which puts it in hook territory regardless of the batch-size question.

## What the RE buys (and doesn't)

- **No free siblings.** `WorkingContainer` services exactly one object type (`472`). Unlike the furnace proc-desc lookup at `0x1DB7C0`, it is not a multiplexer — hooking it gains the tanning tub and nothing else that exists today.
- **A reusable seam.** `CmCraftworkManager` *is* a generic, type-dispatched loader (it reads `ObjectTypeID` from `working_containers` and logs `Bad object type, %u` for ones it doesn't know). Once hooked, you can register **new** object types as working containers — your big tub, a flax-retting vat, a soaking trough — each riding the same hook (each still needs its own object-type row + ability row; the gating stays per-type).
- **It closes the last gap.** With the furnace branch already hooked, `WorkingContainer` is the only craftwork leaf LiFx doesn't yet cover.

## Adjacent processing devices (a different model — not WorkingContainer)

These are processing devices but are **not** `WorkingContainer`; they run on the **recipe-driven** crafting model via their own `Use*` ability handlers, so their outputs are already defined in recipe data (a more data-controllable path):

| Type | Device | Handler |
|---|---|---|
| `118` | Drying Frame | recipe/greenhouse-driven |
| `119` | Wine Press | own handler |
| `120` | Spinning Wheel | `UseSpinningWheel` |
| `121` | Loom | `UseLoom` |
| `122` | Beehive | own handler |
| `123` | Potter's Wheel | `UsePotterWheel` |
| `116` | Quern-stone | own handler |

Treat any specific one separately if it matters — they are not "uncontrolled working containers" in the tanning-tub sense.

## Next step (RE recipe)

Following [`reverse_engineering.md`](reverse_engineering.md):

1. Run `LifxExport.java` to (re)generate `classes.txt` / `strings.tsv` / `functions.tsv` in `/tmp/lifx_ghidra/`.
2. `grep -i 'WorkingContainer\|UseTanningTube\|CmCraftworkManager\|working_containers' /tmp/lifx_ghidra/{classes.txt,strings.tsv}` and read the xref column to reach the owning functions.
3. Walk the `WorkingContainer` vtable to find its tick/finalize slot (the analogue of `WorkingFurnace::recalcTick`); identify where it computes `ResultItemID` and the consume-input/spawn-output quantities; locate the `UseTanningTube` ability `_onDoPerform`.
4. Record the RVAs as `CRAFTWORK_*` constants in [`../source/server/cm_offsets.h`](../source/server/cm_offsets.h) and author the hook under `source/server/hooks/craftwork/` per [`conventions.md`](conventions.md). Per project policy this is a Detours/DLL hook, never an exe byte-patch.

## Status & provenance

- **`re`** — the class identity, ability gating, DB schema, and object/ability/recipe ids are read directly from `ddctd_cm_yo_server.exe` RTTI/strings and the server data files (`objects_types.xml`, `skill_types.xml`, `recipe.xml`, `sql/patch.sql`). **Verified** by those artifacts.
- **Inferred / not yet done:** the exact processing semantics (whether the tub consumes its contents 1:1 over whatever is loaded vs. a fixed batch), and *all* RVAs — none are decompiled or recorded yet. Confirming the 1:1-over-contents behaviour and the output-type computation requires decompiling the `WorkingContainer` tick/finalize routine (step 3 above).

See also: [`YO_SERVER_HOOKS.md`](YO_SERVER_HOOKS.md) - `greenhouseAlias` keeps custom drying frames and tanning tubs working across restarts (`behavesLike` 118 / 472) and `herbGardenGate` adds a timed plant/collect cycle.
