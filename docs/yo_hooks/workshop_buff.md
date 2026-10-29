---
title: YO server hook - workshopBuff
status: re
domain: reverse-engineering
tags: [hooks, yo-server]
related: [conventions.md, build.md, offsets.md]
sources: [source/server/hooks/engine/hook_workshop_buff.h, source/server/hooks/engine/hook_workshop_buff.cpp]
updated: 2026-10-29
---

# workshopBuff

Config-driven in-memory hook for `ddctd_cm_yo_server.exe` (modded *Your Own* servers). Off unless the element below is present in `config/lifxpluss.xml` with `enabled="1"`. Bytes are verified first and the hook skips itself with a log line on a mismatch; nothing is written to the exe on disk. The header comment of `hook_workshop_buff.h` is the authoritative description.

## Config

```xml
<workshopBuff enabled="1" refreshSeconds="30" verbose="1" debug="0">
        <!-- Read-only account: GRANT SELECT ON lif_1.unmovable_objects TO ... (workshopBuff only reads placed workshops). Never use root. -->
        <db host="127.0.0.1" port="3306" user="lifx_ro" password="CHANGE_ME" name="lif_1" />
        <workshop objectTypeId="2894" radiusTiles="6" multiplier="1.2" abilityIds="201 316" />
        <!-- Tailor's Workshop: Sew (88), Sew Armor (282) -->
        <workshop objectTypeId="2836" radiusTiles="6" multiplier="1.2" abilityIds="88 282" />
        <!-- Stonemason's Workplace: Shape Stones (61), Mold (62), Sculpt (64) -->
        <workshop objectTypeId="2020" radiusTiles="6" multiplier="1.2" abilityIds="61 62 64" />
    </workshopBuff>
```

## Sites

`CmCreationManager::checkExceptionalChance` `0x1E46B0` (3rd arg is the GameConnection; `GameConnection::vftable` `0x782DA8`); `craftWithTool` `0x1E5820`; `GameConnection::getControlObject` `0x135C00` (Player = `**(conn+0xB38)`); `Geo::WorldToGeoId` `0x57AAE0`; Player char id stamp `+0x1B44`, cached geoId `+0x2464`; vanilla x1.2 constant `DAT_1407ace74`

RVAs are named constants in `source/server/cm_offsets.h`. They are for the current Steam build of the YO dedicated server and must be re-verified after a game update.

## Notes

The poller thread is stoppable (event + join) and is stopped by `DetachWorkshopBuffHook`. Use a SELECT-only database account. `WorkshopBuffSuppressScope` lets other hooks exempt their own crafts.
