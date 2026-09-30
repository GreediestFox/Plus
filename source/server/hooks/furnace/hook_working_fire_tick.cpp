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

#include "hook_working_fire_tick.h"
#include "engine_internals.h"

#include <unordered_map>

__CM_INSTATNTIATE(_WorkingFire_RecalcTick);

// ============================================================================
// LIGHT-SOURCE PROBE (temporary instrumentation)
//
// Question being answered: do placed light sources (wall torch id 2500, small
// candle id 2502, braziers) tick through WorkingFire, or only campfires and
// hearths? If they do, WorkingFire::stateAlt (+0x28) is a burn-expiry
// timestamp we can write to turn a light on -- no ability call, no
// GameConnection.
//
// Behaviour is UNCHANGED: this only observes. The return value below is
// still the engine's own predicate.
//
// Console discipline: recalcTick fires per object per tick, so we log only
// (a) the first time we ever see a given object pointer, and (b) when an
// object's burning/expired state flips. Steady state is silent.
//
// Set kProbeEnabled to false (or delete this block) once the question is
// answered -- do not ship this enabled.
//
// Thread assumption: craftwork ticks run on the engine main thread, so the
// map below is accessed single-threaded. If that ever stops holding, this
// needs a lock.
// ============================================================================
namespace
{
	constexpr bool kProbeEnabled = true;

	// ========================================================================
	// DUSK ACTIVATION POLICY -- speculative, OFF BY DEFAULT.
	//
	// Do not enable until the probe above has confirmed, on a live server:
	//   (1) placed wall torches / braziers actually route through
	//       WorkingFire::recalcTick (unknown as of 2026-09-11 -- campfires and
	//       hearths are confirmed, torches are not);
	//   (2) the units of Furnace_StateAlt / ServerTime_Now() (seconds vs ms --
	//       read it off the probe's FLIP `delta` field). kBurnDurationTicks
	//       below is a PLACEHOLDER and is almost certainly wrong until then.
	//
	// Even once both hold, this only sets server-side state -- whether that
	// write propagates to clients (packUpdate / send-changes) is unverified
	// and out of scope for this hook.
	// ========================================================================
	constexpr bool kUseLightActivationPolicy = false;

	constexpr int kDuskHour = 20;  // inclusive start of "should be lit"
	constexpr int kDawnHour = 6;   // exclusive end (wraps past midnight)

	// UNVERIFIED -- placeholder only, see caveat (2) above.
	constexpr uint32_t kBurnDurationTicks_UNVERIFIED = 3600;

	bool IsDuskWindow(int hour)
	{
		if (hour < 0)
			return false; // clock not ready yet
		if (kDuskHour <= kDawnHour)
			return hour >= kDuskHour && hour < kDawnHour;
		return hour >= kDuskHour || hour < kDawnHour; // wraps midnight
	}

	// self pointer -> last observed "is burning" state.
	std::unordered_map<const void*, bool> g_fireSeen;

	void ProbeWorkingFire(void* self, uint32_t stateAlt, uint32_t now, bool burning)
	{
		if constexpr (!kProbeEnabled)
			return;

		const auto it = g_fireSeen.find(self);

		if (it == g_fireSeen.end())
		{
			// First sighting of this object. The pointer is the payload:
			// feed it to Lifx::dumpPtr in the console to inspect the
			// surrounding struct and find the owning object's type id.
			g_fireSeen.emplace(self, burning);
			Con::Echo("[lifx-light] NEW WorkingFire self=%p stateAlt=%u now=%u burning=%d",
			          self, stateAlt, now, burning ? 1 : 0);
			return;
		}

		if (it->second != burning)
		{
			it->second = burning;
			Con::Echo("[lifx-light] FLIP self=%p -> %s (stateAlt=%u now=%u, delta=%d)",
			          self,
			          burning ? "BURNING" : "EXPIRED",
			          stateAlt, now,
			          static_cast<int>(static_cast<int64_t>(stateAlt) - static_cast<int64_t>(now)));
		}
	}
}

// ============================================================================
// ENGINE REFERENCE — WorkingFire::recalcTick @ RVA 0x1DB650
// Verbatim Ghidra decompile. Trivial: returns whether the fire's state-alt
// timestamp is in the past (i.e., the fire's expiry time has elapsed).
// ----------------------------------------------------------------------------
// bool FUN_1401db650(longlong param_1)
// {
//   uint uVar1;
//   uVar1 = FUN_1405147a0();             // ::Engine::ServerTime_Now()
//   return *(uint *)(param_1 + 0x28) < uVar1;
// }
// ============================================================================

