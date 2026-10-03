
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

#include "cm_server.h"
#include "core/tinyxml2.h"
#include "hooks/furnace/hook_proc_desc.h"
#include "hooks/furnace/hook_working_furnace_tick.h"
#include "hooks/furnace/hook_brewing_tank_tick.h"
#include "hooks/furnace/hook_brewing_tank_desc.h"
#include "hooks/furnace/hook_working_fire_tick.h"
#include "hooks/furnace/hook_working_greenhouse_tick.h"
#include "hooks/furnace/hook_working_trap_tick.h"
#include "hooks/furnace/hook_working_windmill_tick.h"
#include "hooks/character/hook_vital_process_tick.h"
#include "hooks/character/hook_calc_hit_damage.h"
#include "hooks/character/hook_wounds_deal_damage.h"
#include "hooks/character/hook_send_changes.h"
#include "hooks/character/hook_apply_damage.h"
#include "hooks/character/hook_onepunchman.h"
#include "hooks/character/hook_set_control_object.h"
#include "hooks/character/hook_npcdec_pack.h"
#include "hooks/engine/hook_recipe_starting_tools.h"
#include "hooks/engine/hook_workshop_buff.h"
#include "hooks/engine/hook_greenhouse_alias.h"
#include "hooks/engine/hook_stable_alias.h"
#include "hooks/engine/hook_cart_places.h"
#include "hooks/engine/hook_datablock_range.h"
#include "hooks/engine/hook_craft_effects_range.h"
#include "hooks/engine/hook_crop_types.h"
#include "hooks/engine/hook_well_water.h"
#include "hooks/engine/hook_drink_effects.h"
#include "hooks/engine/hook_herb_garden_gate.h"
#include "hooks/engine/hook_gem_drop.h"
#include "hooks/engine/hook_tunnel_drop.h"
#include "hooks/engine/hook_tree_drop.h"

// A2a #125 equip-over-ghost transport. The intercept is now a VTABLE-SLOT
// PATCH on NPCDecorative's packUpdate slot (Hooks::NpcDecPack::InstallVtablePatch),
// NOT a Detours prologue patch on the shared ShapeBase::packUpdate — the latter
// raced world-load worker threads and hung the server. Set to 0 to build a
// pure-isolation server with no equip intercept at all.
#ifndef LIFX_EQUIP_PACK_HOOK
#define LIFX_EQUIP_PACK_HOOK 0
#endif
#include "hooks/outpost/hook_outpost_default_radius.h"
#include "hooks/outpost/hook_outpost_proximity.h"
#include "hooks/battlezone/hook_battlezone_containment.h"
#include "hooks/effect/hook_effect_parse.h"
#include "hooks/effect/hook_assign_effect.h"
#include "hooks/effect/hook_broadcast_effects.h"
#include "hooks/netevent/hook_netclassrep_dumper.h"
#include "hooks/netevent/sector_handoff_event.h"
#include "hooks/sector/sector_edge_trigger.h"
#include "hooks/sector/world_grid.h"
#include "hooks/dispatcher/dispatcher_client.h"
#include "hooks/ai/hook_behavior_node.h"
#include "hooks/ability/hook_light_working_object.h"
#include "hooks/ability/hook_register_perform.h"
#include "hooks/ability/hook_resolve_light_object.h"

Lifx::Server* Lifx::Server::instance_{ nullptr };
std::mutex Lifx::Server::instance_guard_;

