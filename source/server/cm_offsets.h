#pragma once

/* ===================================================================================
	Copyright (c) 2026, LiFx Contributors. All Rights Reserved.

	This file is a part of LiFx.

	LIFX IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
	EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
	MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
	IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
	DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
	ARISING FROM, OUT OF OR IN CONNECTION WITH LIFX OR THE USE OR OTHER
	DEALINGS IN LIFX.
*  =================================================================================== */

#include "core/cm_aux.h"

enum CmOffset : U32
{
	CON_INTERNAL_PRINTF = 0x405090,
	CON_ADD_CONSTANT = 0x406680,
	CON_EVALUATE = 0x406A50,
	CON_GET_VARIABLE = 0x4077B0,
	CON_INIT = 0x407990,
	CON_LOOKUP_NAMESPACE = 0x4082B0,
	CON_SET_VARIABLE = 0x408CD0,
	CON_ADD_INT_COMMAND = 0x410F90,
	CON_ADD_FLOAT_COMMAND = 0x411000,
	CON_ADD_STRING_COMMAND = 0x411070,
	CON_ADD_VOID_COMMAND = 0x4110E0,
	CON_ADD_BOOL_COMMAND = 0x411150,
	CON_ADD_VARIABLE = 0x411360,
	STRING_TABLE_INSERT = 0x441BF0,

	// Stream* openFileStream(pathObj* /*rcx*/, U32 accessMode /*edx*/)
	// Server twin of the client's CLIENT_OPEN_FILE_STREAM (0x61E620):
	// allocates a FileStream (0x2040 bytes, vtable RVA 0x871AE8), calls
	// open() (vtable slot 12 / off 0x60), returns the opened Stream* in
	// rax. The DSO loader exec() (RVA 0x40B2B0) validates the returned
	// stream (getStreamSize()>=0xC, version==0x2750), so a decrypted
	// MemStream substituted here makes both checks pass. Hook seam for
	// transparent *.cs.dso decryption (issue #114).
	//
	// SUPERSEDED by the FILESTREAM_* hooks below (issue #116): only one of 5
	// FileStream constructors, so it missed direct FileStream::open callers.
	OPEN_FILE_STREAM = 0x44A2C0,

	// --- FileStream class methods (issue #116) --------------------------------
	// One FileStream class; every loose-file read funnels through these. The
	// universal LFXE hook (hooks/engine/hook_filestream.cpp) detours them so
	// all assets decrypt at the stream layer, subsuming the factory hook.
	// FileStream vtable RVA 0x871AE8; slots verified by disassembly (see
	// docs/lfxe_texture_re.md).
	//
	//   bool  FileStream::open(this, const char* path, U32 accessMode)  [slot 12]
	//   bool  FileStream::_read(this, U64 size, void* dst)              [slot 2]
	//   U64   FileStream::getPosition(this)                             [slot 6]
	//   bool  FileStream::setPosition(this, U64 pos)                    [slot 7]
	//   U64   FileStream::getStreamSize(this)                           [slot 8]
	//   void  FileStream::~FileStream(this, U32 flags)                  [slot 0]
	FILESTREAM_OPEN     = 0x44A4F0,
	FILESTREAM_READ     = 0x449B40,
	FILESTREAM_GETPOS   = 0x44A470,
	FILESTREAM_SETPOS   = 0x44A6F0,
	FILESTREAM_GETSIZE  = 0x44A490,
	FILESTREAM_DTOR     = 0x449A50,

	// WorkingFurnace process-descriptor lookup. Hooked by
	// Hooks::Furnace::ProcDescLookup so LiFx can override which row of
	// DAT_140ACFA60 applies to an item type, enabling new furnace recipes
	// without binary patching. See docs/bloomery.md.
	FURNACE_PROC_DESC_LOOKUP = 0x1DB7C0,

	// Per-class craftwork tick functions — vtable slot 3 of each working-*
	// class. Hooked individually because each engine class has its own
	// hardcoded kind-switch; to add a brand-new kind value to (say) brewing
	// tanks you need to hook BrewingTankFurnace's slot 3 specifically. See
	// docs/bloomery.md §"recalcTick — full walkthrough" for the WorkingFurnace
	// breakdown; the others follow the same shape.
	WORKING_FURNACE_RECALC_TICK     = 0x1DCFF0,   // bloomery / kiln / smelter
	BREWING_TANK_RECALC_TICK        = 0x1DB370,   // brewing tank / fermenter
	WORKING_FIRE_RECALC_TICK        = 0x1DB650,   // campfire / hearth (trivial predicate)
	WORKING_GREENHOUSE_RECALC_TICK  = 0x1DEA70,   // greenhouse plant growth
	WORKING_TRAP_RECALC_TICK        = 0x1DEE90,   // animal traps
	WORKING_WINDMILL_RECALC_TICK    = 0x1DFF20,   // windmill flour grinding (trivial state update)

	// Ability id 268 "Light the Fire" -- AbilityImp::LightWorkingObject::_onDoPerform.
	// RE'd 2026-09-11/12: resolves the target ComplexObject_Entity (at self+0x38),
	// then calls ::Engine::LightWorkingObject-equivalent logic (entity+0x384 timer,
	// stateAlt = ServerTime_Now()+duration at +0x28 of a lazily-created WorkingFire).
	// See source/server/hooks/furnace/engine_internals.h's LightWorkingObject().
	LIGHTWORKINGOBJECT_ONDOPERFORM  = 0x3A21F0,

