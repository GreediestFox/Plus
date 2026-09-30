---
title: Knowledge base index
status: reference
domain: conventions
tags: [index, navigation, toc]
related: [llms.txt, contributing.md]
updated: 2026-06-26
---

# LiFx — Knowledge Base

LiFx is a Detours-based extension framework for the *Life is Feudal: Your Own* dedicated server (Steam appid `320850`). This `docs/` tree is the engineering knowledge base: reverse-engineering findings, framework/build references, operational runbooks, client research, and design proposals.

## Using this index

- **Humans:** scan the tables below — grouped by domain, with a status badge and one-line summary per page. New here? Read the **Start here** pages first.
- **AI agents:** read [`llms.txt`](llms.txt) first (a dense, greppable machine index of every page with tags and key symbols), and read [`contributing.md`](contributing.md) before adding or editing a page.

## Status legend

| Badge | Status | Meaning |
| --- | --- | --- |
| ✅ | verified | Confirmed against runtime / live source; trustworthy. |
| 🔬 | re | Reverse-engineered; offsets and behavior derived from analysis. |
| 📐 | design | Proposal / design contract; not yet (fully) implemented. |
| 🛠 | ops | Operational runbook; runtime-validated procedure. |
| 📖 | reference | Stable reference / convention material. |
| ⚠️ | superseded | Kept for history; a newer page supersedes it. |

## Start here

- [Architecture](architecture.md) — what LiFx is and how it is put together.
- [Conventions](conventions.md) — hook naming, file layout, the add-a-hook recipe.
- [Build](build.md) — building on Linux (clang-cl + lld-link + xwin) and the Windows path.
- [Loader and injection](loader_and_injection.md) — how LiFx gets into the server process.
- [Reverse engineering setup](reverse_engineering.md) — Ghidra setup and the analysis scripts.

## Operations & server

| Page | Status | What it covers |
| --- | --- | --- |
| [Clean dedicated-server install](server_clean_install.md) | 🛠 | Runbook: install a vanilla LiF:YO dedicated server from SteamCMD on Linux/Wine, configure its MariaDB world DB, boot, and verify. |
| [Jorvik mod install + bug fixes](jorvik_mod_install.md) | 🛠 | Runbook: install the LiF-x/Jorvik1-2 modpack (framework art.zip, DB data export, Yo Launcher) plus the nine mod bugs fixed in v2.1.0. |
| [LiF MySQL access](mysql_access.md) | 📖 | MariaDB at 127.0.0.1:3306 (NEWROOT/NEWPASS), the socket-path gotcha, `terrain_blocks` RegionID drift, and the engine-side async DB RVAs. |

## LiFx framework & build

| Page | Status | What it covers |
| --- | --- | --- |
| [Architecture](architecture.md) | 📖 | What LiFx is, source-tree map, what is wired vs. a framework hook, and the naming scheme. |
| [Loader and injection](loader_and_injection.md) | 📖 | How LiFx gets into the server process: pdh.dll proxy, deployment layout, Wine WINEDLLOVERRIDES, verification commands. |
| [Build](build.md) | 📖 | Build on Linux with clang-cl + lld-link + xwin (no Windows VM); plus the Windows `LiFx.sln` path. |

## Reverse engineering — engine internals & ABI