// ---------------------------------------------------------------------------- //
void Lifx::Server::Init()
{
	if (is_init_)
		return;

	// here we load our low-level subsystems like Network, Threading e.t.c
	// it's must be done BEFORE Torque init Console and call scripts (with CM server initialization)!

	// Resolve config/lifxpluss.xml relative to the EXE's own directory rather
	// than the process's current working directory -- LoadFile("config/...")
	// was silently failing here because whatever sets up CWD by the time this
	// runs no longer points at the server install dir. GetModuleFileNameA(NULL, ...)
	// gives the exe's real path regardless of CWD.
	char exePath[MAX_PATH]{};
	GetModuleFileNameA(nullptr, exePath, MAX_PATH);
	std::string configPath(exePath);
	const auto lastSlash = configPath.find_last_of("\\/");
	configPath = (lastSlash == std::string::npos)
		? "config/lifxpluss.xml"
		: configPath.substr(0, lastSlash + 1) + "config/lifxpluss.xml";

	tinyxml2::XMLDocument xml_doc;
	const tinyxml2::XMLError e = xml_doc.LoadFile(configPath.c_str());
	if (e != tinyxml2::XML_SUCCESS)
	{
		Lifx::ShowErrorMessage("Can't load lifxpluss.xml (Error code: %d)", static_cast<int>(e));
		// todo: terminate process
		return;
	}

	// setup logger
	if (auto* el = xml_doc.RootElement()->FirstChildElement("resurrectionDurationMs"))
	{
		const std::uint32_t ms = static_cast<std::uint32_t>(el->IntText(0));
		Hooks::Effect::g_resurrectionDurationMs.store(ms, std::memory_order_relaxed);
	}

	config_.LogLevel = xml_doc.RootElement()->FirstChildElement("log")->FirstChildElement("level")->IntText(0);
	config_.SkipConsoleSQLLogging = xml_doc.RootElement()->FirstChildElement("log")->FirstChildElement("skipSQLQueriesLog")->BoolText();
	config_.UseExternalErrorLog = xml_doc.RootElement()->FirstChildElement("log")->FirstChildElement("enableExternalErrorLog")->BoolText();
	config_.UseExternalSQLLog = xml_doc.RootElement()->FirstChildElement("log")->FirstChildElement("enableExternalSqlLog")->BoolText();

	// WorldID — engine-side; drives the DB name (USE `lif_${WorldID}`),
	// log file names, and server lock file. May be SHARED across federated
	// instances when they all read/write the same MySQL DB.
	if (auto* el = xml_doc.RootElement()->FirstChildElement("worldID"))
	{
		const int v = el->IntText(0);
		if (v > 0) config_.WorldID = static_cast<std::uint32_t>(v);
	}

	// DispatcherWorldID — LiFx-side; drives the deterministic dispatcher
	// peer UUID. MUST differ between federated instances. Defaults to
	// WorldID when omitted (single-instance setups don't need to know
	// about this knob).
	if (auto* el = xml_doc.RootElement()->FirstChildElement("dispatcherWorldID"))
	{
		const int v = el->IntText(0);
		if (v > 0) config_.DispatcherWorldID = static_cast<std::uint32_t>(v);
	}
	if (config_.DispatcherWorldID == 0) config_.DispatcherWorldID = config_.WorldID;

	if (auto* el = xml_doc.RootElement()->FirstChildElement("dumpNetClassRep"))
	{
		config_.DumpNetClassRep = el->BoolText();
	}

	if (auto* el = xml_doc.RootElement()->FirstChildElement("registerSectorHandoff"))
	{
		config_.RegisterSectorHandoff = el->BoolText();
	}
	else if (auto* el2 = xml_doc.RootElement()->FirstChildElement("RegisterSectorHandoff"))
	{
		// Tolerated alias — the C++ field is PascalCase so it's a natural typo
		// when hand-editing the XML.
		config_.RegisterSectorHandoff = el2->BoolText();
	}

	if (auto* el = xml_doc.RootElement()->FirstChildElement("sectorHandoffAutoPost"))
	{
		config_.SectorHandoffAutoPost = el->BoolText();
	}
	else if (auto* el2 = xml_doc.RootElement()->FirstChildElement("SectorHandoffAutoPost"))
	{
		config_.SectorHandoffAutoPost = el2->BoolText();
	}

	if (auto* el = xml_doc.RootElement()->FirstChildElement("sectorHandoffTargetPeer"))
	{
		const char* t = el->GetText();
		if (t && *t) config_.SectorHandoffTargetPeer = t;
	}
	if (auto* el = xml_doc.RootElement()->FirstChildElement("sectorHandoffReceiveLog"))
	{
		config_.SectorHandoffReceiveLog = el->BoolText();
	}
	if (auto* el = xml_doc.RootElement()->FirstChildElement("sectorHandoffInjectOnReceive"))
	{
		config_.SectorHandoffInjectOnReceive = el->BoolText();
	}

	if (auto* el = xml_doc.RootElement()->FirstChildElement("dispatcherEnabled"))
	{
		config_.DispatcherEnabled = el->BoolText();
	}
	if (auto* el = xml_doc.RootElement()->FirstChildElement("dispatcherHost"))
	{
		const char* t = el->GetText();
		if (t && *t) config_.DispatcherHost = t;
	}
	if (auto* el = xml_doc.RootElement()->FirstChildElement("dispatcherPort"))
	{
		const int p = el->IntText(0);
		if (p > 0 && p < 65536) config_.DispatcherPort = static_cast<std::uint16_t>(p);
	}
	if (auto* el = xml_doc.RootElement()->FirstChildElement("sectorClaims"))
	{
		const char* t = el->GetText();
		if (t) config_.SectorClaims = t;
	}

	// Drop/recipe hooks configured from their own XML sections. Each returns
	// false only on a malformed section (it has already reported the reason);
	// a missing or disabled section is success and leaves vanilla behaviour.
	// The hooks are attached later, from AttachHooks().
	{
		const tinyxml2::XMLElement* root = xml_doc.RootElement();
		Hooks::Engine::ConfigureRecipeStartingTools(root);
		Hooks::Engine::ConfigureWorkshopBuff(root);
		Hooks::Engine::ConfigureGreenhouseAlias(root);
		Hooks::Engine::ConfigureStableAlias(root);
		Hooks::Engine::ConfigureCartPlaces(root);
		Hooks::Engine::ConfigureDatablockRange(root);
		Hooks::Engine::ConfigureCraftEffectsRange(root);
		Hooks::Engine::ConfigureCropTypes(root);
		Hooks::Engine::ConfigureWellWater(root);
		Hooks::Engine::ConfigureDrinkEffects(root);
		Hooks::Engine::ConfigureHerbGardenGate(root);
		Hooks::Engine::ConfigureGemDrops(root);
		Hooks::Engine::ConfigureTunnelDrops(root);
		Hooks::Engine::ConfigureTreeDrops(root);
	}

	std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
	char ft_buf[100], fd_buf[100] = { 0 };

	std::strftime(ft_buf, sizeof(ft_buf), "%Y-%m-%d", std::localtime(&now));
	std::strftime(fd_buf, sizeof(fd_buf), "%Y-%m-%d-%H-%M-%S", std::localtime(&now));

	if (config_.UseExternalErrorLog)
	{
		auto fp = std::format("logs/{}/errors", ft_buf);
		if (!std::filesystem::exists(fp))
		{
			std::filesystem::create_directories(fp);
		}

		// todo: get NetBios name for full compatible with CM_YO log format (see https://learn.microsoft.com/en-us/windows/win32/sysinfo/getting-system-information)
		// warn: here we has 1 as WorldID
		auto lp = std::format("{}/S{}_{}p{}.log", fp, config_.WorldID, fd_buf, GetCurrentProcessId());
		std::ofstream out(lp, std::ios::app);
		if (out.is_open())
		{
			std::stringstream ss;
			std::strftime(ft_buf, sizeof(ft_buf), "%d/%m/%Y", std::localtime(&now));
			ss << "// -- " << ft_buf;
			std::strftime(ft_buf, sizeof(ft_buf), "%H:%M:%S", std::localtime(&now));
			ss << " -- " << ft_buf << " ======== " << kCoreVersionString << " // CONSOLE ERROR REPORT " << std::endl;
			out << ss.str();
		}
		out.close();
		config_.LogErrorFN = lp;
	}

	std::strftime(ft_buf, sizeof(ft_buf), "%Y-%m-%d", std::localtime(&now));

	if (config_.UseExternalSQLLog)
	{
		auto fp = std::format("logs/{}/sql", ft_buf);
		if (!std::filesystem::exists(fp))
		{
			std::filesystem::create_directories(fp);
		}

		auto lp = std::format("{}/S{}_{}p{}.log", fp, config_.WorldID, fd_buf, GetCurrentProcessId());
		std::ofstream out(lp, std::ios::app);
		if (out.is_open())
		{
			std::stringstream ss;
			std::strftime(ft_buf, sizeof(ft_buf), "%d/%m/%Y", std::localtime(&now));
			ss << "// -- " << ft_buf;
			std::strftime(ft_buf, sizeof(ft_buf), "%H:%M:%S", std::localtime(&now));
			ss << " -- " << ft_buf << " ======== " << kCoreVersionString << " // SQL QUERIES REPORT " << std::endl;
			out << ss.str();
		}
		out.close();
		config_.LogSQLFN = lp;
	}

	// attach main low-level core hooks
	Hooks::AttachHooks();

	// Chunk 15a (#103): parse <worldGrid> + <worldNeighbours> into the
	// WorldGrid module. Replaces chunk-14's <sectorEdgeTriggers> XML
	// (deleted along with SectorEdge::Configure). No Con::Echo here — we
	// run inside DllMain, and the engine console cs isn't initialised
	// yet; LogConfigured() is called from Server::Start instead.
	{
		auto parseGrid = [](tinyxml2::XMLElement* el, Hooks::WorldGrid::GridSpec& out) {
			if (!el) return false;
			const char* originStr = el->Attribute("origin");
			float ox = 0.f, oy = 0.f;
			if (originStr) sscanf(originStr, "%f,%f", &ox, &oy);
			out.originX     = ox;
			out.originY     = oy;
			out.cellSize    = el->FloatAttribute("cellSize", 0.f);
			out.firstSector = el->UnsignedAttribute("firstSector", 0);
			const char* gridStr = el->Attribute("gridSize");
			unsigned cols = 0, rows = 0;
			if (gridStr) sscanf(gridStr, "%ux%u", &cols, &rows);
			out.cols = cols;
			out.rows = rows;
			return out.cellSize > 0.f && out.cols > 0 && out.rows > 0 && out.firstSector > 0;
		};

		Hooks::WorldGrid::GridSpec self{};
		std::vector<Hooks::WorldGrid::Neighbour> ns;

		if (auto* sg = xml_doc.RootElement()->FirstChildElement("worldGrid"))
			parseGrid(sg, self);

		if (auto* nblock = xml_doc.RootElement()->FirstChildElement("worldNeighbours"))
		{
			for (auto* el = nblock->FirstChildElement("neighbour"); el;
			     el = el->NextSiblingElement("neighbour"))
			{
				Hooks::WorldGrid::Neighbour nb{};
				if (!parseGrid(el, nb.grid)) continue;
				const char* dirStr = el->Attribute("direction");
				if (!dirStr) continue;
				if      (dirStr[0] == 'n' || dirStr[0] == 'N') nb.dir = Hooks::WorldGrid::Direction::North;
				else if (dirStr[0] == 's' || dirStr[0] == 'S') nb.dir = Hooks::WorldGrid::Direction::South;
				else if (dirStr[0] == 'e' || dirStr[0] == 'E') nb.dir = Hooks::WorldGrid::Direction::East;
				else if (dirStr[0] == 'w' || dirStr[0] == 'W') nb.dir = Hooks::WorldGrid::Direction::West;
				else continue;
				if (const char* h = el->Attribute("peerHost")) nb.peerHost = h;
				nb.peerPort = static_cast<std::uint16_t>(
					el->UnsignedAttribute("peerPort", 0));
				ns.push_back(std::move(nb));
			}
		}

		Hooks::WorldGrid::Configure(self, std::move(ns));
	}

	is_init_ = true;
}

