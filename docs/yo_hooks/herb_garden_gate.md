---
title: YO server hook - herbGardenGate
status: re
domain: reverse-engineering
tags: [hooks, yo-server]
related: [conventions.md, build.md, offsets.md]
sources: [source/server/hooks/engine/hook_herb_garden_gate.h, source/server/hooks/engine/hook_herb_garden_gate.cpp]
updated: 2026-10-29
---

# herbGardenGate

Config-driven in-memory hook for `ddctd_cm_yo_server.exe` (modded *Your Own* servers). Off unless the element below is present in `config/lifxpluss.xml` with `enabled="1"`. Bytes are verified first and the hook skips itself with a log line on a mismatch; nothing is written to the exe on disk. The header comment of `hook_herb_garden_gate.h` is the authoritative description.

## Config

```xml
<herbGardenGate enabled="1" verbose="1" expireDays="90" abilityId="328" growSeconds="32400" firstGrowRecipe="5200" lastGrowRecipe="5265" collectOffset="100" pendingMessageId="3632" emptyMessageId="5160" collectAnyRecipe="5299" timeMessageBase="5099" file="config/herb_garden_batches.txt">
        <!-- Ore Washer (Sculpt ability 64): instant craft after a durationSeconds progress bar; output quality follows the curve (input +20 falling to +1 at 90, capped at 90); workshop bonus off. -->
        <gate abilityId="64" instant="1" durationSeconds="60" firstGrowRecipe="5610" lastGrowRecipe="5613" curveEnd="90" curveGainStart="20" curveGainEnd="1" noWorkshopBuff="1" />
    </herbGardenGate>
```

## Sites

`CmCreationManager::craftWithDevice` `0x1E4B10`, `_finallyMakeItem` `0x1E3C60`; device-craft ability slot 3 `0x365BE0`; greenhouse reagent check `0x1DD8C0`, start `0x1DE740`, tick `0x1DEA70`; message helper (connection, id only) `0x8CC10`

RVAs are named constants in `source/server/cm_offsets.h`. They are for the current Steam build of the YO dedicated server and must be re-verified after a game update.

## Notes

Depends on `workshopBuff` (`WorkshopBuffSuppressScope`, used for `noWorkshopBuff="1"`): merge that first.

`expireDays` (default 90, 0 = never) drops batches nobody collected that long after they were ready; there is no demolish hook, so age is the only signal. `growSeconds` is limited to 2147483 on gates with `workingState` (the engine timer is an int of milliseconds). The batch file is replaced atomically (`MoveFileExW`, write-through) and a failed write keeps the previous file. Open question: the batch key is read at `entity+8`; the persistent object id may live at `entity+900`.