	// AbilityImp::ServerManager::_registerPerform -- the single generic
	// dispatch point the engine calls for EVERY ability perform. RE'd
	// 2026-09-12 via its own diagnostic string. Used as an observation probe
	// to find which concrete class REALLY backs ability id 268, since
	// LIGHTWORKINGOBJECT_ONDOPERFORM above was confirmed by live testing to
	// never fire for it. See source/server/hooks/ability/hook_register_perform.cpp.
	SERVERMANAGER_REGISTERPERFORM   = 0x3B5D80,

	// Light-component resolver called by LightOn::_onDoPerform with an
	// ALREADY __RTDynamicCast'd ComplexObject_Entity* -- NOT the raw self+
	// 0x38 pointer. Hooked as an observation probe to capture the real,
	// correctly-cast value live (a raw/uncast pointer fed to this function
	// directly crashed a "invalid super-type" engine error, confirmed
	// 2026-09-12). See source/server/hooks/ability/hook_resolve_light_object.cpp.
	LIGHTON_RESOLVE_LIGHT_OBJECT    = 0x33DDB0,

	// BrewingTankFurnace's own process-descriptor lookup at DAT_140ACF8D0.
	// Distinct from the WorkingFurnace lookup hooked above — brewing has its
	// own table. See docs/bloomery.md "Craftwork class family — full coverage".
	BREWING_TANK_PROC_DESC_LOOKUP   = 0x1DAAE0,

	// CharacterVitalParameters::Process_tick — the per-frame HP/Stamina
	// processor. DEAD: this is the abstract base's vtable slot and is never
	// called; the concrete subclass overrides it at an address we haven't
	// identified. Kept for reference; the hook on it is harmless (never fires).
	VITALPARAMS_PROCESS_TICK        = 0x097BC0,

	// CharacterParameters::Calc_hit_damage_distribution — called every time
	// the engine processes a real combat hit on a character. We hook this
	// empirically to observe what writes HP during natural damage. See
	// docs/character_hp.md for the investigation plan. (Earlier note about
	// breaking fall damage was wrong — fall damage still applies with the
	// hook enabled.)
	CHAR_CALC_HIT_DAMAGE            = 0x091A50,

	// CmCharacterWounds::dealDamage — looks like a damage entry point but the
	// runtime "dealDamage(<part>)" log we see is emitted by an LTO-inlined
	// copy of the body. Hooking this address is verified to patch a 0xE9 but
	// never fires; left in place to keep the symbol mapped, do not rely on it.
	WOUNDS_DEAL_DAMAGE              = 0x1C63E0,

	// "ONEPUNCHMAN" damage-calculator (FUN_1400A4BF0). Emits the 5 ONEPUNCHMAN
	// log lines we see on every melee hit. Too large to be inlined, so this
	// IS the real combat path. Its 2nd argument is an output struct filled
	// with the computed damage amounts (hard/soft HP in 1e6 scale) — the
	// caller applies them after we return, which means a post-return hook
	// can mutate the struct to inject arbitrary damage.
	ONEPUNCHMAN_DAMAGE_CALC         = 0x0A4BF0,

	// CmCharacterInfo::_sendChanges — the server→client broadcast for a
	// character's state. Signature `(self, uint32 mask, char sendNow)`.
	// Hooking this with logging tells us (a) which mask bits fire on a
	// real hit and (b) whether HP-shaped fields (+0x194 HardHP, +0x19C
	// SoftHP) change adjacent to the call. If a specific mask bit
	// correlates with HP changes, that's the bit the HUD listens on.
	CHARINFO_SEND_CHANGES           = 0x1BC3D0,

	// Player::_applyHit — the orchestrator the engine calls when a real
	// combat hit lands. Confirmed via decompile of FUN_1400ee0f0; error
	// strings reference "x:\dev\cm_clone\cm_yo_release\engine\source\t3d\
	// player.cpp" "Player::_applyHit". This function calls
	// Calc_hit_damage_distribution, then ONEPUNCHMAN, then dealDamage,
	// then FUN_140090f60 (HIT_APPLY_DAMAGE). Hooking this tells us when
	// a real hit is being processed end-to-end.
	PLAYER_APPLY_HIT                = 0x0EE0F0,

	// Suspected "apply damage to live HP + broadcast". Called from
	// Player::_applyHit at offset +0x511 with `(charStatsSubObj, &dmgPacket)`.
	// dmgPacket contains hardHpDamage (int64) at +0x00 and softHpDamage
	// (int64) at +0x08, both in 1e6 scale. If this function is the HP-apply
	// step, hooking it will fire on every real hit AND any manual call we
	// make to it will move the HUD.
	HIT_APPLY_DAMAGE                = 0x090F60,