// ---------------------------------------------------------------------------- //
void Lifx::Server::Destroy()
{
	// detach server hooks
	DetachHooks();

	// detach core hooks
	Hooks::DetachHooks();
}

// ---------------------------------------------------------------------------- //
void Lifx::Server::Start()
{
	// attach internal server (cm_yo) hooks
	AttachHooks();

	// Now safe to emit the deferred world-grid config log: ConsoleInit
	// has run, so Con::Echo no longer hangs on the engine console cs.
	Hooks::WorldGrid::LogConfigured();
}

// ---------------------------------------------------------------------------- //
void Lifx::Server::Stop()
{
	// here we must stop CM server ...

	// ..then destroy LiFx and detach all hooks
	Destroy();
}

// ---------------------------------------------------------------------------- //
void Lifx::Server::AttachHooks()
{
	// Wrap in an explicit Detours transaction. We're called from inside the
	// Con_Init hook handler — outside the DllMain transaction — so without
	// this every DetourAttach below would be a no-op.
	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());

	// Descriptor lookups (one per descriptor table the engine maintains).
	__CM_ATTACH_HOOK(CmOffset::FURNACE_PROC_DESC_LOOKUP,
	                 _Furnace_LookupProcDesc,
	                 Hooks::Furnace::ProcDescLookup);
	__CM_ATTACH_HOOK(CmOffset::BREWING_TANK_PROC_DESC_LOOKUP,
	                 _BrewingTankFurnace_LookupProcDesc,
	                 Hooks::BrewingTankFurnace::ProcDescLookup);

	// Per-class recalcTick hooks (one per concrete craftwork class).
	__CM_ATTACH_HOOK(CmOffset::WORKING_FURNACE_RECALC_TICK,
	                 _WorkingFurnace_RecalcTick,
	                 Hooks::WorkingFurnace::RecalcTick);
	__CM_ATTACH_HOOK(CmOffset::BREWING_TANK_RECALC_TICK,
	                 _BrewingTankFurnace_RecalcTick,
	                 Hooks::BrewingTankFurnace::RecalcTick);
	__CM_ATTACH_HOOK(CmOffset::WORKING_FIRE_RECALC_TICK,
	                 _WorkingFire_RecalcTick,
	                 Hooks::WorkingFire::RecalcTick);
	__CM_ATTACH_HOOK(CmOffset::WORKING_GREENHOUSE_RECALC_TICK,
	                 _WorkingGreenhouse_RecalcTick,
	                 Hooks::WorkingGreenhouse::RecalcTick);
	__CM_ATTACH_HOOK(CmOffset::WORKING_TRAP_RECALC_TICK,
	                 _WorkingTrap_RecalcTick,
	                 Hooks::WorkingTrap::RecalcTick);
	__CM_ATTACH_HOOK(CmOffset::WORKING_WINDMILL_RECALC_TICK,
	                 _WorkingWindmill_RecalcTick,
	                 Hooks::WorkingWindmill::RecalcTick);

	// Ability id 268 "Light the Fire" -- observation probe, see
	// hooks/ability/hook_light_working_object.cpp.
	__CM_ATTACH_HOOK(CmOffset::LIGHTWORKINGOBJECT_ONDOPERFORM,
	                 _LightWorkingObject_OnDoPerform,
	                 Hooks::LightWorkingObject::OnDoPerform);

	// Generic ability-dispatch probe -- see hooks/ability/hook_register_perform.cpp.
	// Only logs when abilityId==268, so this is silent for every other ability.
	__CM_ATTACH_HOOK(CmOffset::SERVERMANAGER_REGISTERPERFORM,
	                 _ServerManager_RegisterPerform,
	                 Hooks::ServerManager::RegisterPerform);

	// Observation probe -- see hooks/ability/hook_resolve_light_object.cpp.
	__CM_ATTACH_HOOK(CmOffset::LIGHTON_RESOLVE_LIGHT_OBJECT,
	                 _LightOn_ResolveLightObject_Orig,
	                 Hooks::LightOnResolve::Call);

	// Telemetry hook on the HP processor — used to identify which fields
	// move when a player takes damage. Once the right field is known this
	// can be removed.
	__CM_ATTACH_HOOK(CmOffset::VITALPARAMS_PROCESS_TICK,
	                 _VitalParams_ProcessTick,
	                 Hooks::VitalParams::ProcessTick);
	Con::Echo("[lifx-vp] hook attached (Process_tick telemetry active)");

	// Empirical damage-path observation. See docs/character_hp.md.
	__CM_ATTACH_HOOK(CmOffset::CHAR_CALC_HIT_DAMAGE,
	                 _Char_CalcHitDamage,
	                 Hooks::CharCalcHitDamage::Call);
	Con::Echo("[lifx-hit] hook attached");

	// CmCharacterWounds::dealDamage — confirmed real damage entry point.
	// Captures `this` so a Lifx command can re-invoke dealDamage for a known
	// player on demand.
	__CM_ATTACH_HOOK(CmOffset::WOUNDS_DEAL_DAMAGE,
	                 _Wounds_DealDamage,
	                 Hooks::WoundsDealDamage::Call);
	Con::Echo("[lifx-wound] hook attached");

	// CmCharacterInfo::_sendChanges — the server→client broadcast. Logs
	// every call's mask + sendNow and emits a delta of HP-window fields
	// (+0x180..+0x200) that changed during the call. See hook_send_changes.h.
	__CM_ATTACH_HOOK(CmOffset::CHARINFO_SEND_CHANGES,
	                 _CharInfo_SendChanges,
	                 Hooks::CharInfoSendChanges::Call);
	Con::Echo("[lifx-send] hook attached");

	// Suspected HP-apply step: called from Player::_applyHit right after
	// dealDamage with the damage packet ONEPUNCHMAN computed. See
	// hook_apply_damage.h.
	__CM_ATTACH_HOOK(CmOffset::HIT_APPLY_DAMAGE,
	                 _Hit_ApplyDamage,
	                 Hooks::HitApplyDamage::Call);
	Con::Echo("[lifx-apply] hook attached");

	// ONEPUNCHMAN damage calculator — the real combat path, carrying BOTH
	// attacker and defender ctx. Drives the per-player PvP pacifist toggle
	// (Lifx::setPacifist): a flagged player's hits on other players are
	// zeroed here. See hook_onepunchman.h.
	__CM_ATTACH_HOOK(CmOffset::ONEPUNCHMAN_DAMAGE_CALC,
	                 _OnePunchMan,
	                 Hooks::OnePunchMan::Call);
	Con::Echo("[lifx-punch] hook attached at RVA 0x%X", (unsigned)CmOffset::ONEPUNCHMAN_DAMAGE_CALC);

	// GameConnection::setControlObject — captures live Player* per
	// connection without depending on the broken charID-stamp scan.
	// Issue #99 / chunk 13a.
	__CM_ATTACH_HOOK(CmOffset::SET_CONTROL_OBJECT,
	                 _GC_SetControlObject,
	                 Hooks::SetControlObject::Call);
	Con::Echo("[lifx-ctrl] hook attached");

	// Default outpost radius (RVA 0x187360, `return 20;`). Hook returns
	// whatever Lifx::setOutpostDefaultRadius wrote, falling back to 20.
	__CM_ATTACH_HOOK(CmOffset::OUTPOST_DEFAULT_RADIUS_GETTER,
	                 _Outpost_DefaultRadiusGetter,
	                 Hooks::Outpost::DefaultRadiusGetter);
	Con::Echo("[lifx-outpost] default-radius hook attached");

	// Monument↔monument and outpost↔outpost min-distance const-returners.
	__CM_ATTACH_HOOK(CmOffset::MONUMENT_MIN_DISTANCE_GETTER,
	                 _MonumentMinDistanceGetter,
	                 Hooks::Outpost::MonumentMinDistanceGetter);
	__CM_ATTACH_HOOK(CmOffset::OUTPOST_OUTPOST_MIN_DISTANCE_GETTER,
	                 _OutpostOutpostMinDistanceGetter,
	                 Hooks::Outpost::OutpostOutpostMinDistanceGetter);

	// Call-site retargets for the two personal-claim distance checks. Not
	// part of the Detours transaction — these patch a `call rel32` inside
	// existing functions rather than installing a function-entry trampoline.
	Hooks::Outpost::PatchPersonalClaimCallSites();

	// Battlezone containment (see docs/battlezones.md + hook header). Two
	// cooperating detours: _checkSteps stamps the current Player* so the
	// isActiveStartingZone gate can apply per-player exemptions (Lifx::
	// setBattleZoneExempt) and fire the throttled "Return to fight!" TS
	// callback. With no exemptions set and the TS callback absent, behaviour
	// is identical to the stock engine.
	__CM_ATTACH_HOOK(CmOffset::PLAYER_CHECK_STEPS,
	                 _Player_CheckSteps,
	                 Hooks::BattleZone::CheckStepsCall);
	__CM_ATTACH_HOOK(CmOffset::ISACTIVE_STARTING_ZONE,
	                 _Lands_IsActiveStartingZone,
	                 Hooks::BattleZone::IsActiveStartingZoneCall);
	Con::Echo("[lifx-battlezone] containment hooks attached (_checkSteps 0x%X, isActiveStartingZone 0x%X)",
	          (unsigned)CmOffset::PLAYER_CHECK_STEPS, (unsigned)CmOffset::ISACTIVE_STARTING_ZONE);

	// Effect XML parser (FUN_1404dd100). Proof-of-life passthrough — logs
	// entry/exit, no behavioral change. First gameplay-side anchor for the
	// effect subsystem; see docs/effects_and_abilities.md.
	__CM_ATTACH_HOOK(CmOffset::EFFECT_PARSE,
	                 _Effect_Parse,
	                 Hooks::Effect::Parse);
	Con::Echo("[lifx-effect] parser hook attached");

	// Player::BroadcastEffectDelta — the chokepoint for server-side effect
	// changes going out to clients. Rewrites Resurrected duration to the
	// configured value (see lifxpluss.xml <resurrectionDurationMs> and the
	// LifxTimers::resurrection TS setter). Note: Assign_effect (0x4DC810)
	// would be the wrong target — it's only called by clients receiving
	// the delta, never by the server's apply path.
	__CM_ATTACH_HOOK(CmOffset::OBJEFFECTS_BROADCAST_DELTA,
	                 _ObjEffects_BroadcastDelta,
	                 Hooks::Effect::BroadcastDelta);
	Con::Echo("[lifx-effect] broadcast-delta hook attached (resurrection override = %u ms)",
	          (unsigned)Hooks::Effect::g_resurrectionDurationMs.load());

	// One-shot NetEvent ABI dumper. Off by default; runs once per boot when
	// <dumpNetClassRep>1</dumpNetClassRep> is set in lifxpluss.xml. See
	// docs/netevent_abi.md and issue #54.
	if (config_.DumpNetClassRep)
	{
		__CM_ATTACH_HOOK(CmOffset::NETCLASSREP_ADD,
		                 _NetClassRep_Add,
		                 Hooks::NetEvent::NetClassRepAdd);
		__CM_ATTACH_HOOK(CmOffset::SERVERUUIDEVENT_SEND,
		                 _ServerUUIDEvent_Send,
		                 Hooks::NetEvent::ServerUUIDEventSend);
		Con::Echo("[lifx-netevent] dumper hooks attached -> logs/netclassrep_dump.log");
		// Snapshot every classRep already registered (most register via
		// static initializers that ran long before our hook attached).
		// Safe to call before the transaction commits — only reads the
		// engine's classRep list head, no hooked fn invocations.
		Hooks::NetEvent::DumpExistingList();
	}

	// SectorHandoff auto-post requires the SERVERUUIDEVENT_SEND hook to be
	// installed — but Detours doesn't allow two attaches on the same fn in
	// one transaction (we crashed the engine that way in a previous build,
	// see PR #63 follow-up). So if the dumper isn't already on we install
	// the dumper hook here to provide the trigger; the dumper's hook body
	// checks the auto-post flag at runtime.
	if (config_.SectorHandoffAutoPost && !config_.DumpNetClassRep)
	{
		__CM_ATTACH_HOOK(CmOffset::SERVERUUIDEVENT_SEND,
		                 _ServerUUIDEvent_Send,
		                 Hooks::NetEvent::ServerUUIDEventSend);
		Con::Echo("[lifx-sector-handoff] auto-post: installed SERVERUUIDEVENT_SEND trigger hook (dumper off)");
	}
	if (config_.SectorHandoffAutoPost)
	{
		Con::Echo("[lifx-sector-handoff] auto-post enabled");
	}

	// AI behavior-tree node factory extension (issue #119). Hook the per-tree
	// XML loader so we can register the custom "LifxLogNode" node class before
	// any tree's nodes are parsed. This loader runs on both the boot load path
	// (onServerCreated) and the manual reloadBehaviorXml() console command, and
	// fires after the built-in node modules register (so our "Stopped" clone
	// template exists). The original loader then proceeds unchanged.
	__CM_ATTACH_HOOK(CmOffset::AI_LOAD_BEHAVIOR_XML,
	                 _Ai_LoadBehaviorXml,
	                 Hooks::AI::LoadBehaviorXml);
	Con::Echo("[lifx-ai] behavior-XML loader hook attached (registers LifxLogNode)");

	// A2a #125: shared ShapeBase packUpdate. Hook GATES on the NPCDecorative
	// vtable inside the body and appends a custom equip block only for NPC
	// ghosts (real players pass straight through). Resolve writeFlag (used by
	// the hook body, not itself hooked) first.
