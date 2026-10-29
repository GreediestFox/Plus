---
title: YO server hook - cropTypes
status: re
domain: reverse-engineering
tags: [hooks, yo-server]
related: [conventions.md, build.md, offsets.md]
sources: [source/server/hooks/engine/hook_crop_types.h, source/server/hooks/engine/hook_crop_types.cpp]
updated: 2026-10-29
---

# cropTypes

Config-driven in-memory hook for `ddctd_cm_yo_server.exe` (modded *Your Own* servers). Off unless the element below is present in `config/lifxpluss.xml` with `enabled="1"`. Bytes are verified first and the hook skips itself with a log line on a mismatch; nothing is written to the exe on disk. The header comment of `hook_crop_types.h` is the authoritative description.

## Config

```xml
<cropTypes enabled="1" verbose="1" maxGatherableType="224"> <!-- maxSubstanceWidth is derived from the crops (max 155) -->
    <!-- Wild Rye/Barley/Oat reuse Wheat's own terrain visuals+friction for now (aliasBase defaults to Wheat/101);
         Hops reuses Grape's (aliasBase="161"); Sugar Beet reuses Carrot's (aliasBase="141").
         The five crops get their own textures separately later. -->
    <crop abilityId="360" seedItemId="3175" fertileSmallSubstance="191" nonFertileSmallSubstance="192" name="Rye" />
    <crop abilityId="361" seedItemId="3176" fertileSmallSubstance="201" nonFertileSmallSubstance="202" name="Barley" />
    <crop abilityId="362" seedItemId="3177" fertileSmallSubstance="211" nonFertileSmallSubstance="212" name="Oat" />
    <crop abilityId="363" seedItemId="3751" fertileSmallSubstance="221" nonFertileSmallSubstance="222" aliasBase="161" name="Hops" />
    <crop abilityId="364" seedItemId="3752" fertileSmallSubstance="231" nonFertileSmallSubstance="232" aliasBase="141" name="Sugar Beet" />
</cropTypes>
```

## Sites

`BasePlantImp::_onDoPerform` `0x3A4550` (shared by abilities 133-140); ability-id keyed crop map global `0xB98D10`, insert fn `0x3A95C0`; "register all built-in abilities" step `0x31D040`, `_registerAbility` `0x327D20`, base-init `0x317020`, reused vtable `PlantWheat_Ability` `0x7F7A10`; substance registry lookup `0x572DE0`; Inspect "what grows here" seed-item lookup `0x3A9700` (only caller `AbilityImp::Inspect::_inspectEntity` `0x395AC0`); `SubstanceManager::RegisterSubstance` `0x572340`; harvest map (u8 substance -> item) global `0xB98D50`, get-or-default helper `0x3A94A0`; `HarvestPlant::_onDoPerform` `0x3A5030` (see [`farming.md`](farming.md)); gatherable type limit `cmp ecx,0DAh` (`81 F9 DA 00 00 00`, imm at +2) at `0x3796D8`, `0x379758`, `0x3797C8`, `0x379A00`

RVAs are named constants in `source/server/cm_offsets.h`. They are for the current Steam build of the YO dedicated server and must be re-verified after a game update.

## Notes

`maxSubstanceWidth` is optional and derived from the crops; an explicit value must be 76..155 because the engine's range check is 8-bit (`lea eax,[rbx-0x65]; cmp al,imm`), so a wider value would accept substance ids 0..100 as crops. `fertileSmallSubstance` must be 181, 191, ... (`id % 10 == 1`), `nonFertileSmallSubstance` = fertile + 1, and crops may not share a 10-wide block. `maxGatherableType` (220..254): the `isValid` helper at `0x379A00` is filtered instead of patched so the sentinel 219 stays invalid there; the three accessor limits are patched, so they do accept 219 (untested on a live server).