	// A2a #125 equip-over-ghost bolt-on. The shared ShapeBase/Player
	// packUpdate (vtbl slot 54). NPCDecorative reaches it via thunk
	// 0x2E54C0; Player reaches it too — so the hook GATES on the object's
	// vtable (== NPCDEC_VTABLE) and only appends the custom equip block for
	// NPCDecorative. Signature `(this, NetConnection* conn, U64 mask,
	// BitStream* stream)` returns U32 retMask. Verified: reads charStats
	// [this+0xAA8], writeFlag/writeInt the standard fields.
	SHAPEBASE_PACKUPDATE            = 0x0FC8B0,
	// BitStream::writeFlag(rcx=stream, dl=bool) -> returns the bool written.
	// Append the equip block after the stock pack. (writeInt=0xA3F10,
	// readInt=0xA2DF0.) BitStream: +0x10 buf, +0x18 bitPos, +0x20 capBytes,
	// +0x28 overflow; bits LSB-first.
	BITSTREAM_WRITEFLAG             = 0x0A3E80,
	// NPCDecorative primary vtable (RVA). Gate: *(void**)this == base+this.
	NPCDEC_VTABLE                   = 0x7E3D08,
	// PROPER equip hook: patch the NPCDecorative vtable's packUpdate SLOT
	// instead of Detours-prologue-patching the shared impl (0x0FC8B0) — the
	// shared-prologue detour raced world-load worker threads and hung the
	// server. Slot 54 (NetObject::packUpdate slot) lives at +0x1B0 in the
	// vtable and holds the NPC-only thunk 0x2E54C0 (jmp -> 0x0FC8B0). We swap
	// that one pointer; only NPCDecorative instances use this vtable, so the
	// gate is structural (Players/items never reach our code). The thunk RVA
	// is used ONLY as an install-time sanity check (refuse to patch if the
	// slot doesn't match the verified layout).
	NPCDEC_PACKUPDATE_SLOT         = 54,
	NPCDEC_PACKUPDATE_THUNK        = 0x2E54C0,

	// Default-outpost-radius getter. The decompile is literally
	//     ulonglong FUN_140187360(void) { return 0x14; }
	// i.e. it just returns the constant 20. It's called from
	// Lands::DB::CreateOutpostLandAndClaim (RVA 0x2BA8B0) to populate the
	// third argument of `call p_createOutpostLandAndClaim(%u, %u, %u);`,
	// the radius column of the new row in `guild_lands`. Hooking it lets
	// LiFx serve any uint we want as the default radius for newly-built
	// outposts. See docs/outposts.md.
	OUTPOST_DEFAULT_RADIUS_GETTER   = 0x187360,

	// Lands::Manager::changeGuildLandRadius. Live-changes an existing
	// outpost's radius: validates, persists via
	//     UPDATE guild_lands SET Radius=%u WHERE ID=%u;
	// then rebuilds the monument with the new outer radius. Not hooked
	// yet — needs the Lands::Manager singleton resolved first. Documented
	// here so the offset is tracked for the follow-up. See docs/outposts.md.
	OUTPOST_CHANGE_GUILD_LAND_RADIUS = 0x2D0F00,

	// Lands::DB::CreateOutpostLandAndClaim. The function whose decompile
	// shows `call p_createOutpostLandAndClaim(%u, %u, %u);` — guildID,
	// centerGeoID, radius. The third arg is loaded from a call to
	// OUTPOST_DEFAULT_RADIUS_GETTER above, which is our leverage point.
	OUTPOST_CREATE_LAND_AND_CLAIM   = 0x2BA8B0,

	// `Lands::Manager*` singleton storage (a `Manager**` slot in .data).
	// Read via `*(Lands::Manager**)(image_base + LANDS_MANAGER_SINGLETON)`.
	// Confirmed via cross-reference: `guildLandsMaintenance` (0x2C0B00) and
	// `initialLoadGuildLandsFromDB` (0x2D6220) are both invoked through
	// this global; the maintenance tick itself calls
	// `changeGuildLandRadius(*singleton, ...)` so the slot is unambiguously
	// the right one. Used to construct the `this` for the live-radius path.
	LANDS_MANAGER_SINGLETON         = 0xB62F68,

	// `Outposts::Manager*` singleton storage. Same shape as above —
	// dereference at runtime. Confirmed: `Outposts::Manager::startMaintenance`
	// writes `*(uint64_t*)(image_base + 0xB5AD68) = new Manager()`, and the
	// container-mutating `_addObject` / `_findObject` are reached via reads
	// from this slot.
	OUTPOSTS_MANAGER_SINGLETON      = 0xB5AD68,

	// `Outposts::Manager::_findObject(this, ComplexObjectID*)`. Used to map
	// a UnmovableObjectID (or rather its packed ComplexObjectID form) to
	// the in-memory `Outpost*` so we can call setProductionType on it.
	OUTPOSTS_MANAGER_FIND_OBJECT    = 0x2ED0C0,

	// `Outposts::Outpost::setProductionType(this, uint32 typeID)`. Writes
	// the new type into the in-memory object and emits an
	// `UPDATE outposts SET ProductionObjectTypeID=%u WHERE UnmovableObjectID=%u;`
	// so the change is persisted by the engine itself.
	OUTPOST_SET_PRODUCTION_TYPE     = 0x2E8A10,

	// "Monument minimum distance" getter — `return 150;`. The single
	// hardcoded threshold for guild-monument ↔ guild-monument spacing inside
	// `Lands::Manager::canBuildMonumentAt`. Detour-friendly: the function
	// body is one instruction, same shape as OUTPOST_DEFAULT_RADIUS_GETTER.
	MONUMENT_MIN_DISTANCE_GETTER         = 0x2B1EF0,