#if LIFX_EQUIP_PACK_HOOK
	// PROPER equip transport: resolve writeFlag (used by the hook body). The
	// packUpdate intercept itself is a VTABLE-SLOT PATCH installed AFTER this
	// transaction commits (see below) — NOT a Detours prologue patch on the
	// shared 0x0FC8B0, which raced world-load workers and hung the server.
	__CM_FIND(CmOffset::BITSTREAM_WRITEFLAG, _BitStream_WriteFlag);
	__CM_FIND(CmOffset::AI_TREE_PROCESS, _AiTree_Process);
#else
	Con::Echo("[lifx-equip] ISOLATION BUILD: packUpdate hook DISABLED (testing load hang)");
#endif

	// Drop/recipe hooks. Each is a no-op unless its XML section enabled it in
	// Init(), and each byte-pattern-checks its target before attaching, so a
	// mismatched server build reports and skips instead of corrupting code.
	// These call DetourAttach directly, so they must stay inside this
	// transaction.
	Hooks::Engine::AttachRecipeStartingToolsHook();
	Hooks::Engine::AttachWorkshopBuffHook();
	Hooks::Engine::AttachGreenhouseAliasHook();
	Hooks::Engine::AttachStableAliasHook();
		Hooks::Engine::AttachCartPlacesHook();
	Hooks::Engine::AttachDatablockRangeHook();
	Hooks::Engine::AttachCraftEffectsRangeHook();
	Hooks::Engine::AttachCropTypesHook();
		Hooks::Engine::AttachWellWaterHook();
		Hooks::Engine::AttachDrinkEffectsHook();
	Hooks::Engine::AttachHerbGardenGateHooks();
	Hooks::Engine::AttachGemDropHooks();
	Hooks::Engine::AttachTunnelDropHook();

	// Commit the transaction and surface success/failure. If commit fails,
	// none of the above hooks took effect — log loud so we don't waste hours.
	const LONG commitRc = DetourTransactionCommit();

	// Tree drops replace a 26-byte selector with a jump to a generated
	// trampoline rather than going through Detours, so it must NOT be inside
	// the transaction above.
	Hooks::Engine::AttachTreeDropHook();

	if (commitRc != NO_ERROR) {
		Con::Warning("[lifx] DetourTransactionCommit FAILED rc=%ld — none of the above hooks are active", (long)commitRc);
	} else {
		// Verify by reading the first byte of each hooked function. Detours
		// inserts a JMP (E9) over the original prologue when the patch lands.
		auto byteAt = [](unsigned rva) -> unsigned char {
			return *reinterpret_cast<unsigned char*>(
				reinterpret_cast<char*>(GetModuleHandle(nullptr)) + rva);
		};
		Con::Echo("[lifx] post-commit hook verification:");
		Con::Echo("  WOUNDS_DEAL_DAMAGE %s",
		          byteAt(CmOffset::WOUNDS_DEAL_DAMAGE) == 0xE9 ? "(patched)" : "(NOT patched)");
		Con::Echo("  CHARINFO_SEND_CHANGES %s",
		          byteAt(CmOffset::CHARINFO_SEND_CHANGES) == 0xE9 ? "(patched)" : "(NOT patched)");
		Con::Echo("  HIT_APPLY_DAMAGE %s",
		          byteAt(CmOffset::HIT_APPLY_DAMAGE) == 0xE9 ? "(patched)" : "(NOT patched)");
		Con::Echo("  CHAR_CALC_HIT_DAMAGE %s",
		          byteAt(CmOffset::CHAR_CALC_HIT_DAMAGE) == 0xE9 ? "(patched)" : "(NOT patched)");
		Con::Echo("  FURNACE_PROC_DESC_LOOKUP %s",
		          byteAt(CmOffset::FURNACE_PROC_DESC_LOOKUP) == 0xE9 ? "(patched)" : "(NOT patched)");
		// Added while debugging complete silence from the WorkingFire probe
		// (2026-09-11): __CM_ATTACH_HOOK never checks DetourAttach's return
		// value, so a failed attach on any of these is otherwise invisible.
		Con::Echo("  WORKING_FURNACE_RECALC_TICK %s",
		          byteAt(CmOffset::WORKING_FURNACE_RECALC_TICK) == 0xE9 ? "(patched)" : "(NOT patched)");
		Con::Echo("  BREWING_TANK_RECALC_TICK %s",
		          byteAt(CmOffset::BREWING_TANK_RECALC_TICK) == 0xE9 ? "(patched)" : "(NOT patched)");
		Con::Echo("  WORKING_FIRE_RECALC_TICK %s",
		          byteAt(CmOffset::WORKING_FIRE_RECALC_TICK) == 0xE9 ? "(patched)" : "(NOT patched)");
		Con::Echo("  WORKING_GREENHOUSE_RECALC_TICK %s",
		          byteAt(CmOffset::WORKING_GREENHOUSE_RECALC_TICK) == 0xE9 ? "(patched)" : "(NOT patched)");
		Con::Echo("  WORKING_TRAP_RECALC_TICK %s",
		          byteAt(CmOffset::WORKING_TRAP_RECALC_TICK) == 0xE9 ? "(patched)" : "(NOT patched)");
		Con::Echo("  WORKING_WINDMILL_RECALC_TICK %s",
		          byteAt(CmOffset::WORKING_WINDMILL_RECALC_TICK) == 0xE9 ? "(patched)" : "(NOT patched)");
		Con::Echo("  LIGHTWORKINGOBJECT_ONDOPERFORM %s",
		          byteAt(CmOffset::LIGHTWORKINGOBJECT_ONDOPERFORM) == 0xE9 ? "(patched)" : "(NOT patched)");
		Con::Echo("  SERVERMANAGER_REGISTERPERFORM %s",
		          byteAt(CmOffset::SERVERMANAGER_REGISTERPERFORM) == 0xE9 ? "(patched)" : "(NOT patched)");
		Con::Echo("  LIGHTON_RESOLVE_LIGHT_OBJECT %s",
		          byteAt(CmOffset::LIGHTON_RESOLVE_LIGHT_OBJECT) == 0xE9 ? "(patched)" : "(NOT patched)");
	}

