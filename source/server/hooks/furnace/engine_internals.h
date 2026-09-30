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

/*
	Engine-internal function pointers + offset constants used by the LiFx
	reimplementations of craftwork tick functions.

	These are *not* hook targets — they're the engine functions our
	reimplementations CALL into to do the same work the engine does. Each
	pointer is lazy-resolved by RVA on first use; thread-safe via C++11
	function-local statics.

	Naming convention (matches docs/conventions.md):
	  - Free helpers in the engine get a flat name reflecting their semantics
	    (ServerTime_Now, Type_HasParent, etc.).
	  - The RVA literal in each accessor's static initializer is the single
	    source of truth — if a future LiF patch shifts offsets, all uses
	    update by editing one line per function.

	Add new entries here as you reimplement more craftwork ticks. Group by
	subsystem; alphabetize within a group.
*/

#include "core/cm_aux.h"

#include <mutex>

namespace Engine
{
	// ---- module base -----------------------------------------------------
	inline uintptr_t ModuleBase() {
		static const uintptr_t base = (uintptr_t)GetModuleHandle(nullptr);
		return base;
	}

	template <typename Fn>
	inline Fn AtRva(uintptr_t rva) {
		return reinterpret_cast<Fn>(ModuleBase() + rva);
	}

	template <typename T>
	inline T* DataAtRva(uintptr_t rva) {
		return reinterpret_cast<T*>(ModuleBase() + rva);
	}

	// ============================================================================
	// SERVER TIME
	// ============================================================================
	// Server-wide "now" timestamp in engine ticks. Used by every craftwork
	// recalcTick as the "current time" reference.
	typedef unsigned (__fastcall *pfn_serverTime_now)();
	inline unsigned ServerTime_Now() {
		static const auto fn = AtRva<pfn_serverTime_now>(0x5147A0);
		return fn();
	}

	// Day/night clock: 64-bit microsecond counter behind a world/time singleton
	// pointer. Unrelated to ServerTime_Now() above -- do not mix the two units.
	// Verified live 2026-06-15 via the TimeOfDayBetween/IsNight AI nodes
	// (hooks/ai/VERIFICATION.md); duplicated here as a shared wrapper so
	// non-AI hooks (e.g. WorkingFire) don't have to re-derive it.
	inline int GameTime_CurrentHour() {
		constexpr uintptr_t kWorldPtrRva   = 0xB7E4C0;
		constexpr unsigned  kFieldOff      = 0x10;
		constexpr long long kMicrosPerHour = 3'600'000'000LL;

		char* obj = *reinterpret_cast<char**>(ModuleBase() + kWorldPtrRva);
		if (!obj)
			return -1;
		const long long t = *reinterpret_cast<long long*>(obj + kFieldOff);
		long long h = (t / kMicrosPerHour) % 24;
		if (h < 0) h += 24;
		return static_cast<int>(h);
	}

	// ============================================================================
	// LIGHT SOURCES (LightWorkingObject ability chain -- RE'd 2026-09-11)
	// ============================================================================
	// Real mechanism behind ability id 268 "Light the Fire"
	// (AbilityImp::LightWorkingObject::_onDoPerform, RVA 0x3A21F0): lazily
	// creates/finds a WorkingFire-typed timer object at (entity+0x384) and
	// writes ServerTime_Now()+durationMs to its +0x28 stateAlt field -- the
	// SAME mechanism WorkingFire::recalcTick reads from, just reached
	// on-demand (ability-triggered) rather than via a continuous per-tick
	// call. recalcTick never fired for any placed light source in live
	// testing (2026-09-11), even an actively-used, freshly-relit campfire;
	// this on-demand path is the one the engine itself actually uses.
	//
	// The first argument is a runtime-initialized event-type id
	// (DAT_140b84e20) -- it reads as 0 in the static image, so it's almost
	// certainly set up by an engine init routine before ever being used.
	// Read it fresh on every call rather than caching, to match how the
	// engine's own call site (_onDoPerform) uses it.
	typedef void (__fastcall *pfn_light_working_object)(uint32_t eventType, void* target, int durationMs);
	inline void LightWorkingObject(void* entity, int durationMs) {
		static const auto fn = AtRva<pfn_light_working_object>(0x1D9B20);
		const uint32_t eventType = *DataAtRva<uint32_t>(0xB84E20);
		void* target = static_cast<char*>(entity) + 0x384;
		fn(eventType, target, durationMs);
	}