	// "Outpost-vs-outpost (and outpost-vs-guildland) minimum distance"
	// getter — `return 300;`. Read in both `canBuildOutpostAt` and the
	// vs-outpost branch of `canBuildMonumentAt`. Tiny return-const fn.
	OUTPOST_OUTPOST_MIN_DISTANCE_GETTER  = 0x2B1F00,

	// ---- Battlezones (Lands::BattleZoneLand, handle tag 4). See docs/battlezones.md. ----
	// Lands::Manager::createBattleZoneLand(type, geoIdInt, radius, name).
	// Builds a 0x38-byte BattleZoneLand, stores it in the per-type list at
	// (Lands::Manager* + 0x2F0), and posts a CreateBattleZoneLandChange for
	// client sync. Reached by the TS console fn `createBattleZone`.
	CREATE_BATTLEZONE_LAND          = 0x2CB8A0,
	// Lands::Manager::deleteBattleZoneLand(landDbId). Reached by TS `deleteBattleZone`.
	DELETE_BATTLEZONE_LAND          = 0x2CBAB0,
	// Lands::Manager::isActiveStartingZone(landHandle) -> bool. The containment
	// gate: returns true iff tag==4 && subtype(+0x30)==1 && active(+0x34)!=0.
	// Sole caller is Player::_checkSteps; when true the engine snaps a
	// stepping-out player back via Player::teleportTo.
	ISACTIVE_STARTING_ZONE          = 0x2D78C0,
	// Player::_checkSteps(Player*). Per-step movement handler; we detour it
	// only to stamp the "player currently being checked" so the
	// isActiveStartingZone detour can apply per-player exemptions.
	PLAYER_CHECK_STEPS              = 0x0F0500,
	// BattleZoneLand field accessors used by printBattleZones (verified there):
	//   id     = *(u32*)( GetIdHolder(land)+4 )
	//   geoId  = *(u32*)  GetGeoId(land+0x18, &out)
	//   radius =          GetRadius(land+0x18)
	// (type = *(u32*)(land+0x30), active = *(u8*)(land+0x34) are read direct.)
	BATTLEZONE_LAND_GET_ID          = 0x2B2BB0,
	BATTLEZONE_LAND_GET_GEOID       = 0x2B1C70,
	BATTLEZONE_LAND_GET_RADIUS      = 0x2B1C80,

	// Two call instructions inside the binary that we *retarget* (rewrite
	// the rel32 of the existing `E8 …` call) so they invoke a wrapper inside
	// LiFx instead of the engine's `FUN_140187360` (return 20). The default
	// getter at 0x187360 is shared with many other call sites we want left
	// alone, so we patch only these two:
	//
	//   CALL_SITE_PERSONAL_LAND_OUTPOST_DIST — inside
	//   `Lands::Manager::checkPersonalLandOutpostDistance` (RVA 0x2D1A70).
	//   Sole call to 0x187360 inside that function. Drives the
	//   outpost ↔ personal-claim min-distance check (failure emits cm_messages id 748).
	//
	//   CALL_SITE_MONUMENT_PERSONAL_DIST — inside
	//   `Lands::Manager::canBuildMonumentAt` (RVA 0x2CF9D0). Sole call to
	//   0x187360 inside that function. Drives the guild-monument ↔
	//   personal-claim min-distance check (failure emits cm_messages id 756).
	//
	// Both sites are 5-byte `E8 rel32` direct calls — confirmed via Ghidra
	// disassembly. Bytes at 0x2D1C65: `E8 F6 56 EB FF`. Bytes at 0x2CFE45:
	// `E8 16 75 EB FF`. The other call to 0x187360 inside `canBuildOutpostAt`
	// (sites 0x2D0375 footprint, 0x2D0853 vs guild-yo) is intentionally left
	// untouched so it continues to track Lifx::setOutpostDefaultRadius.
	CALL_SITE_PERSONAL_LAND_OUTPOST_DIST = 0x2D1C65,
	CALL_SITE_MONUMENT_PERSONAL_DIST     = 0x2CFE45,

	// Effect XML parser — `FUN_1404dd100`. Sole function that references 25
	// of the 26 distinct parameter-type/applytype tokens that appear in
	// data/cm_effects.xml (SPEED, HARD_HP_MAX, CONSUME, INCREASE_COEFF, …).
	// Identified by string-fan-in analysis in
	// scripts/ghidra/LifxEffectsScan.java. No-arg, no-return per its
	// decompile (24KB stack frame; first action is `LEA RCX,[RSP+0x70];
	// CALL <ctor>` — builds a parser scratch object). Fires once during
	// server boot. Hooked as the first proof-of-life seam for the
	// effect/ability subsystem; see docs/effects_and_abilities.md.
	EFFECT_PARSE                         = 0x4DD100,

	// cObjEffects::Assign_effect — engine's per-row apply/remove sink for
	// the active-effect manager on each Player. Decompile path noted as
	// `engine\source\objeffects\objeffects.cpp` line ~0x23f. Called by the
	// ObjEffectsEvent::process handler (option 3, per-row delta) and
	// directly by gameplay code that adds/removes effects. Signature:
	//   void __fastcall(cObjEffects* this, uint32_t effectID, uint32_t* row16)
	// where row16 points to {u32 expires_at, u32 applied_at, u64 magnitude}.
	// Hooked to enforce a configurable Resurrected (id=47) duration; see
	// hook_assign_effect.h. Located via #32's vtable + delta-shape trace.
	OBJEFFECTS_ASSIGN_EFFECT             = 0x4DC810,