#if LIFX_EQUIP_PACK_HOOK
	// Install the NPCDecorative packUpdate intercept as a VTABLE-SLOT PATCH
	// (outside any Detours transaction — it's a guarded pointer swap). Done
	// here, post-commit, on the main thread with the engine module mapped and
	// the console up. Verifies the slot before patching and bails on mismatch.
	Hooks::NpcDecPack::InstallVtablePatch();
	// #171 — same equip-over-ghost intercept on the native Animal vtable so our
	// bandits render armor. Per-instance gated (marker bit) since the Animal vtable
	// is shared by all wildlife. Verifies the slot before patching, bails on mismatch.
	Hooks::NpcDecPack::InstallAnimalVtablePatch();
#endif

	// SectorHandoff MVP registration (issue #58). Runs AFTER the dumper
	// hook attaches and AFTER the transaction commits — so if the dumper
	// is on, our own rep gets logged as a `[netclassrep] live #N` entry
	// in logs/netclassrep_dump.log, which is how we verify the
	// registration round-trip works.
	if (config_.RegisterSectorHandoff)
	{
		Hooks::SectorHandoff::Register();
	}

	// Dispatcher daemon hello/ack (issue #74). Spawned on a detached
	// thread so a missing or slow daemon can't pin engine init.
	if (config_.DispatcherEnabled)
	{
		// Register the delivery callback BEFORE spawning the session so
		// any inbound frame on the first connect is handled.
		if (config_.SectorHandoffReceiveLog || config_.SectorHandoffInjectOnReceive)
		{
			const bool log    = config_.SectorHandoffReceiveLog;
			const bool inject = config_.SectorHandoffInjectOnReceive;
			Hooks::Dispatcher::SetDeliveryCallback(
				[log, inject](const std::string& from,
				              const std::vector<std::uint8_t>& bytes) {
					if (log)    Hooks::SectorHandoff::DecodeAndLog(from, bytes);
					if (inject) Hooks::SectorHandoff::InjectFromWire(from, bytes);
				});
			Con::Echo("[sector-handoff] delivery callback registered (log=%d inject=%d)",
			          (int)log, (int)inject);
		}

		Hooks::Dispatcher::Config dc;
		dc.host     = config_.DispatcherHost;
		dc.port     = config_.DispatcherPort;
		dc.world_id = config_.DispatcherWorldID;

		// Parse <sectorClaims>442-450,500,505-510</...> into a flat vector.
		// Tolerant of whitespace, ignores malformed segments with a warning.
		{
			const std::string& s = config_.SectorClaims;
			std::size_t i = 0;
			while (i < s.size())
			{
				while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == ',')) ++i;
				if (i >= s.size()) break;
				std::size_t j = i;
				while (j < s.size() && s[j] != ',') ++j;
				std::string seg = s.substr(i, j - i);
				while (!seg.empty() && (seg.back() == ' ' || seg.back() == '\t')) seg.pop_back();
				i = j;

				const auto dash = seg.find('-');
				try
				{
					if (dash == std::string::npos)
					{
						dc.sector_claims.push_back(static_cast<std::uint32_t>(std::stoul(seg)));
					}
					else
					{
						const auto lo = static_cast<std::uint32_t>(std::stoul(seg.substr(0, dash)));
						const auto hi = static_cast<std::uint32_t>(std::stoul(seg.substr(dash + 1)));
						if (lo > hi || (hi - lo) > 1000)
						{
							Con::Warning("[dispatcher] sectorClaims: ignoring suspicious range %s", seg.c_str());
							continue;
						}
						for (std::uint32_t k = lo; k <= hi; ++k) dc.sector_claims.push_back(k);
					}
				}
				catch (...)
				{
					Con::Warning("[dispatcher] sectorClaims: ignoring malformed segment '%s'", seg.c_str());
				}
			}
		}

		Con::Echo("[dispatcher] connecting to %s:%u (world_id=%u sector_claims=%zu)",
		          dc.host.c_str(), (unsigned)dc.port,
		          (unsigned)dc.world_id, dc.sector_claims.size());
		Hooks::Dispatcher::SpawnConnect(dc);
	}
}

