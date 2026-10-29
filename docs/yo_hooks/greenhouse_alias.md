---
title: YO server hook - greenhouseAlias
status: re
domain: reverse-engineering
tags: [hooks, yo-server]
related: [conventions.md, build.md, offsets.md]
sources: [source/server/hooks/engine/hook_greenhouse_alias.h, source/server/hooks/engine/hook_greenhouse_alias.cpp]
updated: 2026-10-29
---

# greenhouseAlias

Config-driven in-memory hook for `ddctd_cm_yo_server.exe` (modded *Your Own* servers). Off unless the element below is present in `config/lifxpluss.xml` with `enabled="1"`. Bytes are verified first and the hook skips itself with a log line on a mismatch; nothing is written to the exe on disk. The header comment of `hook_greenhouse_alias.h` is the authoritative description.

## Config

```xml
<greenhouseAlias enabled="1" verbose="1">
        <!-- Big Drying Frame / Big Tanning Tub: restore their working state (model + timer) at boot; finished-while-down = Working with 1 s left -->
        <alias objectTypeId="2829" behavesLike="118" finishedAsWorking="1" />
        <alias objectTypeId="2017" behavesLike="472" finishedAsWorking="1" />
        <!-- Big Tanning Tub (2017): the tub abilities pick the first matching recipe, which is the vanilla 1-at-a-time one; hand back the 5-at-once versions instead (thick leather, thin leather, flax fibers). -->
        <recipeOverride objectTypeId="2017" from="186" to="5021" />
        <recipeOverride objectTypeId="2017" from="187" to="5600" />
        <recipeOverride objectTypeId="2017" from="191" to="5601" />
        <!-- Extra storage buildings the open-inventory check ("Look in Inventory") must treat like the vanilla warehouses: 2480 = Woodshed, 2400 = Small Wooden Shed, 2830-2832 = Ore/Stone/Grain Stockpile, 2833 = Compost Heap. -->
        <warehouse objectTypeId="2480" />
        <warehouse objectTypeId="2400" />
        <warehouse objectTypeId="2830" />
        <warehouse objectTypeId="2831" />
        <warehouse objectTypeId="2832" />
        <warehouse objectTypeId="2833" />
        <warehouse objectTypeId="2834" />
    </greenhouseAlias>
```

## Sites

`CmCraftworkManager::loadObjects` `0x1D8E20` (compare at `0x1D8E92`, arg 4 remapped); greenhouse "use" entry `0x1D91D0` (compare type id `0x549` at `0x1D92AD`); `Entity::GetTypeInfo` `0xC9930`, only for the call at `0x1D9220` (return address `0x1D9225`); `Type::getId` `0xD3790` (4-byte getter, cannot be detoured)

RVAs are named constants in `source/server/cm_offsets.h`. They are for the current Steam build of the YO dedicated server and must be re-verified after a game update.

## Notes

Each feature (core remap, recipe override, extra warehouses) attaches in its own Detours transaction. Same RTTI cast as `wellWater` before the object lookup.