| Page | Status | What it covers |
| --- | --- | --- |
| [Reverse engineering setup](reverse_engineering.md) | 📖 | Ghidra setup, the `LifxExport.java` / `LifxEventVtables.java` analysis scripts, and the artifact files they produce. |
| [Reverse-engineering offset encyclopedia](offsets.md) | 🔬 | Stable RVAs and data layouts for the verified 1.4.4.5 server image: tree felling, gem drops, tunnelling rewards, and the `maxPlayers` slot limit. |
| [Farming — harvest yield and quality](farming.md) | 🔬 | `Harvest Crops` RE: the exact quantity formula (weather is a fourfold switch, fertility a twofold one) and the separate item-quality formula. |
| [NetEvent vtable layout](net_events.md) | 🔬 | Torque NetEvent vtable layout, slot semantics, and the per-event (pack, unpack, process) handler table. |
| [NetEvent receive / send path](netevent_receive_path.md) | 🔬 | The NetEvent receive/dispatch path: how an inbound event is unpacked and routed to its process handler. |
| [NetEvent ABI](netevent_abi.md) | 🔬 | Settled negative result: no per-class pack/unpack exists in any static vtable; serialization is runtime-built, so SectorHandoff must implement its own. |
| [Dispatcher wire format](dispatcher_wire_format.md) | 🔬 | YO `cmDispUnitManager` is a party/guild membership sync channel (three uint32 IDs per opcode), not a character/object handoff. |
| [NPC / Animal class hierarchy](npc_class_hierarchy.md) | ✅ | RTTI-verified Player→NPCS::Base→{AnimatedNPC→Animal} & →PlayerBased→NPCDecorative tree; native combat nodes gate on `Animals::Animal`, not AnimatedNPC. |
| [Character AI & equip-over-ghost RE](character_ai_re.md) | 🔬 | The A2a character-backed hostile NPC: NPCDecorative carries charStats/equip/death + the AI tree/move-engine, with a vtable-slot-patched packUpdate equip bolt-on. |
| [AI behaviors & spawning](ai_and_spawning.md) | ✅ | The 42-node AI behavior-tree catalog, `Animals::SpawnControl` XML system, Torque trigger callbacks, animation pipeline, and spawn recipes. |
| [Animal data model](animal_data_model.md) | 🔬 | Live `_c` world DB has no `animal_*` tables; combat is AnimalData-datablock-driven, loot is recipe/skinning, types register via `LiFx::registerObjectsTypes`. |
| [On-demand animal spawning](animal_spawn.md) | 🔬 | Spawning `Animals::Animal` via `createAnimal` (0x195FD0) + the death-to-tombstone charStats bind; all RVAs/offsets verified against live source. |
| [Bandit combat: held weapon, melee strike & stamina](bandit_combat.md) | 🔬 | Held-weapon render (`Mount_movable_object`, id 556), native `Animals::Animal::endAttack` (0x18A4D0) cone hit-scan dealing real wounds, AI-driven swing fix, and a player-calibrated stamina pool pacing the swing. |
| [Offline character load](offline_char_load.md) | 🔬 | Build a fully DB-loaded `CmCharacterInfo` offline from a bare charId (no GameConnection) via `CHARACTER_LOAD_INMEM`, on the engine main thread. |
| [Lootstone item injection](lootstone_injection.md) | 🔬 | cci/connection-gated death-loot paths and the shipped cci-free fix: SQL-move the bandit's char-container rows into its grave container, then force a reload. |
| [Character HP](character_hp.md) | 🔬 | HP architecture: what writes/propagates, dead-ends to skip, and the empirical hook-on-real-damage approach. |
| [Per-player pacifist (PvP-off) toggle](pacifist_pvp_toggle.md) | 🔬 | `Lifx::setPacifist(charID)`: generalised believer-weapon "deals no damage to other players", enforced in the `ONEPUNCHMAN` damage calc (`0x0A4BF0`) with registry-validated attacker/defender identity. Melee-confirmed, ranged-unverified. |
| [Battlezones (starting-zone land)](battlezones.md) | 🔬 | `Lands::BattleZoneLand`: `createBattleZone` is a real engine TS fn, the active-starting-zone snap-back is the real lock-in, prep-clamp is unimplemented in YO. |
| [Outposts](outposts.md) | 🔬 | Outpost/claim radius system and the proximity rules between outposts, monuments, and personal claims. |
| [Bloomery & furnace/recipe system](bloomery.md) | ✅ | Full furnace/recipe system: `recalcTick` walkthrough, the 59-row proc-descriptor table, bloomery whitelist, kiln/vostaskus cycles, and hook recipes. |
| [Craftwork working-containers (tanning tub)](craftwork_containers.md) | 🔬 | The tanning tub (type 472, `WorkingContainer`/`CmCraftworkManager`, `UseTanningTube` id 85): the last unhooked craftwork leaf, why its output type/quantity is engine-computed not data, and the RE-to-hook path. |
| [YO server engine hooks](YO_SERVER_HOOKS.md) | 🔬 | Config-driven hooks for modded *Your Own* servers: extra farmable crops and gatherable limit, wider datablock id range, greenhouse/stable aliases, herb-garden time gate, workshop quality buff, cart places and well water; RVA table and how-to. |
| [Effects & abilities](effects_and_abilities.md) | 🔬 | Effect/ability subsystem: effect-XML parser at RVA 0x4DD100, the 311 `*_Ability` RTTI classes, and hookable seams. |
| [Encrypted assets (LFXE)](dts_encryption.md) | ✅ | LFXE encrypted assets: the universal FileStream decrypt hook, the ChaCha20 container format, the key path, and the packer. |
| [Universal LFXE hook RE](lfxe_texture_re.md) | 🔬 | RE behind the universal LFXE hook (#116): FileStream as the single seam, the disproved mmap hypothesis, and both-binary offsets. |

## Client

| Page | Status | What it covers |
| --- | --- | --- |
| [Qt / QtWebEngine & HTTPS](qt_and_https.md) | 🔬 | A loose-DLL Qt 5.15.2 / Chromium 83 overlay binds against the unmodified 5.9-linked client and runs to in-world play; Qt5Network needs OpenSSL 1.1.x. |
| [Client 9x9 grid-size patch](client_grid_patch.md) | 🔬 | Verified RE record of the community `0x189554` `0x03`→`0x09` grid-clamp byte-patch — documented as a hook target only, never to be applied (no-exe-patching rule). |

## Design & proposals

| Page | Status | What it covers |
| --- | --- | --- |
| [Hostile-NPC AI path comparison](hostile-npc-ai-path-comparison.md) | 📐 | Keep NPCDecorative + custom "npcbase" nodes (Path 2, chosen) vs re-platform onto `Animals::Animal` (Path 1); the #125 tombstone/worn-loot decision. |
| [Sector handoff design](sector_handoff_design.md) | 📐 | Design contract for a new `SectorHandoff` NetEvent: wire shape, state machine, DB-ownership, DLL hook surface — verified against `cm_offsets.h` and the RE docs. |
| [Caravan event proposal](caravan_event_proposal.md) | 📐 | A server-side road-following self-defending AI cart-horse: offline A* over road tiles emits waypoints, a LiFx `WalkWaypoints` custom behavior-tree node walks them. |

## Conventions & contributing

| Page | Status | What it covers |
| --- | --- | --- |
| [Conventions](conventions.md) | 📖 | Hook naming + file-layout conventions and the step-by-step recipe for adding a new Detours hook. |
| [Contributing to the knowledge base](contributing.md) | 📖 | How to add and maintain pages: the front-matter schema, status legend, and the memory-mirror rule. |
| [Project principles & hard rules](principles.md) | 📖 | The two project-lead hard rules — never byte-patch the binaries (mod via pdh-proxy + Detours or loose `.cs`) and keep public docs to the TS-exposed API. |

## Public TorqueScript API

The `../ghdocs/docs/` mirror — these pages document only the public, TorqueScript-exposed API and mirror to **Rampart-Games-Limited/LiFxRampart**. Engine internals and the DLL name stay out of this set.

| Page | What it covers |
| --- | --- |
| [Players](../ghdocs/docs/players.md) | Read/write a player's HP, apply damage/heal, knock out, kill. |
| [Effects](../ghdocs/docs/effects.md) | Modify an active effect on a Player (expiry, extend, clear, force a HUD refresh). |
| [Timers](../ghdocs/docs/timers.md) | Override the post-respawn Resurrected debuff duration, globally or per player. |
| [Outposts](../ghdocs/docs/outposts.md) | Default/live outpost radius, production retargeting, proximity rules. |
| [NPCs](../ghdocs/docs/npcs.md) | Spawn NPCs, keep them respawned near players, bind appearance/equipment. |
| [Dispatcher](../ghdocs/docs/dispatcher.md) | Move characters / forward frames between server peers for sector handoff (experimental). |

## Conventions used throughout

- **RVA** = address relative to the exe's image base (`0x140000000` for `ddctd_cm_yo_server.exe`). All offsets in `cm_offsets.h` are RVAs.
- **"the server"** = `ddctd_cm_yo_server.exe`. **"the client"** = `LiF.exe` (or whatever the LiF Steam install names it).
- Paths in shell examples assume:
  - `/home/mjoed/LifeIsFeudal/lifxpluss` — the LiFx source.
  - `/home/mjoed/LifeIsFeudal/lif_server_320850` — the steamcmd-installed dedicated server.
  - `~/.local/share/ghidra` — Ghidra.
  - `~/.xwin` — the xwin SDK splat.
  - `/tmp/lifx_ghidra` — analysis outputs.
