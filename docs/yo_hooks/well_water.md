---
title: YO server hook - wellWater
status: re
domain: reverse-engineering
tags: [hooks, yo-server]
related: [conventions.md, build.md, offsets.md]
sources: [source/server/hooks/engine/hook_well_water.h, source/server/hooks/engine/hook_well_water.cpp]
updated: 2026-10-29
---

# wellWater

Config-driven in-memory hook for `ddctd_cm_yo_server.exe` (modded *Your Own* servers). Off unless the element below is present in `config/lifxpluss.xml` with `enabled="1"`. Bytes are verified first and the hook skips itself with a log line on a mismatch; nothing is written to the exe on disk. The header comment of `hook_well_water.h` is the authoritative description.

## Config

```xml
<wellWater enabled="1" verbose="1">
        <well objectTypeId="95" amount="20" itemTypeId="3180" />
        <well objectTypeId="1712" amount="20" itemTypeId="3180" />
        <!-- NOTE: every <swap> applies to EVERY gather call on the server (not only wells): the rule matches the gathered item id wherever it is gathered. -->
        <!-- wild wheat plants sometimes give wild grains instead of wheat -->
        <swap from="352" to="3175" chance="12" />
        <swap from="352" to="3176" chance="12" />
        <swap from="352" to="3177" chance="12" />
        <!-- fish: sometimes a sardine / pike (only if fishing goes through the gather function; see logs\well_water.log) -->
        <swap from="393" to="3178" chance="35" />
        <swap from="392" to="3179" chance="35" />
    </wellWater>
```

## Sites

`AbilityImp::GetWater::_onDoPerform` `0x39F780` (ability 109); `Gathering::Manager::gather` `0x37B090` (item range `{0xCC = 204 Water}`, quantity 1)

RVAs are named constants in `source/server/cm_offsets.h`. They are for the current Steam build of the YO dedicated server and must be re-verified after a game update.

## Notes

The entity the ability targets is passed through `__RTDynamicCast` to `ComplexObject_Entity` before the engine's object lookup (`0x33DDB0`); other entity classes (natural water, carts) are skipped. `<swap>` rules apply to every `gather` call on the server, not only wells.
