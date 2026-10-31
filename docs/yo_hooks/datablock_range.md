---
title: YO server hook - datablockRange
status: re
domain: reverse-engineering
tags: [hooks, yo-server]
related: [conventions.md, build.md, offsets.md]
sources: [source/server/hooks/engine/hook_datablock_range.h, source/server/hooks/engine/hook_datablock_range.cpp]
updated: 2026-10-29
---

# datablockRange

Config-driven in-memory hook for `ddctd_cm_yo_server.exe` (modded *Your Own* servers). Off unless the element below is present in `config/lifxpluss.xml` with `enabled="1"`. Bytes are verified first and the hook skips itself with a log line on a mismatch; nothing is written to the exe on disk. The header comment of `hook_datablock_range.h` is the authoritative description.

## Config

```xml
<datablockRange enabled="1" verbose="1" />
```

## Sites

`Complex::ObjectType::TypeStorage` ctor counter imm32 `0xCEC86` (800 -> 1027); ghost pack write `mov r9d,402h` imm32 `0x13372B`, read `mov r8d,402h` imm32 `0x133D24` (0x402 -> 0x1002); first ordinary object id imm32 `0x4297F7` and live counter `0xBC8FD4` (0x443 -> 0x1043); `writeInt` width 10 -> 12 at `0x11D7D8`, `0x138989` (`Sim2DAudioEvent::pack`) and `0x1389B6`; matching `readInt` widths 10 -> 12 at `0x11F429`, `0x138A1A` (`Sim2DAudioEvent::unpack`) and `0x138A56`; the mounted-image writer's space check `0x11D7BE` (imm8) and `0x11D7C8`. The client executable needs the equivalent change (no client patcher is part of this repository)

RVAs are named constants in `source/server/cm_offsets.h`. They are for the current Steam build of the YO dedicated server and must be re-verified after a game update.

## Notes

Every site is verified (opcode bytes and current value) before any write; one mismatch patches nothing. Coverage: the sites come from a scan of the whole server for 10-bit ranged writes and reads next to a `datablock id - 3` / `+ 3` conversion; a datablock id serialised in some other shape would not show up in that scan.