	// Player::BroadcastEffectDelta — virtual method at vtable slot 187 on
	// Player and the 6 NPC classes. Constructs an ObjEffectsEvent (opt=3)
	// from the supplied delta list and broadcasts it to every NetConnection
	// ghosting this object. Called by gameplay code whenever an effect is
	// applied/refreshed/cleared on the server side. Signature:
	//   bool __fastcall(void* player, void* unused, DeltaVec* deltaList)
	// where DeltaVec = {begin, end, end_of_storage} of 32-byte entries.
	// Mapped in #32; this is the right chokepoint for #34 because the
	// server's effect-apply path writes Player+0x1238 directly and then
	// calls THIS function — Assign_effect is only invoked by clients
	// receiving the resulting net event.
	OBJEFFECTS_BROADCAST_DELTA           = 0xEBFF0,

	// NetClassRep::add — single-line head insert into the global classRep
	// list (head at DAT_140BC00B0, next-ptr at +0x50 on the rep). Every
	// NetEvent / SimObject class self-registers here during static init.
	// Hooked behind `<dumpNetClassRep>1</dumpNetClassRep>` so the dumper
	// can snapshot each rep's first 0x80 bytes — used to recover the
	// pack/unpack/factory fn-pointer offsets. See docs/netevent_abi.md
	// and hook_netclassrep_dumper.h (issue #54).
	NETCLASSREP_ADD                      = 0x418C40,

	// ServerUUIDEvent::send factory (FUN_1404E7370). Allocates a 0x48-byte
	// event with a UUID at +0x40, then posts via `(*(*conn[+0x1F8])[0])(...)`.
	// Hooked solely so the dumper can capture the sink object at
	// NetConnection+0x1F8 and walk its vtable on first invocation.
	SERVERUUIDEVENT_SEND                 = 0x4E7370,

	// GameConnection::setControlObject(ShapeBase*). RE'd via the unique
	// log string "GameConnection::setControlObject() -- set controlling
	// client -- %s[%u] %u" xref (issue #99). Hooked to capture the live
	// Player* per connection without relying on the broken
	// FindPlayerByScan + Player+0x1B44 charID-stamp assumption.
	SET_CONTROL_OBJECT                   = 0x137D40,

	// AI behavior-tree node factory (issue #119). The factory is a lazy
	// singleton (AI_GET_NODE_FACTORY) holding XML-class-name -> prototype
	// INode*; the engine clones a prototype for every <node class="..."/>.
	// We register our custom prototype from the per-tree XML loader hook
	// (AI_LOAD_BEHAVIOR_XML) — it runs after the built-in node modules are
	// registered (so the "Stopped" clone template exists) and before each
	// tree's nodes are parsed, on both the boot load path (onServerCreated)
	// and the manual reloadBehaviorXml() console command (which calls it per
	// tree). See source/server/hooks/ai/ABI_NOTES.md for the full RE.
	AI_LOAD_BEHAVIOR_XML                 = 0x153B80,   // void* __fastcall(self, const char* file); hook trigger
	AI_GET_NODE_FACTORY                  = 0x153860,   // void* __fastcall()
	AI_REGISTER_NODE                     = 0x153950,   // void __fastcall(factory, name, INode** proto)
	AI_CREATE_BY_NAME                    = 0x153760,   // void* __fastcall(factory, INode** out, name)
	AI_TIXML_ATTRIBUTE                   = 0x45A920,   // const char* __fastcall(TiXmlElement*, const char* name, int 0)

	// ================================================================================
	// A2a character-backed hostile NPC (issue #125).
	//
	// An NPCS::NPCDecorative / NPCS::PlayerBased carries BOTH the player character
	// layout (charStats @ +0xAA8) AND the AI layout (tree @ +0x24B8, move-engine @
	// +0x24C0) — so it can be AI-driven AND use the engine's own equip-render +
	// death->corpse->worn-loot pipelines. Class chain:
	//   Player(0x0E87B0) -> NPCS::Base(0x2E2C10) -> PlayerBased(0x2E5210,
	//   vtbl 0x7E5638) -> NPCDecorative(0x2E46B0). Object size 0x2518.
	// Equip-render + worn-loot are free IFF the NPC's charStats yields a non-null
	// CmPlayerEquipment via the registry accessor below (else the engine logs
	// "player %u has null CmPlayerEquipment" and silently skips — not a crash).
	// See memory project_a2a_character_ai_re + the RE report on issue #125.