	// ============================================================================
	// LIGHT ON (decorative torch/lamp toggle -- ability id 268, RE'd 2026-09-12)
	// ============================================================================
	// The REAL mechanism behind ability id 268 "Light/Put Out the Fire" for
	// DECORATIVE light sources (wall torch id 2500, candle id 2502, braziers).
	// `LightWorkingObject` above (RVA 0x3A21F0) was the WRONG class for this --
	// confirmed by empirically capturing the concrete ability object's own
	// vtable pointer live (hook_register_perform.cpp filters ability id==268
	// and logs *(void**)self) and resolving its slot-3 override in Ghidra:
	// the real class is `AbilityImp::LightOn`, `_onDoPerform` at RVA 0x3A1EE0
	// (confirmed via its own embedded strings -- "LightOn::_onDoPerform() -
	// Bad entity" / "- Can't find entity object" / "- Can't find game
	// connection"). Unlike LightWorkingObject/WorkingFire (a burn-timer with a
	// ServerTime_Now()-based expiry), LightOn has NO timer at all -- it's a
	// plain state flag:
	//
	//   lightObj = ResolveLightObject(entity)   // ref-counted component lookup
	//   state    = *(int*)(lightObj + 0x380)    // 1 = off, 2 = on (empirical:
	//                                            // LightOn::_onDoPerform toggles
	//                                            // exactly between these two)
	//   SetState(lightObj, newState, force=true)
	//
	// SetState (RVA 0xCB7E0) also flips the network-dirty flag and (when the
	// object is already "live", bit 3 of +0x58 set) triggers the render
	// follow-up that does the model swap + smoke/sound loop -- calling it
	// directly reproduces exactly what performing the ability in-game does,
	// no separate visual-update step needed.
	//
	// REFCOUNTING WARNING: ResolveLightObject (RVA 0x33DDB0) increments a
	// strong reference (dual strong+weak counter, same LOCK/UNLOCK-guarded
	// pattern used everywhere in this binary -- see e.g. the teardown code in
	// AbilityImp::ServerManager::_registerPerform) that the engine's own
	// caller (LightOn::_onDoPerform) always releases afterward. LightOn_
	// ForceState below does NOT release it -- each call leaks one strong ref.
	// That's an accepted, deliberate tradeoff for manual/interactive TESTING
	// only (Lifx::forceLightOn console command) where calls are rare. Do NOT
	// wire this into a periodic per-object dusk-activation loop without first
	// implementing the matching release (decrement at lightObj+8; if the
	// PRE-decrement value was 1, call the vtable dtor at slot 0, then repeat
	// at +0xc against slot 1) -- looping this over every placed torch every
	// day/night cycle would leak unboundedly otherwise.
	typedef void* (__fastcall *pfn_lighton_resolve)(void* entity, void* outBuf2Qwords);
	inline void* LightOn_ResolveLightObject(void* entity, void* outBuf2Qwords) {
		static const auto fn = AtRva<pfn_lighton_resolve>(0x33DDB0);
		return fn(entity, outBuf2Qwords);
	}

	typedef int (__fastcall *pfn_lighton_get_state)(void* lightObj);
	inline int LightOn_GetState(void* lightObj) {
		static const auto fn = AtRva<pfn_lighton_get_state>(0xC9920);
		return fn(lightObj);
	}

	typedef void (__fastcall *pfn_lighton_set_state)(void* lightObj, int state, char force);
	inline void LightOn_SetState(void* lightObj, int state, bool force) {
		static const auto fn = AtRva<pfn_lighton_set_state>(0xCB7E0);
		fn(lightObj, state, force ? 1 : 0);
	}

	constexpr int kLightOn_Off = 1;
	constexpr int kLightOn_On  = 2;

	// Testing-only convenience -- see the refcounting warning above. `entity`
	// must already be the RTTI-cast ComplexObject_Entity* (same as what
	// LightOn::_onDoPerform itself resolves at self+0x38). Returns false if
	// the entity has no resolvable light component.
	inline bool LightOn_ForceState(void* entity, int state) {
		unsigned long long buf[2] = { 0, 0 };
		void* outBuf = LightOn_ResolveLightObject(entity, buf);
		void* lightObj = *reinterpret_cast<void**>(outBuf);
		if (!lightObj)
			return false;
		LightOn_SetState(lightObj, state, true);
		return true;
	}

	// ============================================================================
	// RTTI DYNAMIC CAST (MSVC __RTDynamicCast thunk, RVA 0x6E0FF2)
	// ============================================================================
	// The FIRST live test of LightOn_ForceState fed it a raw, un-cast pointer
	// (the ability object's self+0x38 field directly) and crashed the server
	// with "Complex::ServerObjectStorage::_getObjectMapUnsafe() - invalid
	// super-type" (confirmed live 2026-09-12) -- because LightOn::_onDoPerform
	// itself always RTTI-casts (__RTDynamicCast) that field to
	// ComplexObject_Entity* before calling LightOn_ResolveLightObject, and we
	// had skipped that step. This wraps the same compiler-emitted cast helper
	// (thunks to vcruntime140.dll) so callers do it correctly. Standard
	// dynamic_cast semantics apply: returns nullptr on a failed/incompatible
	// cast instead of crashing, AS LONG AS the input is a genuinely live
	// object with an intact vtable (true for anything TorqueScript itself
	// resolved for us, e.g. a GameBase-scoped console command's `obj`
	// argument -- NOT true for a hand-typed/garbage raw address, which is
	// exactly what caused the original crash).
	//
	// Signature matches the compiler's own call sites in LightOn::
	// _onDoPerform / AddSomeFuel::_onDoPerform:
	//     void* __RTDynamicCast(void* ptr, long vfDelta, void* srcTypeDesc,
	//                            void* targetTypeDesc, int isReference);
	typedef void* (__fastcall *pfn_rtdynamiccast)(void* ptr, long vfDelta, void* srcType, void* targetType, int isReference);
	inline void* RTDynamicCast(void* ptr, void* srcTypeDescriptor, void* targetTypeDescriptor) {
		static const auto fn = AtRva<pfn_rtdynamiccast>(0x6E0FF2);
		return fn(ptr, 0, srcTypeDescriptor, targetTypeDescriptor, 0);
	}

