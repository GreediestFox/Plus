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

#include "hook_light_working_object.h"
#include "server/hooks/furnace/engine_internals.h"

__CM_INSTATNTIATE(_LightWorkingObject_OnDoPerform);

namespace
{
	// Temporary instrumentation -- disable once the real entity-resolution path
	// (e.g. a proper object-id lookup command) replaces this probe.
	constexpr bool kProbeEnabled = true;
}

unsigned long long Hooks::LightWorkingObject::OnDoPerform(void* self)
{
	// Observation only -- does not affect the return value below.
	//
	// self is the AbilityImp::LightWorkingObject instance (ability id 268
	// "Light the Fire"). The real target ComplexObject_Entity* lives at
	// self+0x38 -- the original reads it there too, then RTTI-casts it before
	// use; we read the raw pointer directly, which is the same address for any
	// legitimately-typed target (RE'd 2026-09-11/12, see cm_offsets.h
	// LIGHTWORKINGOBJECT_ONDOPERFORM).
	//
	// This is exactly the pointer ::Engine::LightWorkingObject() expects (it
	// adds +0x384 internally) -- logged here so it can be fed into
	// Lifx::forceLightAt for live testing without needing a DB-id-to-live-
	// pointer resolver.
	if constexpr (kProbeEnabled)
	{
		void* entity = *reinterpret_cast<void**>(static_cast<char*>(self) + 0x38);
		Con::Echo("[lifx-light] LightWorkingObject::_onDoPerform self=%p entity=%p", self, entity);
	}

	return _LightWorkingObject_OnDoPerform(self);
}
