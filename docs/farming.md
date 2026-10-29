---
title: Farming — harvest yield and quality
status: re
domain: reverse-engineering
tags: [farming, harvest, crops, weather, fertility]
related: [offsets.md, reverse_engineering.md, bloomery.md]
sources: [offsets.md]
updated: 2026-08-27
---

# Farming — harvest yield and quality

Reverse-engineering and analysis contributed by **Pabluuz**.

## Farming: `Harvest Crops` yield

The following RVAs apply to the 1.4.4.5 dedicated-server image. The native
implementation is `AbilityImp::HarvestPlant::_onDoPerform` from
`ability_harvestplant.cpp`.

| RVA / data RVA | Meaning |
| --- | --- |
| `+0x3A5030` | Start of `AbilityImp::HarvestPlant::_onDoPerform`. |
| `+0x3A527C` | Builds the weather-history window: `8 + stageBit0 + 2*stageBit1`, therefore 8 through 11 days. |
| `+0x3A52B0` | Calls the historical-weather getter for offsets `0, -1, -2, ...`. |
| `+0x3A52BF` | Counts weather enum `2` (`fair`). |
| `+0x3A52C8` | Counts weather enum `3` (`shower`). |
| `+0x3A52D9` | Calculates `fairCount / (showerCount + 1)`. |
| `+0x3A52F4` | Lower good-weather threshold: `0.5`. |
| `+0x3A5307` | Selects good-weather multiplier `2.0`; the fallback is `0.5`. |
| `+0x3A5358` | Starts the crop quantity calculation. |
| `+0x3A5390` | Calls `floorf` after adding `0.5`. |
| `+0x3A53A1` | Starts the harvested-item quality calculation. |
| `+0x3A53D6` | Calls `ceilf` for the final harvested-item quality. |
| `+0x3A5565` | Starts the configured regional-resource probability check; it can replace the item type but does not alter quantity. |
| `+0x471590` | Returns current or historical weather for the requested day offset. |
| `+0x87FB40` | Weather-name pointer table: `0=unknown`, `1=cloudy`, `2=fair`, `3=shower`, `4=snowy`. |

The exact quantity formula is:

```text
randomBase       = 2 + (rng & 1)              // 2 or 3
fertilityFactor  = 1 + (cropSubstance & 1)    // 2 fertile, 1 non-fertile
weatherRatio     = fairCount / (showerCount + 1)
weatherFactor    = 2.0 when 0.5 <= weatherRatio <= 2.0,
                   otherwise 0.5

quantity = floor(randomBase * fertilityFactor * weatherFactor + 0.5)
```

Mature crop substance IDs are paired in `cm_substances.cs`: odd IDs such as
`105` are `Fertile...Big`, while the following even IDs such as `106` are
`NonFertile...Big`. This is why the low bit used by the native formula is an
exact fertility multiplier.

The two stage bits are bits `0x20` and `0x40` in the geo level's
`LevelFlags`. Crop maintenance treats them as a two-bit substage counter,
incrementing it from 0 through 3 before changing the crop substance to its
next visible growth stage. Harvesting a mature crop therefore samples 8 to 11
weather days depending on its progress through the mature stage. The harvest
reader extracts the soil quality, substance, and these bits at
`+0x3AF9C0..+0x3AF9E6`; the crop-maintenance transition is visible at
`+0x26EF73..+0x26F021`.

Possible primary-stack quantities are:

| Soil state | Bad weather (`0.5`) | Good weather (`2.0`) |
| --- | ---: | ---: |
| Non-fertile | 1 or 2 | 4 or 6 |
| Fertile | 2 or 3 | 8 or 12 |

Thus weather is a hard fourfold switch before rounding, not a gradual bonus.
Fertility adds another exact twofold switch. Farming skill and luck are not
read by the quantity calculation; the skill configuration affects ability
duration, while the level-100 luck bonus only gives a chance to reduce farming
ability duration.

Wheat and flax also create a second stack with the same quantity and quality:
Wheat (`352`) adds Straw (`362`), and Flax stem (`361`) adds Flax Seeds
(`1030`). The other crops create only the primary stack. The ability's
configured 5% regional-resource roll changes the resource variant, not its
amount.

Harvested-item quality is separate from quantity:

```text
quality = ceil((1 - 0.2 * weatherFactor) * soilQuality
               + 20 * weatherFactor)
```

For good weather this is `ceil(0.6 * soilQuality + 40)`; for bad weather it is
`ceil(0.9 * soilQuality + 10)`. Soil quality therefore changes item quality,
not the number of items.

See also: [`YO_SERVER_HOOKS.md`](YO_SERVER_HOOKS.md) - hooks that add extra farmable crops on the same sow/grow/harvest code and raise the gatherable type limit.