	// Converts a live engine object pointer into the ComplexObject_Entity*
	// LightOn_ResolveLightObject/LightWorkingObject actually require.
	//
	// IMPORTANT: uses SimObject::RTTI_Type_Descriptor (RVA 0xAF0DB8) as the
	// SrcType, NOT GameBase::RTTI_Type_Descriptor (RVA 0xAF1818) despite the
	// function's name/original use case (a GameBase-scoped console command's
	// `obj`). __RTDynamicCast is only well-defined when the asserted SrcType
	// is a genuine ancestor of the object's REAL runtime type -- true for
	// GameBase when `obj` is a Player, but NOT necessarily true for other
	// candidates (e.g. static/decorative props) a broader object search might
	// return. Asserting GameBase as SrcType on a non-GameBase-derived object
	// is undefined behavior, not a safe null-return, and crashed the live
	// server (confirmed 2026-09-12, second crash of the dusk-sweep scan
	// work). SimObject is the true root of every class Torque's own object
	// hierarchy print showed (`Player -> DynamicShape -> ShapeBase ->
	// GameBase -> ... -> NetObject -> SimObject`), so it's the ancestor type
	// safe to assert for ANY object handed to us by the engine's own object/
	// container system -- Player included, since SimObject is still a real
	// ancestor of Player too.
	//
	// RTTI descriptor addresses found via Ghidra symbol search 2026-09-12:
	//   SimObject::RTTI_Type_Descriptor           RVA 0xAF0DB8
	//   ComplexObject_Entity::RTTI_Type_Descriptor RVA 0xB05D78
	inline void* GameBaseToComplexEntity(void* obj) {
		void* srcType = DataAtRva<void>(0xAF0DB8);
		void* dstType = DataAtRva<void>(0xB05D78);
		return RTDynamicCast(obj, srcType, dstType);
	}

	// ============================================================================
	// CONTAINER RADIUS SEARCH (RVA 0x51A490 / 0x51A480) -- the object-
	// enumeration primitive the eventual dusk-sweep needs. Already proven live
	// in production by hooks/ai/hook_behavior_node.cpp's ScanNearestPlayer (NOT
	// duplicated here byte-for-byte since that copy is file-local/anonymous-
	// namespace -- this is the same technique, generalized).
	//
	// initContainerRadiusSearch(centerXYZ, radius, typeMask, useClient) seeds a
	// PROCESS-GLOBAL, NON-REENTRANT search singleton; containerSearchNext()
	// repeatedly called afterward returns each matching object in turn, then
	// nullptr once exhausted. Caller MUST fully drain it (or at least not
	// interleave a second search) -- see hook_behavior_node.cpp's g_scanMtx for
	// why any new caller needs the same serialization discipline.
	//
	// TYPE MASK VALUES -- read directly out of the engine's own object-type
	// registration table (RVA 0xAC6E38, 24-byte rows: {namePtr, unknownPtr,
	// bitValue}), NOT assumed from generic Torque documentation, since this
	// fork's bit assignments have already been shown to NOT match vanilla
	// Torque numbering (see the kPlayerObjectType=0x8000 discrepancy noted
	// below). Confirmed via Ghidra 2026-09-12:
	//   MarkerObjectType        = 0x80
	//   LightObjectType         = 0x100   <- candidate bit for placed light sources
	//   ZoneObjectType          = 0x200
	//   StaticShapeObjectType   = 0x400   <- candidate bit for decorative static props (torches?)
	//   DynamicShapeObjectType  = 0x800
	//   GameBaseObjectType      = 0x1000
	//   GameBaseHiFiObjectType  = 0x2000
	//   ShapeBaseObjectType     = 0x4000
	//   CameraObjectType        = 0x8000  <- NOTE: hook_behavior_node.cpp's
	//                                         kPlayerObjectType constant is ALSO
	//                                         0x8000 -- either that constant is
	//                                         mislabeled (really Camera, not
	//                                         Player) or Player objects also set
	//                                         the Camera bit; unresolved, flagged
	//                                         here rather than silently trusted.
	//                                         Table's own PlayerObjectType = 0x20000.
	//   PlayerObjectType        = 0x20000 (per table; the working AI code uses
	//                                         0x8000 instead -- see note above)
	// Not yet empirically confirmed WHICH of Light/StaticShape/DynamicShape (or
	// some OR combination) actually matches a placed torch's real typemask --
	// that's exactly what Lifx::scanNearbyLights (lifx_effects.cpp) is for.
	constexpr unsigned kTypeMask_Light        = 0x100;
	constexpr unsigned kTypeMask_StaticShape  = 0x400;
	constexpr unsigned kTypeMask_DynamicShape = 0x800;

