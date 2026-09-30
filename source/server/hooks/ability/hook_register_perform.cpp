/* ===================================================================================
	Copyright (c) 2026, LiFx Contributors. All Rights Reserved.
	Contributed by GreedyFox.

	This file is a part of LiFx.

	LIFX IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
	EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
	MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
	IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
	DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
	ARISING FROM, OUT OF OR IN CONNECTION WITH LIFX OR THE USE OR OTHER
	DEALINGS IN LIFX.
*  =================================================================================== */

#include "hook_register_perform.h"
#include <unordered_set>

__CM_INSTATNTIATE(_ServerManager_RegisterPerform);

namespace
{
	// Temporary instrumentation -- disable once ability id 268's real
	// concrete-class vtable is confirmed and a targeted hook replaces this
	// generic probe.
	constexpr bool kProbeEnabled = true;
	constexpr int kWatchAbilityId = 268;

	// 2026-09-14: broadened from "log only id 268" to "also log the first
	// sighting of every OTHER ability id that passes through this generic
	// dispatch entry point". Same observation-only contract as before --
	// still never touches the return value, still doesn't change 268's
	// existing detailed log line. This is how you capture an unknown
	// ability's id/name/vtable/entity without first knowing its RVA --
	// e.g. use the plow once in-game and grep the console/log for
	// "[lifx-probe] new ability=".
	std::unordered_set<int>& SeenAbilityIds()
	{
		static std::unordered_set<int> seen;
		return seen;
	}
}

// Lay Down (ability 192, repurposed emote): notify the Torque script `LiFxLayDown_onStart(charId)` right after the
// engine has registered the perform. charId = *(u32*)(abilityImp + 0x20) (see decompile of _registerPerform: "invalid player_id").
namespace { constexpr int kLayDownAbilityId = 192; }

unsigned long long Hooks::ServerManager::RegisterPerform(void* self, void* request)
{
	int layDownCharId = 0;
	if (request)
	{
		void* ab = *reinterpret_cast<void**>(request);
		if (ab)
		{
			void* desc = *reinterpret_cast<void**>(static_cast<char*>(ab) + 0x28);
			if (desc && *reinterpret_cast<int*>(static_cast<char*>(desc) + 8) == kLayDownAbilityId)
				layDownCharId = static_cast<int>(*reinterpret_cast<unsigned int*>(static_cast<char*>(ab) + 0x20));
		}
	}

	// Observation only -- does not affect the return value below.
	if constexpr (kProbeEnabled)
	{
		if (request)
		{
			void* ability = *reinterpret_cast<void**>(request);
			if (ability)
			{
				void* abilityDesc = *reinterpret_cast<void**>(static_cast<char*>(ability) + 0x28);
				if (abilityDesc)
				{
					int abilityId = *reinterpret_cast<int*>(static_cast<char*>(abilityDesc) + 8);
					const char* abilityName =
						*reinterpret_cast<const char**>(static_cast<char*>(abilityDesc) + 0x10);
					void* vtable = *reinterpret_cast<void**>(ability);
					// Same self+0x38 entity-field offset confirmed for both
					// LightOn and LightWorkingObject (shared AbilityBaseImp
					// layout) -- read speculatively here for any ability, since
					// the layout is shared across the family this hook fires
					// for. Treat it as unverified for an ability we haven't
					// individually confirmed yet.
					void* entity = *reinterpret_cast<void**>(static_cast<char*>(ability) + 0x38);

					if (abilityId == kWatchAbilityId)
					{
						Con::Echo("[lifx-light] registerPerform ability=%d (%s) self=%p vtable=%p entity=%p",
						          abilityId, abilityName ? abilityName : "?", ability, vtable, entity);
					}
					else if (SeenAbilityIds().insert(abilityId).second)
					{
						// First time this ability id has fired since boot --
						// one-line capture. Enough to identify which ability a
						// tool/object (e.g. the plow's till action) dispatches
						// through this entry point, and its concrete vtable for
						// a follow-up targeted hook (same pattern as
						// hook_light_working_object.cpp / hook_resolve_light_object.cpp).
						Con::Echo("[lifx-probe] new ability=%d (%s) self=%p vtable=%p entity=%p",
						          abilityId, abilityName ? abilityName : "?", ability, vtable, entity);
					}
				}
			}
		}
	}

	const unsigned long long rc = _ServerManager_RegisterPerform(self, request);
	if (layDownCharId > 0)
	{
		char script[96];
		snprintf(script, sizeof(script), "LiFxLayDown_onStart(%d);", layDownCharId);
		Con::Evaluate(script, false, "<LayDown::onStart>");
	}
	return rc;
}