	NPCDECORATIVE_CREATE                 = 0x2E4AB0,   // NPCDecorative* __fastcall() — malloc(0x2518)+ctor+registerObject(vtbl+0x50=0x4304A0)
	NPCDECORATIVE_CTOR                   = 0x2E46B0,   // void __fastcall(self)
	NPCS_BASE_CTOR                       = 0x2E2C10,   // void __fastcall(self) — writes charStats@+0xAA8, tree@+0x24B8, engine@+0x24C0
	SET_BEHAVIOR                         = 0x2E4850,   // void __fastcall(creature, const char* treeName) — getBehaviorTree + TREE_ATTACH
	GET_BEHAVIOR_TREE                    = 0x150D30,   // void* __fastcall(behaviorMgr, void** out, const char* name); mgr singleton @ BEHAVIOR_MGR_SINGLETON
	TREE_ATTACH                          = 0x2E38D0,   // void __fastcall(creature, void** tree) — installs tree at creature+0x24B8
	BEHAVIOR_MGR_SINGLETON               = 0xB7BEB8,   // module-base-relative ptr to the BehaviorsManager singleton
	// AiTree::process(rcx = tree-ptr-at-creature+0x24B8) -> int node status. Thunk:
	// `mov rcx,[rcx+0x40]; jmp 0x153130` (runs the root node). This is exactly what
	// Animal::packUpdate (0x18B450) calls to tick the tree; SAFE on the main/pack
	// thread, guarded on tree != null. NPCDecorative's own pack DOESN'T call it
	// (slaves stay static), so we tick it ourselves for AI-enabled NPCs.
	AI_TREE_PROCESS                      = 0x154020,
	NPC_AI_TREE_OFF                      = 0x24B8,     // creature+0x24B8 = behaviour-tree ptr
	NPC_MOVE_ENGINE_OFF                  = 0x24C0,     // creature+0x24C0 = move-engine ptr
	ANIMATED_NPC_SET_ANIMATION           = 0x2E2A90,   // void __fastcall(self, nameHandle, u8 flag) — logs "can't find animation %s" + sets seq by index; #145 anim-remap hook target

	// #145 Animal-derived hostile (RE'd via Ghidra; see reference_animal_spawn_re memory):
	CREATE_ANIMAL                        = 0x195FD0,   // Animals::Manager::createAnimal: Animal* __fastcall(mgr, u32 typeId, u32 quality, int id[0=auto], u8 flag)
	ANIMALS_MANAGER_GLOBAL_RVA           = 0xB80C90,   // module-rel ptr to the Animals::Manager singleton (createAnimal arg1)
	ANIMAL_GET_DATABLOCK_BY_TYPE         = 0x18C790,   // AnimalData::GetDatablockByTypeID(u32 typeId) -> AnimalData* (typeId hash map)
	ANIMAL_VTABLE                        = 0x798AA0,   // Animals::Animal primary vtable (observed live)
	SIMOBJECT_ID_OFF                     = 0x90,       // SimObject mId (u32), assigned by registerObject 0x4304A0

	// #171 equip-over-ghost on the native Animal (clothing render for bandits).
	// Same slot index as NPCDecorative (54 = NetObject::packUpdate), but on the
	// Animal vtable the slot holds Animal::packUpdate (0x18B450), the class's OWN
	// pack (not a thunk). Verified by RTTI walk of ANIMAL_VTABLE (COL.offset==0)
	// + slot scan: vtbl[54] == 0x18B450. The Animal vtable is shared by ALL
	// wildlife, so OnAnimalPackUpdate gates the append per-instance (marker bit =
	// IsHostile). The fn RVA is an install-time sanity check (refuse if mismatch).
	ANIMAL_PACKUPDATE_SLOT               = 54,
	ANIMAL_PACKUPDATE_FN                 = 0x18B450,   // Animals::Animal::packUpdate (ANIMAL_VTABLE[54])

	// #145 death->tombstone redirect (RE'd 2026-06-25, see reference_animal_spawn_re):
	ANIMAL_CREATE_CORPSE                 = 0x18A370,   // Animals::Animal::createCorpse(this) — THE carcass chokepoint (reads datablock+0x8478, creates carcass, removes from mgr)
	ANIMAL_MGR_REMOVE                    = 0x196850,   // Animals::Manager remove(mgr, animalId) — createCorpse's tail cleanup
	ANIMAL_MGR_ID_OFF                    = 0x2540,     // animal+0x2540 = manager animal id (arg to ANIMAL_MGR_REMOVE)
	CHARSTATS_DEATH_TRIGGER_SLOT         = 0x130,      // charStats.vtbl[+0x130] = Player death->lootstone+worn-loot trigger; engine calls (charStats, 0) in death router 0x3BE890

	CREATE_TEST_CHARACTER                = 0x1D29B0,   // U32 __fastcall(U32 accountId, U32 charId) — wraps Character::Create; full DB-side char (f_createInventory/f_createEquipment/f_insertNewItemInventory/p_allocate_equipment_slots + INSERT character)

	// #145 Step 2 — worn-loot tombstone: load the bound char's inventory+equipment
	// IN MEMORY so the death loot-resolver (FUN_140090ab0) finds them. The DB rows
	// (containers, starting items, equipment_slots) are already created by
	// CreateTestCharacter -> Character::Create. RE'd 2026-06-25, see reference_animal_spawn_re.
	CM_INVENTORY_PLAYER_INIT             = 0x2900B0,   // CmInventoryPlayer::init(record, cci) — allocs CmPlayerEquipment@record+0x48 + inventory@record+0x40, builds root container, calls CmPlayerEquipment::initialize
	EQUIP_LOAD_FROM_DB                   = 0x1F2760,   // CmPlayerEquipment::loadFromDb(equip) — populates slots from SQL equipment_slots (needs equip+0x50 player_id + equip+0x58 container)
	MOUNT_MOVABLE_OBJECT                 = 0x0EBA30,   // Player::Mount_movable_object(param1 = obj+0xAA8 charStats, U32 movableTypeId): resolves ShapeBaseImageData by typeId (FUN_140120b80) then obj->mountImage (vtbl+0x3C0). cci-free held-weapon render; bad typeId logs "Can't find ShapeBaseImageData for movable TypeID=%u". #154
	// #154 movable-image (ShapeBaseImageData) registry — the hash the resolver FUN_140120b80
	// reads. Keyed by ShapeBaseImageData *datablock id* (NOT WeaponData id / item ObjectTypeID).
	// Node layout: [0]=next, +0x8=key(u32 id), +0x10=value(ShapeBaseImageData*). Dump it live
	// (Lifx::dumpMovableImages) to learn valid sword ids. typeId=5 was rejected => not a real id.
	MOVABLE_IMG_BUCKETS_PTR              = 0xB6FB10,   // DAT_140b6fb10: ptr to bucket array (each = node* or null)
	MOVABLE_IMG_BUCKET_COUNT            = 0xB6FB18,   // DAT_140b6fb18: number of buckets (hash modulus)