	typedef void  (__fastcall *pfn_radius_init)(void* posXyz, float radius, unsigned mask, bool useClient);
	typedef void* (__fastcall *pfn_radius_next)();
	inline void ContainerRadiusInit(const float posXyz[3], float radius, unsigned typeMask, bool useClient = false) {
		static const auto fn = AtRva<pfn_radius_init>(0x51A490);
		fn(const_cast<float*>(posXyz), radius, typeMask, useClient);
	}
	inline void* ContainerRadiusNext() {
		static const auto fn = AtRva<pfn_radius_next>(0x51A480);
		return fn();
	}

	// SHARED lock for the container radius search above. The search is a
	// single process-global, non-reentrant singleton -- hooks/ai/
	// hook_behavior_node.cpp's AI tick (SetNearestPlayerAsTarget/ChaseTarget/
	// FleeFromTarget) calls it constantly in the background via this SAME
	// accessor. A FIRST version of the dusk-sweep diagnostic scan
	// (Lifx::scanNearbyLights) called ContainerRadiusInit/Next directly with
	// NO lock at all (that file's own private g_scanMtx doesn't help unless
	// EVERY caller uses the exact same mutex instance) and crashed the live
	// server almost immediately -- confirmed 2026-09-12, the AI tick thread
	// was racing the console command against this same global state. ANY new
	// caller of ContainerRadiusInit/Next MUST hold this lock for the full
	// init-then-drain sequence, matching hook_behavior_node.cpp's own
	// discipline (which now locks this same accessor, not a private mutex).
	inline std::mutex& ContainerSearchMutex() {
		static std::mutex m;
		return m;
	}

	// Generic SceneObject-family field offsets, reused across several
	// independent RE efforts (hook_behavior_node.cpp for Pos/SimId,
	// client/hook_naked_render.cpp for TypeMask) -- centralized here so a
	// third user (the dusk-sweep scan) doesn't have to re-derive them.
	namespace ObjOff
	{
		constexpr unsigned PosX     = 0x284;
		constexpr unsigned PosY     = 0x294;
		constexpr unsigned PosZ     = 0x2A4;
		constexpr unsigned SimId    = 0x90;
		constexpr unsigned TypeMask = 0x1B0;
	}
	inline void ObjPos(void* obj, float& x, float& y, float& z) {
		x = *reinterpret_cast<float*>(static_cast<char*>(obj) + ObjOff::PosX);
		y = *reinterpret_cast<float*>(static_cast<char*>(obj) + ObjOff::PosY);
		z = *reinterpret_cast<float*>(static_cast<char*>(obj) + ObjOff::PosZ);
	}
	inline uint32_t ObjSimId(void* obj) {
		return *reinterpret_cast<uint32_t*>(static_cast<char*>(obj) + ObjOff::SimId);
	}
	inline uint32_t ObjTypeMask(void* obj) {
		return *reinterpret_cast<uint32_t*>(static_cast<char*>(obj) + ObjOff::TypeMask);
	}

	// ============================================================================
	// TYPE SYSTEM
	// ============================================================================
	// DAT_140B53908 = pointer to the global ObjectTypeManager singleton. The
	// hasParent / getTypeById helpers below take an offset of (+8) into this
	// manager to reach the actual type table. The +8 is convention from the
	// engine; we keep it here to match the decompile literally.
	inline void* TypeManagerWithChildOffset() {
		return *DataAtRva<void*>(0xB53908) ? reinterpret_cast<char*>(*DataAtRva<void*>(0xB53908)) + 8 : nullptr;
	}

	// Returns 1 if `typeInfo` is a descendant of `targetTypeId` within `maxDepth`
	// ParentID hops; otherwise 0. Hook target lives at 0x27EB30 — but we're
	// CALLING the engine's implementation here, not the hooked version.
	typedef int (__fastcall *pfn_type_hasParent)(void* typeInfo, int targetTypeId, int maxDepth);
	inline int Type_HasParent(void* typeInfo, int targetTypeId, int maxDepth = 100) {
		static const auto fn = AtRva<pfn_type_hasParent>(0x27EB30);
		return fn(typeInfo, targetTypeId, maxDepth);
	}

	// Resolve a typeId to its Type* via the global manager. NULL if unknown.
	typedef void* (__fastcall *pfn_type_getById)(void* mgr, int typeId);
	inline void* Type_GetById(int typeId) {
		static const auto fn = AtRva<pfn_type_getById>(0x27CA00);
		return fn(TypeManagerWithChildOffset(), typeId);
	}

