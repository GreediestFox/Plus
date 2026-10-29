---
title: YO server hook - craftEffectsRange
status: re
domain: reverse-engineering
tags: [hooks, yo-server]
related: [conventions.md, build.md, offsets.md]
sources: [source/server/hooks/engine/hook_craft_effects_range.h, source/server/hooks/engine/hook_craft_effects_range.cpp]
updated: 2026-10-29
---

# craftEffectsRange

Config-driven in-memory hook for `ddctd_cm_yo_server.exe` (modded *Your Own* servers). Off unless the element below is present in `config/lifxpluss.xml` with `enabled="1"`. Bytes are verified first and the hook skips itself with a log line on a mismatch; nothing is written to the exe on disk. The header comment of `hook_craft_effects_range.h` is the authoritative description.

## Config

```xml
<craftEffectsRange enabled="1" />
```

## Sites

`CraftItemEffectsManager::init` `0x4DA990`: `cmp ecx,25h` at `0x4DAC33` and `cmp dil,26h` at `0x4DAC3F` raised so `data/item_effects.xml` accepts effect ids 1..127 instead of 1..38 (the DB `effects` table is a byte-keyed map, no other limit)

RVAs are named constants in `source/server/cm_offsets.h`. They are for the current Steam build of the YO dedicated server and must be re-verified after a game update.

## Notes

In-memory byte patches only (no Detours); rolled back if the second one fails. Not covered by `build_linux.sh` before this change; untested with clang-cl.