	// #154 STRIKE — native animal melee attack chain (cci-free). The bandit's no-damage
	// cause: the AI swing (ANIMAL_SWING) only plays the Attack_Fast/Power animation; the
	// real hit (ANIMAL_END_ATTACK) is normally fired by the per-tick gate Animals 0x18BD30
	// when the DTS attack-sequence trigger marker hits — but male.dts has no such marker,
	// so endAttack never runs. We call the chain directly. All operate on the Animal `this`.
	ANIMAL_SWING                         = 0x18B950,   // FUN_14018b950(this, int attackType): plays "Attack_Fast"(0)/"Attack_Power"(1) via setAnimation; sets attackType@+0x24f8, consumed-flag@+0x24fc=0. Visual only.
	ANIMAL_END_ATTACK                    = 0x18A4D0,   // Animals::Animal::endAttack(this): native melee resolve — cone hit-scan (range@datablock+0x84c0, angle@+0x84c4) over nearby Players, builds WeaponData hit descriptor (dmg @+0x84c8/+0x84cc), applies via victim->vtbl[+0x350] (ServerCombatHitEvent). Reads WeaponData* @this+0x24f0 (set at spawn by onNewDataBlock from AnimalData+0x8488); -1/missing => logs "animal %s without weapon tries to attack". Indexes datablock by attackType@+0x24f8 (keep 0/1).
	ANIMAL_WEAPONDATA_OFF                = 0x24F0,     // this+0x24f0: WeaponData* used by endAttack (populated at spawn). u64.
	ANIMAL_ATTACKTYPE_OFF                = 0x24F8,     // this+0x24f8: attack-type index (0=fast,1=power); indexes datablock attack params — keep in range to avoid OOB read.
	ANIMAL_ATTACK_CONSUMED_OFF          = 0x24FC,     // this+0x24fc: per-swing "hit consumed" byte (endAttack sets 1; swing clears 0).

	CHARACTER_LOAD_INMEM                 = 0x1BB290,   // CharacterLoad(cci) — full connect-time load: CharacterParameters::loadFromDb (stats+RootContainerID/EquipmentContainerID) + CmInventoryPlayer::init + inventory loadFromDb + CmPlayerEquipment::loadFromDb (+ HP DB sync); arg = CmCharacterInfo*, reads charId @ cci+0x358. Connection-FREE — verified 2026-06-25.
	// Offline cci construction (no GameConnection): mirror FUN_14028b050's alloc.
	//   refc = ENGINE_ALLOC(0x3a8); *refc = base+CCI_REFCOUNT_VFTABLE_RVA;
	//   *(u32*)(refc+8)=1; *(u32*)(refc+0xC)=1; cci = refc+0x10;
	//   CCI_FIELD_INIT_CTOR(cci, charId);   // stores charId @ cci+0x358
	//   CHARACTER_LOAD_INMEM(cci);          // full DB load
	IS_MAIN_THREAD                       = 0x407EF0,   // bool __fastcall() — true iff caller is the engine main thread (caches first caller's _Thrd_id). The cci ctor's signal/TLS registration is main-thread-only; console cmds run on a worker thread, so marshal via schedule().
	ENGINE_ALLOC                         = 0x6DF950,   // void* __fastcall(size_t) — engine operator new / allocator
	CCI_FIELD_INIT_CTOR                  = 0x1B9C50,   // CmCharacterInfo field-init ctor(cci, u32 charId): sets vftable, zeroes fields, stores charId @ cci+0x358. No DB access.
	CCI_REFCOUNT_VFTABLE_RVA             = 0x7CFD98,   // std::_Ref_count_obj<CmCharacterInfo>::vftable (stored at *refc of the 0x3a8 ref-count block)
	CCI_CHARID_OFF                       = 0x358,      // CmCharacterInfo + 0x358 = charId (read by CharacterParameters::loadFromDb)
	CCI_RECORD_OFF                       = 0x370,      // CmCharacterInfo + 0x370 = record (== cci[0x6e])
	RECORD_INVENTORY_OFF                 = 0x40,       // record+0x40 = CmInventoryPlayer*
	RECORD_EQUIP_OFF                     = 0x48,       // record+0x48 = CmPlayerEquipment*
	RECORD_EQUIP_REFC_OFF                = 0x50,       // record+0x50 = _Ref_count_obj<CmPlayerEquipment>*
	EQUIP_PLAYER_ID_OFF                  = 0x50,       // CmPlayerEquipment+0x50 = CharacterID (player_id)
	EQUIP_CONTAINER_OFF                  = 0x58,       // CmPlayerEquipment+0x58 = root/slot container (CmEquipmentContainer)
	EQUIP_SLOT_BASE_OFF                  = 0x100,      // container+0x100 + slot*8 = slot's itemId (slots 1..0x11); written by _setSlot 0x1EE930
	EQUIP_SET_SLOT_HIGH_FN               = 0x1F38D0,   // setSlot(equip, u8 slot, u64 itemId(+skinId hi32), int) — validates+writes slot-DB+persists+fires EquipTriggerSignal (== EQUIP_SET_SLOT_HIGH)