	// ============================================================================
	// INVENTORY ITERATOR (shared across all craftwork recalcTicks)
	// ============================================================================
	// The engine constructs a 72-byte iterator on the stack, walks the contents
	// of a container, and uses the *_Inv_* helpers below to mutate slot state.
	// In the decompiles these appear as FUN_14029B750, FUN_14029C8A0, etc.
	typedef void  (__fastcall *pfn_inv_begin)(void* iterator, void* containerCtx);
	typedef char  (__fastcall *pfn_inv_valid)(void* iterator);
	typedef void  (__fastcall *pfn_inv_finish)(void* iterator);
	typedef void  (__fastcall *pfn_inv_teardown)(void* iterator);
	typedef void  (__fastcall *pfn_inv_advance_progress)(void* iterator, void* slot, unsigned amount);
	typedef void  (__fastcall *pfn_inv_advance_alt)(void* iterator, void* slot, unsigned amount);
	typedef void  (__fastcall *pfn_inv_set_quality)(void* iterator, void* slot, unsigned q);
	typedef void  (__fastcall *pfn_inv_abort)(void* iterator, void* slot);

	inline void Inv_Begin(void* iterator, void* containerCtx) {
		static const auto fn = AtRva<pfn_inv_begin>(0x29B750);
		fn(iterator, containerCtx);
	}
	inline char Inv_Valid(void* iterator) {
		static const auto fn = AtRva<pfn_inv_valid>(0x29C8A0);
		return fn(iterator);
	}
	inline void Inv_Finish(void* iterator) {
		static const auto fn = AtRva<pfn_inv_finish>(0x29C600);
		fn(iterator);
	}
	inline void Inv_Teardown(void* iterator) {
		static const auto fn = AtRva<pfn_inv_teardown>(0x29BBE0);
		fn(iterator);
	}
	inline void Inv_AdvanceProgress(void* iterator, void* slot, unsigned amount) {
		static const auto fn = AtRva<pfn_inv_advance_progress>(0x29D950);
		fn(iterator, slot, amount);
	}
	inline void Inv_AdvanceAlt(void* iterator, void* slot, unsigned amount) {
		static const auto fn = AtRva<pfn_inv_advance_alt>(0x29DBD0);
		fn(iterator, slot, amount);
	}
	inline void Inv_SetQuality(void* iterator, void* slot, unsigned q) {
		static const auto fn = AtRva<pfn_inv_set_quality>(0x29DD30);
		fn(iterator, slot, q);
	}
	inline void Inv_Abort(void* iterator, void* slot) {
		static const auto fn = AtRva<pfn_inv_abort>(0x29DAA0);
		fn(iterator, slot);
	}

	// ============================================================================
	// FURNACE / BREWING HELPERS
	// ============================================================================
	typedef int (__fastcall *pfn_furnace_get_temp)(void* self);
	typedef void (__fastcall *pfn_furnace_set_temp)(void* self, int t);
	inline int Furnace_GetTemperature(void* self) {
		static const auto fn = AtRva<pfn_furnace_get_temp>(0x1D5D40);
		return fn(self);
	}
	inline void Furnace_SetTemperature(void* self, int t) {
		static const auto fn = AtRva<pfn_furnace_set_temp>(0x1D5D50);
		fn(self, t);
	}

	// Fetches the container view associated with the furnace (param_1+0x0C).
	typedef void (__fastcall *pfn_furnace_get_contents)(void* self, void* outContainer);
	inline void Furnace_GetContents(void* self, void* outContainer) {
		static const auto fn = AtRva<pfn_furnace_get_contents>(0x1DF4B0);
		fn(self, outContainer);
	}

	// Visitor — invokes the supplied callback for each... something, per furnace.
	// Used by Greenhouse for the plant-growth walk.
	typedef void (__fastcall *pfn_furnace_visit)(void* self, void* callbackObj);
	inline void Furnace_Visit(void* self, void* callbackObj) {
		static const auto fn = AtRva<pfn_furnace_visit>(0x1DF4F0);
		fn(self, callbackObj);
	}

	// Windmill helper called by WorkingWindmill::recalcTick. Takes no args and
	// touches global state (likely "advance the grindstone counter"). Engine
	// label unknown; we keep the FUN_-style name to be honest.
	typedef void (*pfn_windmill_helper)();
	inline void Windmill_TickHelper() {
		static const auto fn = AtRva<pfn_windmill_helper>(0x1DFB20);
		fn();
	}

	// ============================================================================
	// DESCRIPTOR LOOKUPS (hook targets — calling the originals via RVA gives
	//                    us the engine's unmodified table walk)
	// ============================================================================
	// NB: these duplicate the LiFx trampolines used in hook_proc_desc.cpp /
	// hook_brewing_tank_desc.cpp, but going through the trampolines would
	// re-enter our own hooks. From a reimplementation we want the engine's
	// original lookup behavior, not the hooked one — so we call by RVA.
	typedef void* (__fastcall *pfn_desc_lookup)(void* itemTypeInfo);
	inline void* Furnace_OriginalLookup(void* itemTypeInfo) {
		static const auto fn = AtRva<pfn_desc_lookup>(0x1DB7C0);
		return fn(itemTypeInfo);
	}
	inline void* BrewingTank_OriginalLookup(void* itemTypeInfo) {
		static const auto fn = AtRva<pfn_desc_lookup>(0x1DAAE0);
		return fn(itemTypeInfo);
	}

