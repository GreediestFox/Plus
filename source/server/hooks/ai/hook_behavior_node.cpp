/* ===================================================================================
	Copyright (c) 2026, LiFx Contributors. All Rights Reserved.

	This file is a part of LiFx.
*  =================================================================================== */

#include "hook_behavior_node.h"

#include "server/cm_offsets.h"
#include "server/api/t3d_console.h"
#include "server/hooks/furnace/engine_internals.h"

#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <unordered_map>

__CM_INSTATNTIATE(_Ai_LoadBehaviorXml);

namespace
{
	// ---- engine ABI (RE'd from ddctd_cm_yo_server.exe, see ABI_NOTES.md) ----
	using pfn_getFactory   = void* (__fastcall*)();
	using pfn_registerNode = void  (__fastcall*)(void* factory, const char* name, void** proto);
	using pfn_createByName = void* (__fastcall*)(void* factory, void** out, const char* name);
	using pfn_clone        = void* (__fastcall*)(void* self, void** out);
	using pfn_process      = int   (__fastcall*)(void* self);
	using pfn_xmlAttr      = const char* (__fastcall*)(void* elem, const char* name, int);

	inline uintptr_t ModuleBase() { return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)); }
	template <typename Fn> inline Fn AtRva(uintptr_t rva) { return reinterpret_cast<Fn>(ModuleBase() + rva); }

	// INode vtable: [0]=dtor(this,flags) [1]=load(this,xml)->bool
	//               [2]=clone(this,out)  [3]=process(this)->int  [4],[5]=base
	constexpr int kVtblSlots   = 6;
	constexpr int kSlotLoad    = 1;
	constexpr int kSlotClone   = 2;
	constexpr int kSlotProcess = 3;

	// process() return codes (RE: Damaged returns 2 on its failure path).
	constexpr int kSuccess = 1;
	constexpr int kFailure = 2;

	// Per-instance scalar params live in the base object's free slot at +0x40
	// (object is 0x48 bytes; the no-param "Stopped" template doesn't use it).
	constexpr unsigned kParamOff = 0x40;

	// Template clones — captured once during registration. LifxLogNode/
	// TimeOfDayBetween clone the stateless "Stopped"; GoToPoint clones
	// "GoToPosition" (it needs that node's larger layout + coord/flag fields).
	pfn_clone g_stoppedClone = nullptr;
	pfn_clone g_gotoClone    = nullptr;

	// Each custom node type owns a patched copy of the template vtable, alive
	// for the DLL's lifetime; every instance (prototype + clones) points here.
	void* g_logVtbl[kVtblSlots]  = {};
	void* g_todVtbl[kVtblSlots]  = {};
	void* g_gotoVtbl[kVtblSlots] = {};

	std::atomic<uint64_t> g_logTicks{0};

	// Registration is retried on each tree load until it succeeds (the first
	// load may, in principle, precede built-in node registration). Latches
	// once the "Stopped" template is found and our nodes are registered.
	std::atomic<bool> g_registered{false};
	std::mutex        g_regMtx;

	// ---- LifxLogNode: logs on tick, always succeeds (PoC, issue #119) -------
	int __fastcall LogNodeProcess(void* self)
	{
		const uint64_t n = g_logTicks.fetch_add(1, std::memory_order_relaxed) + 1;
		if (n <= 5 || (n % 500) == 0)
			Con::Echo("[lifx-ai] LifxLogNode tick #%llu (self=%p)",
			          static_cast<unsigned long long>(n), self);
		return kSuccess;
	}
	void* __fastcall LogNodeClone(void* self, void** out)
	{
		void* ret = g_stoppedClone(self, out);
		if (out && *out)
			*reinterpret_cast<void**>(*out) = g_logVtbl;
		return ret;
	}

	// ---- TimeOfDayBetween: succeeds when the in-game hour is in range -------
	// Game clock = 64-bit microsecond counter (see ABI_NOTES.md §time-of-day).
	constexpr uintptr_t kGameTimeWorldPtrRva = 0xB7E4C0;  // -> world/time singleton ptr
	constexpr unsigned  kGameTimeFieldOff    = 0x10;      // int64 micros at obj+0x10
	constexpr long long kMicrosPerHour       = 3'600'000'000LL;

	// Current hour of day 0..23, or -1 if the clock isn't ready yet.
	int CurrentHourOfDay()
	{
		char* obj = *reinterpret_cast<char**>(ModuleBase() + kGameTimeWorldPtrRva);
		if (!obj)
			return -1;
		const long long t = *reinterpret_cast<long long*>(obj + kGameTimeFieldOff);
		long long h = (t / kMicrosPerHour) % 24;
		if (h < 0) h += 24;
		return static_cast<int>(h);
	}

	// Two floats packed into the +0x40 slot: start at +0x40, end at +0x44.
	inline float& ParamStart(void* self) { return *reinterpret_cast<float*>(static_cast<char*>(self) + kParamOff); }
	inline float& ParamEnd  (void* self) { return *reinterpret_cast<float*>(static_cast<char*>(self) + kParamOff + 4); }

	// loadFromXml: parse value="start end" (hours). Defaults to night (20..6)
	// when absent — so <node class="IsNight"/> works with no value.
	char __fastcall TodLoad(void* self, void* xmlElem)
	{
		auto attr = AtRva<pfn_xmlAttr>(CmOffset::AI_TIXML_ATTRIBUTE);
		float start = 20.0f, end = 6.0f;
		const char* v = attr(xmlElem, "value", 0);
		if (v)
		{
			char* p = nullptr;
			const double s = std::strtod(v, &p);
			const double e = std::strtod(p, nullptr);
			start = static_cast<float>(s);
			end   = static_cast<float>(e);
		}
		ParamStart(self) = start;
		ParamEnd(self)   = end;

		static std::atomic<bool> logged{false};
		if (!logged.exchange(true))
			Con::Echo("[lifx-ai] TodLoad: value='%s' -> start=%g end=%g (self=%p)",
			          v ? v : "(null)", start, end, self);
		return 1; // success
	}
	int __fastcall TodProcess(void* self)
	{
		const int h = CurrentHourOfDay();
		if (h < 0)
			return kFailure; // clock not ready yet

		const float start = ParamStart(self);
		const float end   = ParamEnd(self);
		const bool  in = (start <= end) ? (h >= start && h < end)   // daytime window
		                                : (h >= start || h < end);  // wraps midnight
		return in ? kSuccess : kFailure;
	}
	void* __fastcall TodClone(void* self, void** out)
	{
		void* ret = g_stoppedClone(self, out);
		if (out && *out)
		{
			*reinterpret_cast<void**>(*out) = g_todVtbl;
			// Stopped's clone doesn't know about our +0x40 param, so copy the
			// packed start/end floats across ourselves (else the range resets
			// to 0 on the per-creature clone of the loaded template node).
			*reinterpret_cast<uint64_t*>(static_cast<char*>(*out) + kParamOff) =
				*reinterpret_cast<uint64_t*>(static_cast<char*>(self) + kParamOff);
		}
		return ret;
	}

	// ---- GoToPoint: pathfind to a fixed world coord (issue #123) ------------
	// Cloned from "GoToPosition" so it inherits that node's larger layout
	// (dest x/y/z at +0x40/+0x44/+0x48, "moving" flag at +0x4c), correct clone
	// + dtor, AND its process (slot 3) verbatim — engine pathfinding + arrival
	// detection come for free. We override only the loader (parse coords) and
	// the clone (swap to our vtable).
	constexpr unsigned kDestX = 0x40, kDestY = 0x44, kDestZ = 0x48;

	char __fastcall GoToPointLoad(void* self, void* xmlElem)
	{
		auto attr = AtRva<pfn_xmlAttr>(CmOffset::AI_TIXML_ATTRIBUTE);
		float x = 0, y = 0, z = 0;
		const char* v = attr(xmlElem, "value", 0);
		if (v)
		{
			char* p = nullptr;
			x = static_cast<float>(std::strtod(v, &p));
			y = static_cast<float>(std::strtod(p, &p));
			z = static_cast<float>(std::strtod(p, nullptr));
		}
		*reinterpret_cast<float*>(static_cast<char*>(self) + kDestX) = x;
		*reinterpret_cast<float*>(static_cast<char*>(self) + kDestY) = y;
		*reinterpret_cast<float*>(static_cast<char*>(self) + kDestZ) = z;

		static std::atomic<bool> logged{false};
		if (!logged.exchange(true))
			Con::Echo("[lifx-ai] GoToPointLoad: value='%s' -> dest=(%g,%g,%g)",
			          v ? v : "(null)", x, y, z);
		return 1;
	}
	void* __fastcall GoToPointClone(void* self, void** out)
	{
		// GoToPosition's clone copies the coord/flag fields for us, so we only
		// retarget the vtable. (No hand-copy like the Stopped-based nodes need.)
		void* ret = g_gotoClone(self, out);
		if (out && *out)
			*reinterpret_cast<void**>(*out) = g_gotoVtbl;
		return ret;
	}
	// Engine helpers for movement (RVAs, see ABI_NOTES.md §AI context).
	constexpr int kRunning = 3;
	constexpr unsigned kMovingFlag = 0x4c;       // GoToPosition-shaped: "moving" byte
	using pfn_strCtor  = void* (__fastcall*)(void* outStr, const char* cstr);
	using pfn_strDtor  = void  (__fastcall*)(void* str);
	using pfn_bbFind   = void* (__fastcall*)(void* aidata, void* keyStr); // -> creature handle
	using pfn_getEngine= void* (__fastcall*)(void* creature);
	using pfn_setTarget= void  (__fastcall*)(void* engine, void* vec3);
	using pfn_poll     = int   (__fastcall*)(void* engine);              // 1 = arrived
	using pfn_stop     = void  (__fastcall*)(void* engine);

	// Resolve the creature object for this node via the AI-context blackboard,
	// which is keyed by creature class ("animal" vs "npcbase"). GoToPosition is
	// hard-wired to "npcbase" (fails on animals); we try "animal" then "npcbase"
	// so GoToPoint works for either. Returns nullptr if not found. Shared by the
	// GoToPoint movement node and the human-NPC perception nodes below.
	void* BbResolveCreature(void* self)
	{
		void* aidata = *reinterpret_cast<void**>(static_cast<char*>(self) + 0x28);
		if (!aidata) return nullptr;
		auto strCtor = AtRva<pfn_strCtor>(0x454FA0);
		auto strDtor = AtRva<pfn_strDtor>(0x86D60);
		// The blackboard boxes the creature as a SimObjectPtr<T>, and the find is
		// a per-T template that RTTI-type-GATES: animals are boxed as
		// SimObjectPtr<Animals::Animal> under key "animal" (find 0x190D70); NPCs
		// (our NPCDecorative, an NPCS::Base) are boxed as SimObjectPtr<NPCS::Base>
		// under key "npcbase" (find 0x190E90). Using the Animal find on the
		// "npcbase" box returns null (the bug that made GoToPoint fail on NPCs).
		// So pair each key with its matching find instantiation.
		struct KeyFind { const char* key; unsigned rva; };
		for (const KeyFind& t : { KeyFind{ "animal", 0x190D70 }, KeyFind{ "npcbase", 0x190E90 } })
		{
			auto bbFind = AtRva<pfn_bbFind>(t.rva);
			alignas(16) unsigned char keyStr[64] = {};
			strCtor(keyStr, t.key);
			void* handle = bbFind(aidata, keyStr);
			strDtor(keyStr);
			if (handle)
			{
				void* inner = *reinterpret_cast<void**>(handle);     // SimObjectPtr box (== 0x191130)
				if (inner) return *reinterpret_cast<void**>(inner);  // -> live creature
			}
		}
		return nullptr;
	}

	int __fastcall GoToPointProcess(void* self)
	{
		void* creature = BbResolveCreature(self);
		static std::atomic<bool> logged{false};
		const bool first = !logged.exchange(true);

		if (!creature)
		{
			if (first) Con::Echo("[lifx-ai] GoToPoint: no creature in AI-context; failing");
			return kFailure;
		}
		void* engine = AtRva<pfn_getEngine>(0x2E3380)(creature);
		if (!engine) return kFailure;

		char* flag = static_cast<char*>(self) + kMovingFlag;
		if (*flag)
		{
			if (AtRva<pfn_poll>(0x14FD30)(engine) == 1)   // arrived
			{
				*flag = 0;
				AtRva<pfn_stop>(0x14FE40)(engine);
				Con::Echo("[lifx-ai] GoToPoint -> arrived (1)");
				return kSuccess;
			}
			return kRunning;
		}
		*flag = 1;
		AtRva<pfn_setTarget>(0x14FD50)(engine, static_cast<char*>(self) + kDestX);  // dest x/y/z
		if (first)
			Con::Echo("[lifx-ai] GoToPoint -> moving to (%g,%g,%g)",
			          *reinterpret_cast<float*>(static_cast<char*>(self) + kDestX),
			          *reinterpret_cast<float*>(static_cast<char*>(self) + kDestY),
			          *reinterpret_cast<float*>(static_cast<char*>(self) + kDestZ));
		return kRunning;
	}

	// ======================================================================
	// Human-NPC perception / movement nodes (issue #133, the "npcbase" path).
	//
	// The native animal combat nodes (EnemyInRange, SetPlayerAsTarget, ...) are
	// RTTI-gated to Animals::Animal (blackboard key "animal", target slot
	// Animal+0x2520) and crash with "object is nullptr" on a human NPCDecorative
	// (an NPCS::Base). These gate-less variants resolve the creature via the
	// "npcbase" box (BbResolveCreature) and keep per-NPC state (target / home /
	// cooldowns) in our own side table keyed by SimObjectId, driving the same
	// NPCS::Base move engine GoToPoint already uses.
	// See docs/hostile-npc-ai-path-comparison.md and ABI_NOTES.md.
	// ======================================================================

	// Object world position = the translation column of its transform. The
	// engine getPosition (vtbl+0x280, RVA 0x520DC0) just reads these; we read
	// them directly. SimObjectId lives at obj+0x90 (id allocator 0x4290D0).
	constexpr unsigned kPosX = 0x284, kPosY = 0x294, kPosZ = 0x2A4;
	constexpr unsigned kSimIdOff = 0x90;

	inline void ObjPos(void* obj, float& x, float& y, float& z)
	{
		x = *reinterpret_cast<float*>(static_cast<char*>(obj) + kPosX);
		y = *reinterpret_cast<float*>(static_cast<char*>(obj) + kPosY);
		z = *reinterpret_cast<float*>(static_cast<char*>(obj) + kPosZ);
	}
	inline uint32_t ObjId(void* obj)
	{
		return *reinterpret_cast<uint32_t*>(static_cast<char*>(obj) + kSimIdOff);
	}
	inline float Dist2(float ax, float ay, float az, float bx, float by, float bz)
	{
		const float dx = ax - bx, dy = ay - by, dz = az - bz;
		return dx * dx + dy * dy + dz * dz;
	}

	// ---- per-NPC AI state, keyed by the creature's SimObjectId -------------
	// TODO(#133 follow-up): prune entries on NPC removal (SimObjectIds can be
	// reused). Fine for current single/low-count NPC use.
	struct NpcAiState
	{
		uint32_t targetId  = 0;        // SimObjectId of the current target (0 = none)
		void*    targetPtr = nullptr;  // last-known target pointer (valid same-tick)
		bool     homeSet   = false;
		float    homeX = 0.f, homeY = 0.f, homeZ = 0.f;
		std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> cooldowns; // name-hash -> last fire
	};
	std::mutex                               g_npcStateMtx;
	std::unordered_map<uint32_t, NpcAiState> g_npcState;

	// The container radius search is a process-global, non-reentrant singleton,
	// so serialize the scan via the SHARED lock in engine_internals.h
	// (Engine::ContainerSearchMutex) rather than a private mutex here -- a
	// private-per-file mutex does NOT actually exclude other callers of the
	// same engine singleton from a DIFFERENT translation unit. Confirmed the
	// hard way 2026-09-12: Lifx::scanNearbyLights (lifx_effects.cpp) called
	// this same search with no lock at all, raced this AI tick, and crashed
	// the live server. initContainerRadiusSearch / containerSearchNext.
	using pfn_radiusInit = void  (__fastcall*)(void* posXyz, float radius, unsigned mask, bool useClient); // 0x51A490
	using pfn_radiusNext = void* (__fastcall*)();                                                          // 0x51A480 (sets this)
	constexpr unsigned kPlayerObjectType = 0x8000;

	inline uint32_t Fnv1a(const char* s)
	{
		uint32_t h = 2166136261u;
		for (; s && *s; ++s) { h ^= static_cast<unsigned char>(*s); h *= 16777619u; }
		return h;
	}

	// Stopped-template clone: delegate to "Stopped"'s clone, retarget the vtable,
	// and hand-copy our packed +0x40 param (Stopped's clone doesn't know it).
	void* StoppedCloneInto(void* self, void** out, void** vtbl)
	{
		void* ret = g_stoppedClone(self, out);
		if (out && *out)
		{
			*reinterpret_cast<void**>(*out) = vtbl;
			*reinterpret_cast<uint64_t*>(static_cast<char*>(*out) + kParamOff) =
				*reinterpret_cast<uint64_t*>(static_cast<char*>(self) + kParamOff);
		}
		return ret;
	}

	// ---- SetNearestPlayerAsTarget value="radius" --------------------------
	// Scans players within `radius`, stores the nearest as the side-table
	// target, succeeds. Fails (no state change) if none in range -> doubles as
	// the proximity gate at the top of a greet/flee branch.
	// KNOWN LIMITATION (#133 follow-up): NPCDecorative is itself a
	// PlayerObjectType, so the scan also returns other NPCs; we skip self but do
	// not yet filter NPC-vs-real-player (needs a client-controlled check).
	// Scan players within `radius` of `creature`; return the nearest (skipping
	// self) or nullptr. Shared by SetNearestPlayerAsTarget / ChaseTarget /
	// FleeFromTarget. Re-scanning per tick (rather than caching a target pointer)
	// keeps Chase/Flee self-healing when the target leaves scope. The container
	// search is a global non-reentrant singleton -> serialized by the shared
	// Engine::ContainerSearchMutex() (engine_internals.h).
	void* ScanNearestPlayer(void* creature, float radius)
	{
		float sx, sy, sz; ObjPos(creature, sx, sy, sz);
		float center[3] = { sx, sy, sz };
		void* best = nullptr; float bestD2 = FLT_MAX;
		std::lock_guard<std::mutex> lk(::Engine::ContainerSearchMutex());
		AtRva<pfn_radiusInit>(0x51A490)(center, radius, kPlayerObjectType, false);
		auto next = AtRva<pfn_radiusNext>(0x51A480);
		for (void* o = next(); o != nullptr; o = next())       // drain fully (singleton)
		{
			if (o == creature) continue;                       // skip ourselves
			float ox, oy, oz; ObjPos(o, ox, oy, oz);
			const float d2 = Dist2(sx, sy, sz, ox, oy, oz);
			if (d2 < bestD2) { bestD2 = d2; best = o; }
		}
		return best;
	}

	void* g_setTargetVtbl[kVtblSlots] = {};
	char __fastcall SetTargetLoad(void* self, void* xmlElem)
	{
		auto attr = AtRva<pfn_xmlAttr>(CmOffset::AI_TIXML_ATTRIBUTE);
		float radius = 5.0f;
		const char* v = attr(xmlElem, "value", 0);
		if (v) radius = static_cast<float>(std::strtod(v, nullptr));
		*reinterpret_cast<float*>(static_cast<char*>(self) + kParamOff) = radius;
		return 1;
	}
	int __fastcall SetTargetProcess(void* self)
	{
		void* creature = BbResolveCreature(self);
		if (!creature) return kFailure;
		const float radius = *reinterpret_cast<float*>(static_cast<char*>(self) + kParamOff);
		void* best = ScanNearestPlayer(creature, radius);

		std::lock_guard<std::mutex> lk(g_npcStateMtx);
		NpcAiState& st = g_npcState[ObjId(creature)];
		if (!best) { st.targetId = 0; st.targetPtr = nullptr; return kFailure; }
		st.targetPtr = best;
		st.targetId  = ObjId(best);
		return kSuccess;
	}
	void* __fastcall SetTargetClone(void* self, void** out) { return StoppedCloneInto(self, out, g_setTargetVtbl); }

	// ---- HasPlayerTarget --------------------------------------------------
	void* g_hasTargetVtbl[kVtblSlots] = {};
	int __fastcall HasTargetProcess(void* self)
	{
		void* creature = BbResolveCreature(self);
		if (!creature) return kFailure;
		std::lock_guard<std::mutex> lk(g_npcStateMtx);
		auto it = g_npcState.find(ObjId(creature));
		const bool has = (it != g_npcState.end() && it->second.targetId != 0 && it->second.targetPtr);
		return has ? kSuccess : kFailure;
	}
	void* __fastcall HasTargetClone(void* self, void** out) { return StoppedCloneInto(self, out, g_hasTargetVtbl); }

	// ---- TargetInRange value="min max" ------------------------------------
	// Reads the side-table target set this tick by SetNearestPlayerAsTarget
	// (intended to follow it in the same Sequence). min/max in world units.
	void* g_targetRangeVtbl[kVtblSlots] = {};
	char __fastcall TargetRangeLoad(void* self, void* xmlElem)
	{
		auto attr = AtRva<pfn_xmlAttr>(CmOffset::AI_TIXML_ATTRIBUTE);
		float lo = 0.0f, hi = 5.0f;
		const char* v = attr(xmlElem, "value", 0);
		if (v)
		{
			char* p = nullptr;
			lo = static_cast<float>(std::strtod(v, &p));
			hi = static_cast<float>(std::strtod(p, nullptr));
		}
		*reinterpret_cast<float*>(static_cast<char*>(self) + kParamOff)     = lo;
		*reinterpret_cast<float*>(static_cast<char*>(self) + kParamOff + 4) = hi;
		return 1;
	}
	int __fastcall TargetRangeProcess(void* self)
	{
		void* creature = BbResolveCreature(self);
		if (!creature) return kFailure;
		void* target = nullptr;
		{
			std::lock_guard<std::mutex> lk(g_npcStateMtx);
			auto it = g_npcState.find(ObjId(creature));
			if (it != g_npcState.end()) target = it->second.targetPtr;
		}
		if (!target) return kFailure;
		const float lo = *reinterpret_cast<float*>(static_cast<char*>(self) + kParamOff);
		const float hi = *reinterpret_cast<float*>(static_cast<char*>(self) + kParamOff + 4);
		float sx, sy, sz; ObjPos(creature, sx, sy, sz);
		float tx, ty, tz; ObjPos(target,   tx, ty, tz);
		const float d2 = Dist2(sx, sy, sz, tx, ty, tz);
		return (d2 >= lo * lo && d2 <= hi * hi) ? kSuccess : kFailure;
	}
	void* __fastcall TargetRangeClone(void* self, void** out) { return StoppedCloneInto(self, out, g_targetRangeVtbl); }

	// ---- CooldownGate value="name seconds" --------------------------------
	// Named gate: succeeds + arms on entry, then fails until `seconds` of
	// WALL-CLOCK time elapse (deliberately NOT the accelerated game clock
	// TimeOfDayBetween uses -- 20 game-seconds would pass almost instantly).
	// Name stored as a 32-bit hash at +0x40, seconds as a float at +0x44.
	void* g_cooldownVtbl[kVtblSlots] = {};
	char __fastcall CooldownLoad(void* self, void* xmlElem)
	{
		auto attr = AtRva<pfn_xmlAttr>(CmOffset::AI_TIXML_ATTRIBUTE);
		uint32_t nameHash = Fnv1a("default");
		float seconds = 20.0f;
		const char* v = attr(xmlElem, "value", 0);
		if (v)
		{
			char buf[64] = {};
			const char* p = v;
			while (*p == ' ' || *p == '\t') ++p;                 // skip leading ws
			int i = 0;
			while (*p && *p != ' ' && *p != '\t' && i < 63) buf[i++] = *p++;
			buf[i] = '\0';
			if (i > 0) nameHash = Fnv1a(buf);
			seconds = static_cast<float>(std::strtod(p, nullptr));
			if (seconds <= 0.0f) seconds = 20.0f;
		}
		*reinterpret_cast<uint32_t*>(static_cast<char*>(self) + kParamOff)  = nameHash;
		*reinterpret_cast<float*>(static_cast<char*>(self) + kParamOff + 4) = seconds;
		return 1;
	}
	int __fastcall CooldownProcess(void* self)
	{
		void* creature = BbResolveCreature(self);
		if (!creature) return kFailure;
		const uint32_t nameHash = *reinterpret_cast<uint32_t*>(static_cast<char*>(self) + kParamOff);
		const float    seconds  = *reinterpret_cast<float*>(static_cast<char*>(self) + kParamOff + 4);
		const auto now = std::chrono::steady_clock::now();

		std::lock_guard<std::mutex> lk(g_npcStateMtx);
		NpcAiState& st = g_npcState[ObjId(creature)];
		auto it = st.cooldowns.find(nameHash);
		if (it != st.cooldowns.end())
		{
			const float elapsed = std::chrono::duration<float>(now - it->second).count();
			if (elapsed < seconds) return kFailure;              // still on cooldown
		}
		st.cooldowns[nameHash] = now;                            // arm
		return kSuccess;
	}
	void* __fastcall CooldownClone(void* self, void** out) { return StoppedCloneInto(self, out, g_cooldownVtbl); }

	// ---- ReturnToHomePosition (cloned from GoToPosition for its move layout) -
	// Home = the NPC position captured on first tick (per-NPC, in the side
	// table). Drives the move engine home when displaced; FAILS when already
	// home so lower-priority branches (greet / idle) run. Value is ignored.
	void* g_returnHomeVtbl[kVtblSlots] = {};
	char __fastcall ReturnHomeLoad(void* /*self*/, void* /*xmlElem*/) { return 1; }
	int __fastcall ReturnHomeProcess(void* self)
	{
		void* creature = BbResolveCreature(self);
		if (!creature) return kFailure;
		void* engine = AtRva<pfn_getEngine>(0x2E3380)(creature);
		if (!engine) return kFailure;

		float cx, cy, cz; ObjPos(creature, cx, cy, cz);
		float hx, hy, hz;
		{
			std::lock_guard<std::mutex> lk(g_npcStateMtx);
			NpcAiState& st = g_npcState[ObjId(creature)];
			if (!st.homeSet) { st.homeX = cx; st.homeY = cy; st.homeZ = cz; st.homeSet = true; }
			hx = st.homeX; hy = st.homeY; hz = st.homeZ;
		}

		char* flag = static_cast<char*>(self) + kMovingFlag;
		constexpr float kArrive2 = 1.0f;                         // within 1 unit of home = "home"
		if (Dist2(cx, cy, cz, hx, hy, hz) <= kArrive2)
		{
			if (*flag) { *flag = 0; AtRva<pfn_stop>(0x14FE40)(engine); }
			return kFailure;                                     // at home -> let greet/idle run
		}
		*reinterpret_cast<float*>(static_cast<char*>(self) + kDestX) = hx;
		*reinterpret_cast<float*>(static_cast<char*>(self) + kDestY) = hy;
		*reinterpret_cast<float*>(static_cast<char*>(self) + kDestZ) = hz;
		if (!*flag)
		{
			*flag = 1;
			AtRva<pfn_setTarget>(0x14FD50)(engine, static_cast<char*>(self) + kDestX);
		}
		return kRunning;
	}
	void* __fastcall ReturnHomeClone(void* self, void** out)
	{
		void* ret = g_gotoClone(self, out);                      // copies dest/flag layout for us
		if (out && *out) *reinterpret_cast<void**>(*out) = g_returnHomeVtbl;
		return ret;
	}

	// ---- ChaseTarget value="radius" ---------------------------------------
	// Pursue the nearest player within radius (re-scanned each tick). Drives the
	// move engine toward the target's live position; RUNNING while a target
	// exists, FAILURE (stop) when none in range.
	void* g_chaseVtbl[kVtblSlots] = {};
	char __fastcall ChaseLoad(void* self, void* xmlElem)
	{
		auto attr = AtRva<pfn_xmlAttr>(CmOffset::AI_TIXML_ATTRIBUTE);
		float radius = 25.0f;
		const char* v = attr(xmlElem, "value", 0);
		if (v) radius = static_cast<float>(std::strtod(v, nullptr));
		*reinterpret_cast<float*>(static_cast<char*>(self) + kParamOff) = radius;
		return 1;
	}
	int __fastcall ChaseProcess(void* self)
	{
		void* creature = BbResolveCreature(self);
		if (!creature) return kFailure;
		const float radius = *reinterpret_cast<float*>(static_cast<char*>(self) + kParamOff);
		void* engine = AtRva<pfn_getEngine>(0x2E3380)(creature);
		if (!engine) return kFailure;

		void* target = ScanNearestPlayer(creature, radius);
		char* flag = static_cast<char*>(self) + kMovingFlag;
		if (!target)
		{
			if (*flag) { *flag = 0; AtRva<pfn_stop>(0x14FE40)(engine); }
			return kFailure;                                   // no quarry -> fall through
		}
		float tx, ty, tz; ObjPos(target, tx, ty, tz);
		float* dest = reinterpret_cast<float*>(static_cast<char*>(self) + kDestX);
		// Re-issue the move target only when it drifts enough to matter (avoids
		// repathing every tick to a near-identical point).
		const bool moved = !*flag || Dist2(dest[0], dest[1], dest[2], tx, ty, tz) > 4.0f;
		if (moved)
		{
			dest[0] = tx; dest[1] = ty; dest[2] = tz;
			*flag = 1;
			AtRva<pfn_setTarget>(0x14FD50)(engine, dest);
		}
		return kRunning;
	}
	void* __fastcall ChaseClone(void* self, void** out)
	{
		void* ret = g_gotoClone(self, out);
		if (out && *out) *reinterpret_cast<void**>(*out) = g_chaseVtbl;
		return ret;
	}

	// ---- FleeFromTarget value="radius [fleeDist]" -------------------------
	// If a player is within `radius` (re-scanned each tick), move directly away
	// from them to a point `fleeDist` units off (default 30). RUNNING while a
	// threat is in range, SUCCESS (stop) once clear. radius +0x40, fleeDist +0x44.
	void* g_fleeVtbl[kVtblSlots] = {};
	char __fastcall FleeLoad(void* self, void* xmlElem)
	{
		auto attr = AtRva<pfn_xmlAttr>(CmOffset::AI_TIXML_ATTRIBUTE);
		float radius = 50.0f, fleeDist = 30.0f;
		const char* v = attr(xmlElem, "value", 0);
		if (v)
		{
			char* p = nullptr;
			radius = static_cast<float>(std::strtod(v, &p));
			const double d = std::strtod(p, nullptr);
			if (d > 0.0) fleeDist = static_cast<float>(d);
		}
		*reinterpret_cast<float*>(static_cast<char*>(self) + kParamOff)     = radius;
		*reinterpret_cast<float*>(static_cast<char*>(self) + kParamOff + 4) = fleeDist;
		return 1;
	}
	int __fastcall FleeProcess(void* self)
	{
		void* creature = BbResolveCreature(self);
		if (!creature) return kFailure;
		const float radius   = *reinterpret_cast<float*>(static_cast<char*>(self) + kParamOff);
		const float fleeDist = *reinterpret_cast<float*>(static_cast<char*>(self) + kParamOff + 4);
		void* engine = AtRva<pfn_getEngine>(0x2E3380)(creature);
		if (!engine) return kFailure;

		void* threat = ScanNearestPlayer(creature, radius);
		char* flag = static_cast<char*>(self) + kMovingFlag;
		if (!threat)
		{
			if (*flag) { *flag = 0; AtRva<pfn_stop>(0x14FE40)(engine); }
			return kSuccess;                                   // clear -> branch done
		}
		float cx, cy, cz; ObjPos(creature, cx, cy, cz);
		float tx, ty, tz; ObjPos(threat,   tx, ty, tz);
		// Away vector (self - threat), normalized in the XY plane.
		float ax = cx - tx, ay = cy - ty;
		float len = std::sqrt(ax * ax + ay * ay);
		if (len < 0.001f) { ax = 1.0f; ay = 0.0f; len = 1.0f; }   // degenerate -> +X
		ax /= len; ay /= len;
		float* dest = reinterpret_cast<float*>(static_cast<char*>(self) + kDestX);
		dest[0] = cx + ax * fleeDist;
		dest[1] = cy + ay * fleeDist;
		dest[2] = cz;
		*flag = 1;
		AtRva<pfn_setTarget>(0x14FD50)(engine, dest);
		return kRunning;
	}
	void* __fastcall FleeClone(void* self, void** out)
	{
		void* ret = g_gotoClone(self, out);
		if (out && *out) *reinterpret_cast<void**>(*out) = g_fleeVtbl;
		return ret;
	}

	// ---- FaceTarget -------------------------------------------------------
	// Yaw-rotate the NPC to face the side-table target (set this tick by a
	// preceding SetNearestPlayerAsTarget). Builds a Z-rotation MatrixF in a
	// local copy of the object transform (obj+0x278, row-major 4x4 — translation
	// at flat indices 3/7/11 preserved), installs it via the real setTransform
	// core (0x5224C0), then setMaskBits (0x54A950, bit 0x80) so it replicates.
	// No move layout needed -> Stopped-based. RVAs proven (facing RE, #135).
	// NOTE: forward = +Y; the rotation sign below may need flipping after an
	// in-game check (cosmetic only — the matrix stays orthonormal either way).
	// Place AFTER the cooldown gate so it doesn't re-replicate every tick.
	using pfn_setTransform = void (__fastcall*)(void* self, const void* matrix); // 0x5224C0
	using pfn_setMaskBits  = void (__fastcall*)(void* self, unsigned mask);      // 0x54A950
	constexpr unsigned kTransformOff = 0x278;
	constexpr unsigned kTransformMaskBit = 0x80;

	void* g_faceVtbl[kVtblSlots] = {};
	int __fastcall FaceProcess(void* self)
	{
		void* creature = BbResolveCreature(self);
		if (!creature) return kFailure;
		void* target = nullptr;
		{
			std::lock_guard<std::mutex> lk(g_npcStateMtx);
			auto it = g_npcState.find(ObjId(creature));
			if (it != g_npcState.end()) target = it->second.targetPtr;
		}
		if (!target) return kFailure;

		float sx, sy, sz; ObjPos(creature, sx, sy, sz);
		float tx, ty, tz; ObjPos(target,   tx, ty, tz);
		float dx = tx - sx, dy = ty - sy;
		const float len = std::sqrt(dx * dx + dy * dy);
		if (len < 0.001f) return kSuccess;                   // on top of target -> nothing to do
		const float dirx = dx / len, diry = dy / len;

		float m[16];
		std::memcpy(m, static_cast<char*>(creature) + kTransformOff, sizeof(m)); // keep translation (3/7/11) + bottom row
		m[0] = diry;  m[1] = dirx; m[2]  = 0.f;              // rotation basis (Z yaw), forward(+Y col) -> (dirx,diry,0)
		m[4] = -dirx; m[5] = diry; m[6]  = 0.f;
		m[8] = 0.f;   m[9] = 0.f;  m[10] = 1.f;
		AtRva<pfn_setTransform>(0x5224C0)(creature, m);
		AtRva<pfn_setMaskBits>(0x54A950)(creature, kTransformMaskBit);
		return kSuccess;
	}
	void* __fastcall FaceClone(void* self, void** out) { return StoppedCloneInto(self, out, g_faceVtbl); }

	// ---- AttackTarget value="abilityId [range] [cooldown]" ---------------
	// Trigger a melee ability against the side-table target when in range, on a
	// per-NPC cooldown. Reuses the engine ability-trigger 0x9AC20(charStats,
	// abilityId) -- the SAME call the native PerformAbilityAnim node makes on an
	// npcbase creature's Player charStats (creature+0xAA8, proven embedded
	// subobject via `add rcx,0xaa8`). The swing's generic hit-resolution
	// (0xFBBA0 -> victim _applyHit at vtbl+0x340) then sweeps whoever the NPC
	// faces, so pair this with FaceTarget.
	//
	// IMPORTANT (combat-init RE, #135): this triggers the swing ANIMATION; it
	// only DEALS DAMAGE if the NPC has real combat state -- a valid melee
	// abilityId + AttackAnimationData binding, ideally an equipped weapon. On a
	// bare NPCDecorative the swing may play with no damage; that gap ties to the
	// equip/weapon work (#125), not to this node. `abilityId` must be a valid
	// ability id (an unknown id may misbehave exactly as PerformAbilityAnim would).
	// Layout (GoToPosition-clone): abilityId(int) +0x40, range +0x44, cooldown +0x48.
	using pfn_useAbility = void (__fastcall*)(void* charStats, int abilityId);   // 0x9AC20
	constexpr unsigned kCharStatsOff = 0xAA8;
	constexpr unsigned kAtkAbility = 0x40, kAtkRange = 0x44, kAtkCd = 0x48;

	void* g_attackVtbl[kVtblSlots] = {};
	char __fastcall AttackLoad(void* self, void* xmlElem)
	{
		auto attr = AtRva<pfn_xmlAttr>(CmOffset::AI_TIXML_ATTRIBUTE);
		int abilityId = 0; float range = 3.0f, cooldown = 1.5f;
		const char* v = attr(xmlElem, "value", 0);
		if (v)
		{
			char* p = nullptr;
			abilityId = static_cast<int>(std::strtol(v, &p, 10));
			const double r = std::strtod(p, &p);      if (r > 0.0) range    = static_cast<float>(r);
			const double c = std::strtod(p, nullptr); if (c > 0.0) cooldown = static_cast<float>(c);
		}
		*reinterpret_cast<int*>  (static_cast<char*>(self) + kAtkAbility) = abilityId;
		*reinterpret_cast<float*>(static_cast<char*>(self) + kAtkRange)   = range;
		*reinterpret_cast<float*>(static_cast<char*>(self) + kAtkCd)      = cooldown;
		return 1;
	}
	int __fastcall AttackProcess(void* self)
	{
		void* creature = BbResolveCreature(self);
		if (!creature) return kFailure;
		const int   abilityId = *reinterpret_cast<int*>  (static_cast<char*>(self) + kAtkAbility);
		const float range     = *reinterpret_cast<float*>(static_cast<char*>(self) + kAtkRange);
		const float cooldown  = *reinterpret_cast<float*>(static_cast<char*>(self) + kAtkCd);

		void* target = nullptr;
		{
			std::lock_guard<std::mutex> lk(g_npcStateMtx);
			auto it = g_npcState.find(ObjId(creature));
			if (it != g_npcState.end()) target = it->second.targetPtr;
		}
		if (!target) return kFailure;

		float sx, sy, sz; ObjPos(creature, sx, sy, sz);
		float tx, ty, tz; ObjPos(target,   tx, ty, tz);
		if (Dist2(sx, sy, sz, tx, ty, tz) > range * range) return kFailure;   // out of reach -> chase

		// Per-NPC swing cooldown (reuses the side-table cooldown map, fixed key).
		const uint32_t cdKey = Fnv1a("attack");
		const auto now = std::chrono::steady_clock::now();
		{
			std::lock_guard<std::mutex> lk(g_npcStateMtx);
			NpcAiState& st = g_npcState[ObjId(creature)];
			auto it = st.cooldowns.find(cdKey);
			if (it != st.cooldowns.end())
			{
				const float elapsed = std::chrono::duration<float>(now - it->second).count();
				if (elapsed < cooldown) return kSuccess;     // in range, between swings
			}
			st.cooldowns[cdKey] = now;                       // arm
		}

		void* charStats = static_cast<char*>(creature) + kCharStatsOff;  // embedded subobject
		AtRva<pfn_useAbility>(0x9AC20)(charStats, abilityId);
		return kSuccess;
	}
	void* __fastcall AttackClone(void* self, void** out)
	{
		void* ret = g_gotoClone(self, out);
		if (out && *out) *reinterpret_cast<void**>(*out) = g_attackVtbl;
		return ret;
	}

	// Clone `templateName`, point the clone at `vtbl`, register under `xmlName`.
	// registerNode moves ownership of the prototype into the factory.
	bool RegisterUnder(pfn_createByName createByName, pfn_registerNode registerNode,
	                   void* factory, const char* templateName, const char* xmlName, void** vtbl)
	{
		void* proto = nullptr;
		createByName(factory, &proto, templateName);
		if (!proto)
			return false;
		*reinterpret_cast<void**>(proto) = vtbl;
		registerNode(factory, xmlName, &proto);
		return true;
	}

	// Returns true once our nodes are registered (or already were).
	bool TryRegisterCustomNodes()
	{
		auto getFactory   = AtRva<pfn_getFactory>(CmOffset::AI_GET_NODE_FACTORY);
		auto registerNode = AtRva<pfn_registerNode>(CmOffset::AI_REGISTER_NODE);
		auto createByName = AtRva<pfn_createByName>(CmOffset::AI_CREATE_BY_NAME);

		void* factory = getFactory();
		if (!factory)
			return false;

		// Snapshot the template ("Stopped") vtable once + capture its clone. If
		// the built-in modules haven't registered "Stopped" yet, retry next load.
		void* tmpl = nullptr;
		createByName(factory, &tmpl, "Stopped");
		if (!tmpl)
			return false;
		void** base = *reinterpret_cast<void***>(tmpl);
		g_stoppedClone = reinterpret_cast<pfn_clone>(base[kSlotClone]);

		// LifxLogNode — log + succeed; reuse template loader (ignores value).
		for (int i = 0; i < kVtblSlots; ++i) g_logVtbl[i] = base[i];
		g_logVtbl[kSlotClone]   = reinterpret_cast<void*>(&LogNodeClone);
		g_logVtbl[kSlotProcess] = reinterpret_cast<void*>(&LogNodeProcess);

		// TimeOfDayBetween / IsNight — own loader (parses value) + own process.
		for (int i = 0; i < kVtblSlots; ++i) g_todVtbl[i] = base[i];
		g_todVtbl[kSlotLoad]    = reinterpret_cast<void*>(&TodLoad);
		g_todVtbl[kSlotClone]   = reinterpret_cast<void*>(&TodClone);
		g_todVtbl[kSlotProcess] = reinterpret_cast<void*>(&TodProcess);

		// GoToPoint — clone "GoToPosition" (richer layout); override loader to
		// parse coords, reuse its process (slot 3) for pathfinding + arrival.
		void* gtmpl = nullptr;
		createByName(factory, &gtmpl, "GoToPosition");
		if (!gtmpl)
			return false; // built-ins not ready yet; retry next load
		void** gbase = *reinterpret_cast<void***>(gtmpl);
		g_gotoClone = reinterpret_cast<pfn_clone>(gbase[kSlotClone]);
		for (int i = 0; i < kVtblSlots; ++i) g_gotoVtbl[i] = gbase[i];
		g_gotoVtbl[kSlotLoad]    = reinterpret_cast<void*>(&GoToPointLoad);
		g_gotoVtbl[kSlotClone]   = reinterpret_cast<void*>(&GoToPointClone);
		g_gotoVtbl[kSlotProcess] = reinterpret_cast<void*>(&GoToPointProcess);  // own animal-aware movement

		// Human-NPC perception nodes (issue #133) — Stopped-based (+0x40 param).
		for (int i = 0; i < kVtblSlots; ++i) g_setTargetVtbl[i] = base[i];
		g_setTargetVtbl[kSlotLoad]    = reinterpret_cast<void*>(&SetTargetLoad);
		g_setTargetVtbl[kSlotClone]   = reinterpret_cast<void*>(&SetTargetClone);
		g_setTargetVtbl[kSlotProcess] = reinterpret_cast<void*>(&SetTargetProcess);

		for (int i = 0; i < kVtblSlots; ++i) g_hasTargetVtbl[i] = base[i];
		g_hasTargetVtbl[kSlotClone]   = reinterpret_cast<void*>(&HasTargetClone);
		g_hasTargetVtbl[kSlotProcess] = reinterpret_cast<void*>(&HasTargetProcess);

		for (int i = 0; i < kVtblSlots; ++i) g_targetRangeVtbl[i] = base[i];
		g_targetRangeVtbl[kSlotLoad]    = reinterpret_cast<void*>(&TargetRangeLoad);
		g_targetRangeVtbl[kSlotClone]   = reinterpret_cast<void*>(&TargetRangeClone);
		g_targetRangeVtbl[kSlotProcess] = reinterpret_cast<void*>(&TargetRangeProcess);

		for (int i = 0; i < kVtblSlots; ++i) g_cooldownVtbl[i] = base[i];
		g_cooldownVtbl[kSlotLoad]    = reinterpret_cast<void*>(&CooldownLoad);
		g_cooldownVtbl[kSlotClone]   = reinterpret_cast<void*>(&CooldownClone);
		g_cooldownVtbl[kSlotProcess] = reinterpret_cast<void*>(&CooldownProcess);

		// ReturnToHomePosition / ChaseTarget / FleeFromTarget — GoToPosition-based
		// (need the move/dest layout: dest +0x40/+0x44/+0x48, moving flag +0x4c).
		for (int i = 0; i < kVtblSlots; ++i) g_returnHomeVtbl[i] = gbase[i];
		g_returnHomeVtbl[kSlotLoad]    = reinterpret_cast<void*>(&ReturnHomeLoad);
		g_returnHomeVtbl[kSlotClone]   = reinterpret_cast<void*>(&ReturnHomeClone);
		g_returnHomeVtbl[kSlotProcess] = reinterpret_cast<void*>(&ReturnHomeProcess);

		for (int i = 0; i < kVtblSlots; ++i) g_chaseVtbl[i] = gbase[i];
		g_chaseVtbl[kSlotLoad]    = reinterpret_cast<void*>(&ChaseLoad);
		g_chaseVtbl[kSlotClone]   = reinterpret_cast<void*>(&ChaseClone);
		g_chaseVtbl[kSlotProcess] = reinterpret_cast<void*>(&ChaseProcess);

		for (int i = 0; i < kVtblSlots; ++i) g_fleeVtbl[i] = gbase[i];
		g_fleeVtbl[kSlotLoad]    = reinterpret_cast<void*>(&FleeLoad);
		g_fleeVtbl[kSlotClone]   = reinterpret_cast<void*>(&FleeClone);
		g_fleeVtbl[kSlotProcess] = reinterpret_cast<void*>(&FleeProcess);

		// FaceTarget — Stopped-based (no params, no move layout); reuse the
		// template loader, override clone + process only.
		for (int i = 0; i < kVtblSlots; ++i) g_faceVtbl[i] = base[i];
		g_faceVtbl[kSlotClone]   = reinterpret_cast<void*>(&FaceClone);
		g_faceVtbl[kSlotProcess] = reinterpret_cast<void*>(&FaceProcess);

		// AttackTarget — GoToPosition-based (needs room for 3 params at +0x40..+0x48).
		for (int i = 0; i < kVtblSlots; ++i) g_attackVtbl[i] = gbase[i];
		g_attackVtbl[kSlotLoad]    = reinterpret_cast<void*>(&AttackLoad);
		g_attackVtbl[kSlotClone]   = reinterpret_cast<void*>(&AttackClone);
		g_attackVtbl[kSlotProcess] = reinterpret_cast<void*>(&AttackProcess);

		bool ok = true;
		ok &= RegisterUnder(createByName, registerNode, factory, "Stopped",      "LifxLogNode",              g_logVtbl);
		ok &= RegisterUnder(createByName, registerNode, factory, "Stopped",      "TimeOfDayBetween",         g_todVtbl);
		ok &= RegisterUnder(createByName, registerNode, factory, "Stopped",      "IsNight",                  g_todVtbl);
		ok &= RegisterUnder(createByName, registerNode, factory, "GoToPosition", "GoToPoint",                g_gotoVtbl);
		ok &= RegisterUnder(createByName, registerNode, factory, "Stopped",      "SetNearestPlayerAsTarget", g_setTargetVtbl);
		ok &= RegisterUnder(createByName, registerNode, factory, "Stopped",      "HasPlayerTarget",          g_hasTargetVtbl);
		ok &= RegisterUnder(createByName, registerNode, factory, "Stopped",      "TargetInRange",            g_targetRangeVtbl);
		ok &= RegisterUnder(createByName, registerNode, factory, "Stopped",      "CooldownGate",             g_cooldownVtbl);
		ok &= RegisterUnder(createByName, registerNode, factory, "GoToPosition", "ReturnToHomePosition",     g_returnHomeVtbl);
		ok &= RegisterUnder(createByName, registerNode, factory, "GoToPosition", "ChaseTarget",             g_chaseVtbl);
		ok &= RegisterUnder(createByName, registerNode, factory, "GoToPosition", "FleeFromTarget",          g_fleeVtbl);
		ok &= RegisterUnder(createByName, registerNode, factory, "Stopped",      "FaceTarget",              g_faceVtbl);
		ok &= RegisterUnder(createByName, registerNode, factory, "GoToPosition", "AttackTarget",            g_attackVtbl);
		if (!ok)
			return false;

		Con::Echo("[lifx-ai] registered behavior node classes: LifxLogNode, TimeOfDayBetween, "
		          "IsNight, GoToPoint, SetNearestPlayerAsTarget, HasPlayerTarget, TargetInRange, "
		          "CooldownGate, ReturnToHomePosition, ChaseTarget, FleeFromTarget, FaceTarget, "
		          "AttackTarget");
		return true;
	}
}

// ---------------------------------------------------------------------------- //
void* __fastcall Hooks::AI::LoadBehaviorXml(void* self, const char* fileName)
{
	// Register our custom node(s) before any tree's nodes are parsed. Runs at
	// most once successfully; retried on later loads if the first attempt was
	// too early (built-ins not yet registered).
	if (!g_registered.load(std::memory_order_acquire))
	{
		std::lock_guard<std::mutex> lk(g_regMtx);
		if (!g_registered.load(std::memory_order_relaxed) && TryRegisterCustomNodes())
			g_registered.store(true, std::memory_order_release);
	}

	return _Ai_LoadBehaviorXml(self, fileName);
}