	// CORRECTION (verified by disasm of the real call site 0x140087045): 0x28BD30
	// is NOT a charStats hash-map lookup. It is a GENERIC registry lookup on the
	// global type/registry manager singleton at *0x140B53908 (the same global
	// engine_internals.h uses), keyed by a value obtained from a VIRTUAL CALL on
	// the entity (vtable slot 1), with rdx = a caller-provided 16-byte out buffer:
	//   key = entity->vtbl[1](entity);
	//   accessor(*0x140B53908 /*rcx*/, &out16 /*rdx*/, key /*r8d*/);
	// Passing charStats as arg1 (the earlier assumption) indexes a garbage bucket
	// count and CRASHES. The exact equipment-resolution (which key the loot path
	// 0x1F2DA0 uses, and the handle->CmPlayerEquipment chain) still needs a clean
	// static RE pass before this is called again. CHARSTATS_EQUIP_KEY_OFF (0xB14)
	// was WRONG and is removed.
	EQUIP_ACCESSOR                       = 0x28BD30,   // void* __fastcall(void* registryGlobal /*=*0x140B53908*/, void* out16, U32 key)
	EQUIP_REGISTRY_GLOBAL_RVA            = 0xB53908,   // module-rel ptr to the registry/type-manager singleton (accessor arg1)
	EQUIP_CAN_SET_SLOT                   = 0x1F01D0,   // validates object/slot before a write
	EQUIP_APPLY_SLOT_CHANGES             = 0x1F00B0,   // commits slot changes; drives the mesh-hide recompute
	EQUIP_SET_SLOT_DB                    = 0x1EEA40,   // persists a slot to SQL equipment_slots
	EQUIP_SET_DEFAULT_MESHES_HIDDEN      = 0x1F0660,   // recomputes the visible/culled body-mesh set (fixes all-armor render)
	EQUIP_SET_SLOT_HIGH                  = 0x1F38D0,   // high-level slot mutate (used by the loot-transfer path)

	SPAWN_LOOTSTONE                      = 0x102570,   // Player vtbl slot 44 — corpse container; loot transfer in 0x102770 -> 0x1F2DA0
	ON_DEATH_HAPPENS                     = 0x0FB390,   // Player vtbl slot 48 — death orchestrator (shared by NPCDecorative)

	// #145 Step 2 — cci-free worn-loot via direct SQL. The engine's own async DB
	// exec primitives, recovered from CharacterParameters::Zed_is_dead (0x89660),
	// which uses them for the `chars_deathlog` INSERT:
	//   conn = DB_GET_WORLD_CONN(idx)  — idx 1 = the world-DB connection object
	//          (returns (&DAT_140BF86F0)[idx]; idx>4 -> null). Verified 2026-06-25.
	//   DB_EXEC_FORMATTED(conn, fmt, &u32a, &u32b) does
	//          snprintf(buf, n, fmt, *a, *b); then conn->vtbl[0](conn, buf) to run it.
	//          ALWAYS derefs a/b, so pass two valid U32 ptrs even when fmt has no
	//          specifiers (pre-format the SQL yourself; keep '%' out of it).
	// Used by Lifx::dropBanditLoot to move a dead bandit's char-container items into
	// its grave's container (the cci-gated transfer the engine skips for a
	// connection-less bandit). See reference_lootstone_injection.
	DB_GET_WORLD_CONN                    = 0x54CAF0,   // void* __fastcall(U32 idx); idx 1 = world DB conn
	DB_EXEC_FORMATTED                    = 0x0884F0,   // U8 __fastcall(void* conn, const char* fmt, const U32* a, const U32* b)

	// CmServerInventoryContainer::tryInit (loadFromDb) — populates an in-memory
	// container from the DB: `SELECT ... FROM items WHERE ContainerID=mID`, builds
	// each item via createItemFromDbResult and adds it (vtbl slot 0x90). Guarded by
	// the "already-initialized" flag at +0x14 (0 => it loads; nonzero => no-op), so
	// the container loads ONCE and caches. Object layout (verified in the decomp):
	//   +0x14 init guard (u8)   +0x18 mID (u32)   +0x1c ObjectTypeID (u32, grave=1070)
	//   +0x20 ParentID (u32)    +0x24 Quality (u16)
	// #145 worn-loot: a freshly-created grave's container is tryInit'd EMPTY (items
	// still on the dead char), so the SQL move is invisible until reload. We hook
	// this to capture the grave (type 1070) container pointer, then after the move
	// clear +0x14 and call it again to repopulate the in-mem container live.
	CONTAINER_TRYINIT                    = 0x299140,   // U64 __fastcall(CmServerInventoryContainer* container)
};