	// Alternative brewing lookup keyed by a different row field (+0x18 vs +0x00).
	inline void* BrewingTank_OriginalLookupAlt(void* itemTypeInfo) {
		static const auto fn = AtRva<pfn_desc_lookup>(0x1DAB60);
		return fn(itemTypeInfo);
	}

	// ============================================================================
	// CONSTANTS (read from .rdata)
	// ============================================================================
	inline float MaxTickDt()     { return *DataAtRva<float>(0x737008); }   // 1.0f clamp
	inline float TickRateUnits() { return *DataAtRva<float>(0x73AB78); }   // dt -> "progress units" multiplier

	// ============================================================================
	// CHARACTER LOOKUPS / STATE
	// ============================================================================
	// CmServer global singleton — holds object types, character info, and many
	// other manager-shaped collections. DAT_140B53908 is its instance pointer.
	inline void* CmServer() {
		return *DataAtRva<void*>(0xB53908);
	}

	// Look up CmCharacterInfo* by character ID. The engine signature is
	//     FUN_14028BC20(mgr, out**, charId, flag);
	// where `out` receives the CmCharacterInfo* or NULL if not found.
	typedef void (__fastcall *pfn_get_character_info)(void* mgr, void** out,
	                                                   uint32_t charId, int flag);
	inline void* Character_GetByID(uint32_t charId) {
		static const auto fn = AtRva<pfn_get_character_info>(0x28BC20);
		void* out = nullptr;
		fn(CmServer(), &out, charId, 0);
		return out;
	}

	// Persist a CmCharacterInfo's HardHP and SoftHP to the DB. Engine fn at
	// 0x1BB290 — synchronously enqueues the UPDATE.
	typedef void (__fastcall *pfn_persist_hp)(void* charInfo);
	inline void Character_PersistHp(void* charInfo) {
		static const auto fn = AtRva<pfn_persist_hp>(0x1BB290);
		fn(charInfo);
	}

	// Mark a CmCharacterInfo as dirty (via bitmask of changed fields) and
	// optionally broadcast a state-update event to the owning client.
	//
	//   mask     — bitmask of which fields changed (1<<statType for primary
	//              stats; HP uses higher bits whose exact assignment we
	//              haven't enumerated, so passing 0xFFFFFFFF forces a full
	//              resync which is wasteful but reliable).
	//   sendNow  — 0 to just OR the bitmask into the dirty field (deferred
	//              broadcast on the next aggregated send), 1 to broadcast
	//              immediately.
	//
	// Engine fn at 0x1BC3D0 (CmCharacterInfo::_sendChanges).
	typedef void (__fastcall *pfn_send_changes)(void* charInfo, unsigned mask, char sendNow);
	inline void Character_SendChanges(void* charInfo, unsigned mask, char sendNow) {
		static const auto fn = AtRva<pfn_send_changes>(0x1BC3D0);
		fn(charInfo, mask, sendNow);
	}

	// HP is stored as a scaled int32 at +0x194: internal_value = display * 1e6.
	// Confirmed against DAT_140737AB80 (the engine's scale-double constant).
	constexpr int kHpScale = 1000000;

	// On the Player's character-stats sub-object (= charStats, hooked as
	// Process_tick.self / Calc_hit_damage.self / HitApplyDamage.charStats):
	//
	//     Player + 0xAA8  = charStats   (so charStats - 0xAA8 = Player)
	//     Player + 0x1B44 = charID (uint32, derived from Player::_applyHit
	//                              decompile lines 597, 716, 717)
	//
	// Therefore from any charStats pointer:
	//     charID = *(uint32*)(charStats + 0x109C)
	//
	// where 0x109C = 0x1B44 - 0xAA8. Verified empirically — this is what the
	// charID→charStats registry in hook_vital_process_tick uses.
	constexpr unsigned kCharStatsToPlayerDelta = 0xAA8;
	constexpr unsigned kCharIdOffOnCharStats   = 0x109C;

