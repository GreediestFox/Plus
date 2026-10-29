---
title: YO server hook - cartPlaces
status: re
domain: reverse-engineering
tags: [hooks, yo-server]
related: [conventions.md, build.md, offsets.md]
sources: [source/server/hooks/engine/hook_cart_places.h, source/server/hooks/engine/hook_cart_places.cpp]
updated: 2026-10-29
---

# cartPlaces

Config-driven in-memory hook for `ddctd_cm_yo_server.exe` (modded *Your Own* servers). Off unless the element below is present in `config/lifxpluss.xml` with `enabled="1"`. Bytes are verified first and the hook skips itself with a log line on a mismatch; nothing is written to the exe on disk. The header comment of `hook_cart_places.h` is the authoritative description.

## Config

```xml
<cartPlaces enabled="1" verbose="1">
        <cart datablockId="707" places="30" />
    </cartPlaces>
```

## Sites

`AbilityImp::PutMovableInCart::_onDoPerform` `0x37E1D0`, compare `41 83 FE 0C 76 17` at `0x37E33A` (imm8 `0x37E33D`); `PullMovableFromCart::_onDoPerform` `0x37D880`; displayed maximum global int `0x81D3E0` (12); message id `2678`; hitched carts resolve by `AttachedShapeData` datablock id (707 = harnessed wood cart), object lookup `0x3E7750`

RVAs are named constants in `source/server/cm_offsets.h`. They are for the current Steam build of the YO dedicated server and must be re-verified after a game update.

## Notes

Same RTTI cast as `wellWater` before the object lookup; harnessed carts (`AttachedObject_Entity`) fall back to the datablock id.