// ---------------------------------------------------------------------------- //
void Lifx::Server::DetachHooks()
{
#if LIFX_EQUIP_PACK_HOOK
	// Restore the NPCDecorative packUpdate vtable slot (a pointer swap, not a
	// Detours patch — do it outside the transaction).
	Hooks::NpcDecPack::RemoveVtablePatch();
#endif

	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());

	// Detach in reverse-attach order. The call-site rel32 patches are NOT
	// reverted on shutdown — the engine module is unmapping right after
	// anyway, and restoring would race with the same unmap.
	__CM_DETACH_HOOK(_Ai_LoadBehaviorXml,                Hooks::AI::LoadBehaviorXml);
	__CM_DETACH_HOOK(_ObjEffects_BroadcastDelta,         Hooks::Effect::BroadcastDelta);
	__CM_DETACH_HOOK(_Effect_Parse,                      Hooks::Effect::Parse);
	__CM_DETACH_HOOK(_OutpostOutpostMinDistanceGetter,   Hooks::Outpost::OutpostOutpostMinDistanceGetter);
	__CM_DETACH_HOOK(_MonumentMinDistanceGetter,         Hooks::Outpost::MonumentMinDistanceGetter);
	__CM_DETACH_HOOK(_Outpost_DefaultRadiusGetter,       Hooks::Outpost::DefaultRadiusGetter);
	__CM_DETACH_HOOK(_GC_SetControlObject,               Hooks::SetControlObject::Call);
	__CM_DETACH_HOOK(_Hit_ApplyDamage,                   Hooks::HitApplyDamage::Call);
	__CM_DETACH_HOOK(_CharInfo_SendChanges,              Hooks::CharInfoSendChanges::Call);
	__CM_DETACH_HOOK(_Wounds_DealDamage,                 Hooks::WoundsDealDamage::Call);
	__CM_DETACH_HOOK(_Char_CalcHitDamage,                Hooks::CharCalcHitDamage::Call);
	__CM_DETACH_HOOK(_VitalParams_ProcessTick,           Hooks::VitalParams::ProcessTick);
	__CM_DETACH_HOOK(_WorkingWindmill_RecalcTick,        Hooks::WorkingWindmill::RecalcTick);
	__CM_DETACH_HOOK(_LightOn_ResolveLightObject_Orig,   Hooks::LightOnResolve::Call);
	__CM_DETACH_HOOK(_ServerManager_RegisterPerform,     Hooks::ServerManager::RegisterPerform);
	__CM_DETACH_HOOK(_LightWorkingObject_OnDoPerform,    Hooks::LightWorkingObject::OnDoPerform);
	__CM_DETACH_HOOK(_WorkingTrap_RecalcTick,            Hooks::WorkingTrap::RecalcTick);
	__CM_DETACH_HOOK(_WorkingGreenhouse_RecalcTick,      Hooks::WorkingGreenhouse::RecalcTick);
	__CM_DETACH_HOOK(_WorkingFire_RecalcTick,            Hooks::WorkingFire::RecalcTick);
	__CM_DETACH_HOOK(_BrewingTankFurnace_RecalcTick,     Hooks::BrewingTankFurnace::RecalcTick);
	__CM_DETACH_HOOK(_WorkingFurnace_RecalcTick,         Hooks::WorkingFurnace::RecalcTick);
	__CM_DETACH_HOOK(_BrewingTankFurnace_LookupProcDesc, Hooks::BrewingTankFurnace::ProcDescLookup);
	__CM_DETACH_HOOK(_Furnace_LookupProcDesc,            Hooks::Furnace::ProcDescLookup);

	DetourTransactionCommit();
}