	// HP is a TRIPLET, not a single field. From the process_tick decomp at
	// RVA 0x97BC0 (death-gate at line 313):
	//
	//     if ((param_1[0x27] - param_1[0x23]) + param_1[0x24] < 1)
	//         vtable[0xa8](self);   // death virtual
	//
	// i.e. hard HP effective = (+0x138 - +0x118 + +0x120). The HUD reads
	// effective, not +0x118 alone. Soft HP is the parallel triplet
	// (+0x218 - +0x1F8 + +0x200) — see kSoft*FieldOff below.
	//
	// +0x118 happens to drift opposite the HUD for hard HP, which is why
	// the "HUD = floor(-field/1e6) + 1" shortcut worked there empirically.
	// That shortcut is brittle — it falls apart when wound state moves
	// +0x138 or +0x120. Long-term we should compute HUD from the triplet.
	constexpr unsigned kHardHpDamageOff   = 0x118;   // damage term (subtracted)
	constexpr unsigned kHardHpBonusOff    = 0x120;   // bonus term (added)
	constexpr unsigned kHardHpEffMaxOff   = 0x138;   // effective max term
	constexpr unsigned kHardHpFieldOff    = 0x118;   // legacy alias for the damage term
	constexpr unsigned kHardHpMaxFieldOff = 0x110;   // nominal max HP (Constitution cap)
	inline long long HardHpRaw(void* charStats) {
		return *reinterpret_cast<long long*>(static_cast<char*>(charStats) + kHardHpFieldOff);
	}
	inline long long HardHpMaxRaw(void* charStats) {
		return *reinterpret_cast<long long*>(static_cast<char*>(charStats) + kHardHpMaxFieldOff);
	}
	// Empirical: HUD hard HP = floor(-field/1e6) + 1 (1-based "minimum
	// alive = 1" display), capped at max_display.
	inline long long HardHpDisplay(void* charStats) {
		const long long internal   = -HardHpRaw(charStats) / kHpScale;
		const long long maxRaw     = HardHpMaxRaw(charStats);
		const long long maxDisplay = (maxRaw > 0) ? (maxRaw / kHpScale) : 105;
		const long long hud        = internal + 1;
		return hud > maxDisplay ? maxDisplay : hud;
	}

	// Soft HP triplet (process_tick line ~100):
	//     soft_effective = (+0x218 - +0x1F8 + +0x200)
	// Same shape as the hard triplet. Direct writes to +0x1F8 don't
	// budge the HUD because effective is a sum of three terms — writing
	// one to a "negated HP" value just makes the subtracted term large
	// and effective gets clamped at max. To MOVE the HUD you have to
	// either preserve the triplet invariant or update +0x218 / +0x200.
	constexpr unsigned kSoftHpDamageOff   = 0x1F8;   // damage term (subtracted)
	constexpr unsigned kSoftHpBonusOff    = 0x200;   // bonus term (added)
	constexpr unsigned kSoftHpEffMaxOff   = 0x218;   // effective max term
	constexpr unsigned kSoftHpFieldOff    = 0x1F8;   // legacy alias
	constexpr unsigned kSoftHpMaxFieldOff = 0x1F0;   // nominal max soft HP
	inline long long SoftHpRaw(void* charStats) {
		return *reinterpret_cast<long long*>(static_cast<char*>(charStats) + kSoftHpFieldOff);
	}
	inline long long SoftHpMaxRaw(void* charStats) {
		return *reinterpret_cast<long long*>(static_cast<char*>(charStats) + kSoftHpMaxFieldOff);
	}
	// Empirical: HUD soft HP = floor(-field/1e6) (no offset), capped at max.
	inline long long SoftHpDisplay(void* charStats) {
		const long long internal   = -SoftHpRaw(charStats) / kHpScale;
		const long long maxRaw     = SoftHpMaxRaw(charStats);
		const long long maxDisplay = (maxRaw > 0) ? (maxRaw / kHpScale) : 105;
		return internal > maxDisplay ? maxDisplay : internal;
	}

	// Triplet field accessors — read the three terms the engine uses
	// internally for the effective-HP / death-check calculation.
	inline long long ReadI64At(void* base, unsigned off) {
		return *reinterpret_cast<long long*>(static_cast<char*>(base) + off);
	}
	inline long long HardHpEffective(void* cs) {
		return ReadI64At(cs, kHardHpEffMaxOff)
		     - ReadI64At(cs, kHardHpDamageOff)
		     + ReadI64At(cs, kHardHpBonusOff);
	}
	inline long long SoftHpEffective(void* cs) {
		return ReadI64At(cs, kSoftHpEffMaxOff)
		     - ReadI64At(cs, kSoftHpDamageOff)
		     + ReadI64At(cs, kSoftHpBonusOff);
	}

	// CmCharacterWounds::dealDamage(this, int bodyPart) — applies staged
	// damage to all 3 injury types of the given body part (0..5). Engine RVA
	// 0x1C63E0. We use this directly from a LiFx command after the hook on
	// it captures a live CmCharacterWounds* for a player.
	typedef void (__fastcall *pfn_wounds_deal_damage)(void* self, int bodyPart);
	inline void Wounds_DealDamage(void* self, int bodyPart) {
		static const auto fn = AtRva<pfn_wounds_deal_damage>(0x1C63E0);
		fn(self, bodyPart);
	}

	// ============================================================================
	// CONSOLE LOGGING (rare — most hooks should use Con::Echo via t3d_console.h)
	// ============================================================================
	typedef char (__fastcall *pfn_console_enabled)();
	typedef void (__fastcall *pfn_console_printf)(unsigned type, unsigned unk, const char* msg);
	inline char Console_Enabled() {
		static const auto fn = AtRva<pfn_console_enabled>(0x405040);
		return fn();
	}
	inline void Console_Printf(unsigned type, const char* msg) {
		static const auto fn = AtRva<pfn_console_printf>(0x405090);
		fn(type, 0, msg);
	}

