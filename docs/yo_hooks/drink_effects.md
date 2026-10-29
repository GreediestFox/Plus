---
title: YO server hook - drinkEffects
status: re
domain: reverse-engineering
tags: [hooks, yo-server]
related: [conventions.md, build.md, offsets.md]
sources: [source/server/hooks/engine/hook_drink_effects.h, source/server/hooks/engine/hook_drink_effects.cpp]
updated: 2026-10-29
---

# drinkEffects

Config-driven in-memory hook for `ddctd_cm_yo_server.exe` (modded *Your Own* servers). Off unless the element below is present in `config/lifxpluss.xml` with `enabled="1"`. Bytes are verified first and the hook skips itself with a log line on a mismatch; nothing is written to the exe on disk. The header comment of `hook_drink_effects.h` is the authoritative description.

## Config

```xml
<drinkEffects enabled="1" verbose="1" dump="0">
        <!-- Read-only account: GRANT SELECT ON lif_1.items TO ... (one persistent connection, results cached). Never use root. -->
        <db host="127.0.0.1" port="3306" user="lifx_ro" password="CHANGE_ME" name="lif_1" />
        <drink objectTypeId="3920">   <!-- Thin Beer: Accelerated + Clumsiness -->
            <effect id="6" magnitude="0.10" />
            <effect id="7" magnitude="0.10" />
        </drink>
        <drink objectTypeId="3921">   <!-- Strong Beer: Swiftness + Slowed -->
            <effect id="8" magnitude="0.20" />
            <effect id="5" magnitude="0.20" />
        </drink>
    </drinkEffects>
```

## Sites

`CharacterSaveableEffects::applyPotionItemEffect` `0x1C4A40` (`fx`, item, **float** mult = magnitude of Full; called by `AbilityImp::Drink::_onDoPerform` `0x36F850`); core add-effect `0x1C3AA0` (fx, player effect id, float value, int durationMs, int 0); item effect feature lookup `0x27C040` (item feature 5, `ItemEffectsFeature`); duration = q*q*`[0x736568]`(1000.0)/`[0x73AB8C]`(15.0) ms (q = u16 at item+0x20); item id at item+0x08; writer `CharacterSaveableEffects::_addEffect` `0x1C3350`; client blur: `FUN_140932610` calls `initDrinkPostEffect` when effect `0x19` (Full) changes

RVAs are named constants in `source/server/cm_offsets.h`. They are for the current Steam build of the YO dedicated server and must be re-verified after a game update.

## Notes

Item types are read with one persistent connection, cached per item id, 1 s timeouts and a circuit breaker (3 failures -> 30 s pause). Use a SELECT-only account.
