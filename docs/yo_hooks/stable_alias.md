---
title: YO server hook - stableAlias
status: re
domain: reverse-engineering
tags: [hooks, yo-server]
related: [conventions.md, build.md, offsets.md]
sources: [source/server/hooks/engine/hook_stable_alias.h, source/server/hooks/engine/hook_stable_alias.cpp]
updated: 2026-10-29
---

# stableAlias

Config-driven in-memory hook for `ddctd_cm_yo_server.exe` (modded *Your Own* servers). Off unless the element below is present in `config/lifxpluss.xml` with `enabled="1"`. Bytes are verified first and the hook skips itself with a log line on a mismatch; nothing is written to the exe on disk. The header comment of `hook_stable_alias.h` is the authoritative description.

## Config

```xml
<stableAlias enabled="1" verbose="1">
        <stable objectTypeId="144" allowAnimals="1052" />
        <stable objectTypeId="2834" allowAnimals="1053" strict="1" />
    </stableAlias>
```

## Sites

breeding boot query literal ` WHERE IsComplete =1 AND ObjectTypeID IN (133,134,143,144,517) AND (` at `0x7A1210` (68 chars, rewritten with the same length); parameter table in `.data` `0xACEB40` (11 dwords per record, `-1` terminated); getters `0x1B0EB0` isStable, `0x1B0D60` maxAnimals, `0x1B0D20` / `0x1B04D0` / `0x1B0410` numeric params, `0x1B07E0` product item; `CmBreedingManager::checkContainerLimits` `0x1AC440`

RVAs are named constants in `source/server/cm_offsets.h`. They are for the current Steam build of the YO dedicated server and must be re-verified after a game update.

## Notes

The SQL literal and parameter-table patches are recorded and rolled back if the Detours commit fails.