	// ============================================================================
	// DATABASE (execute-only) -- RE'd 2026-09-12 for a script-side GM tools mod
	// that assumed a fictional `dbi.Update(sql)` object; that object doesn't
	// exist anywhere in this engine's TorqueScript surface (confirmed by
	// grepping every real .cs file in the server tree -- zero matches for any
	// DB-object method call pattern). What DOES exist is a native, engine-
	// internal "run this SQL, no result set" function, found via its own
	// diagnostic log tag string ("mNoRS", the same one behind every
	// "DB::mNoRS(N ms) <sql>" line already visible in the server console all
	// night) -- RVA 0x54EF30. It's a member function on a global DB-manager
	// singleton at RVA 0xB7D3A0 (found by tracing an existing caller,
	// FUN_140174830, which reads that exact global and passes it as `this`).
	// This is execute-only (no rows returned) -- fine for INSERT/UPDATE/CREATE
	// TABLE, not usable for SELECT-with-results (that's a separate, more
	// involved native function, FUN_14054ea30, which takes a caller-owned
	// native result-set object as a third argument -- deliberately NOT wrapped
	// here; the one script feature that needs it (GM Tools' economy/inflation
	// tracking) is disabled rather than guessing at that object's C++ layout).
	typedef char (__fastcall *pfn_db_exec_noresult)(void* dbManager, const char* sql);
	inline bool DB_ExecNoResult(const char* sql) {
		static const auto fn = AtRva<pfn_db_exec_noresult>(0x54EF30);
		void* dbManager = *DataAtRva<void*>(0xB7D3A0);
		if (!dbManager || !sql) return false;
		return fn(dbManager, sql) != 0;
	}

	// ============================================================================
	// STRUCT OFFSETS (used by reimplementations to read engine state)
	// ============================================================================
	// Each constant names an offset within an engine object. Read like:
	//     int temp = *(int*)((char*)workingFurnaceSelf + ::Engine::Off::Furnace_State);
	// Confirmed against the decompiles in /tmp/lifx_ghidra/decompile/rt_*.c.
	namespace Off
	{
		// WorkingFire / WorkingFurnace / BrewingTankFurnace — shared base layout
		constexpr unsigned Windmill_LastTick   = 0x08;   // Windmill: timestamp of last tick
		constexpr unsigned Furnace_ContentsRef = 0x0C;   // pointer that Furnace_GetContents reads
		constexpr unsigned Furnace_StateField  = 0x14;   // [5]: furnace state (2000=hot, 500=cool)
		constexpr unsigned Furnace_KindlingBurn= 0x18;   // [6]: kindling-burn counter
		constexpr unsigned Furnace_StateAlt    = 0x28;   // alternate state field used by Fire/Greenhouse
		constexpr unsigned Furnace_Temperature = 0x2C;   // current temperature accumulator
		constexpr unsigned Furnace_FreezeFlag  = 0x34;   // "skip temperature decay" flag

		// Inventory slot layout
		constexpr unsigned Slot_ItemData       = 0x18;   // pointer to the per-instance item data
		constexpr unsigned Slot_QualityField   = 0x30;   // current quality (0..100)

		// ItemData layout
		constexpr unsigned ItemData_TypeId     = 0x08;   // uint32 type id

		// CmCharacterInfo layout (relevant fields)
		constexpr unsigned CharInfo_HardHP     = 0x194;  // current HP (int32)
		constexpr unsigned CharInfo_SoftHP     = 0x19C;  // HP soft cap   (int32)
		constexpr unsigned CharInfo_StatsCount = 0x308;  // int32 — entries in name-table
		constexpr unsigned CharInfo_StatsArray = 0x310;  // ptr to stat array (24-byte rows)
		constexpr unsigned CharInfo_InitFlag   = 0x358;  // 0 if not initialized

		// Process-descriptor row (28 bytes total)
		constexpr unsigned ProcDesc_TypeId         = 0x00;
		constexpr unsigned ProcDesc_Kind           = 0x04;
		constexpr unsigned ProcDesc_Factor         = 0x08;
		constexpr unsigned ProcDesc_OutputType     = 0x0C;
		constexpr unsigned ProcDesc_Flag           = 0x10;
		constexpr unsigned ProcDesc_Field5         = 0x14;
		constexpr unsigned ProcDesc_TempThreshold  = 0x18;

		// Player (ShapeBase) world-space position. Three little-endian floats
		// (x, y, z). RE'd in chunk 13 (#97): scanned the live Player allocation
		// for a known spawn coord. Chunk 13a/14 verification across three samples
		// of moving the character showed +0x1EC0 and +0x2060 both tracked the
		// player exactly. The lower offset is the canonical SceneObject
		// mObjToWorld translation; +0x2060 is a downstream render/last-tick
		// mirror written after the canonical transform, so reading +0x1EC0 is
		// the right source of truth for movement/edge-trigger logic.
		constexpr unsigned Player_WorldPos = 0x1EC0;
	}
}