// ----------------------------------------------------------------------------
// LiFx C++ reimplementation of the engine's WorkingFire::recalcTick.
//
// Mirrors the engine line-for-line so behavior is byte-equivalent when this
// branch is taken. Edit freely to extend or replace; the passthrough below
// remains available via the kUseLifxReimplementation toggle.
// ----------------------------------------------------------------------------
namespace LifxImpl
{
	namespace WorkingFire
	{
		// ------------------------------------------------------------------------
		// ForceLight -- NOT WIRED IN, NOT CALLED ANYWHERE YET.
		//
		// Mechanical write side of the "light this fire" hypothesis from the
		// 2026-09-11 session (see stateAlt in the read below). Mirrors the
		// existing read exactly: same offset, same reinterpret_cast pattern, so
		// it needs no new engine wrappers and should compile as-is once dropped
		// next to a real Furnace_StateAlt definition.
		//
		// Deliberately does NOT decide *when* to light something -- that needs
		// the getGameTime() dusk check, which isn't referenced anywhere in this
		// file and I don't have engine_internals.h in front of me to know what
		// (if any) C++ wrapper exists for it. Paste that header/declaration in
		// if you want the actual dusk trigger wired here rather than on your box.
		//
		// Also unverified: whether burnDurationTicks is in the same units as
		// ServerTime_Now() (open question in the archive -- seconds vs ms). The
		// probe's FLIP log's `delta` field is what answers that; don't guess at
		// a duration constant until that number is in hand.
		// ------------------------------------------------------------------------
		void ForceLight(void* self, uint32_t burnDurationTicks)
		{
			auto* const stateAlt = reinterpret_cast<uint32_t*>(
				static_cast<char*>(self) + ::Engine::Off::Furnace_StateAlt);
			*stateAlt = ::Engine::ServerTime_Now() + burnDurationTicks;
		}

		unsigned long long RecalcTick(void* self, float /*dt*/, char /*finalize*/)
		{
			// Engine: return self->stateAlt < ServerTime::Now();
			auto state = *reinterpret_cast<uint32_t*>(
				static_cast<char*>(self) + ::Engine::Off::Furnace_StateAlt);
			const auto now = ::Engine::ServerTime_Now();

			// Observation only -- does not affect the value returned below.
			// NOTE the sense: the engine predicate is TRUE when the fire has
			// EXPIRED (stateAlt is in the past), so "burning" is its inverse.
			ProbeWorkingFire(self, state, now, /*burning=*/ !(state < now));

			// See the DUSK ACTIVATION POLICY block above -- stays fully inert
			// (dead-code-eliminated) until kUseLightActivationPolicy is flipped
			// on, which should not happen before the probe answers its two
			// open questions.
			if constexpr (kUseLightActivationPolicy)
			{
				const bool expired = state < now;
				if (expired && IsDuskWindow(::Engine::GameTime_CurrentHour()))
				{
					ForceLight(self, kBurnDurationTicks_UNVERIFIED);
					state = *reinterpret_cast<uint32_t*>(
						static_cast<char*>(self) + ::Engine::Off::Furnace_StateAlt);
					Con::Echo("[lifx-light] ACTIVATE self=%p new_stateAlt=%u now=%u",
					          self, state, now);
				}
			}

			// Original returns `bool`; the implicit conversion to our wider
			// uniform return type sets RAX's low byte correctly.
			return state < now;
		}
	}
}

// ----------------------------------------------------------------------------
// Dispatch. Default = passthrough (byte-equivalent to the unmodified engine).
// Flip kUseLifxReimplementation to `true` to route through LifxImpl above
// when you want to extend the behavior. `if constexpr` ensures the unused
// branch is dead-code-eliminated, so there's no runtime cost either way.
// ----------------------------------------------------------------------------
namespace { constexpr bool kUseLifxReimplementation = true; }

unsigned long long Hooks::WorkingFire::RecalcTick(void* self, float dt, char finalize)
{
	if constexpr (kUseLifxReimplementation)
		return LifxImpl::WorkingFire::RecalcTick(self, dt, finalize);
	return _WorkingFire_RecalcTick(self, dt, finalize);
}
