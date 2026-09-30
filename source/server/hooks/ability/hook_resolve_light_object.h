#pragma once

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

/*
	Observation probe on the light-component resolver (RVA 0x33DDB0, called
	"LightOn_ResolveLightObject" in engine_internals.h). LightOn::_onDoPerform
	calls this with an ALREADY RTTI-cast (__RTDynamicCast to
	ComplexObject_Entity) pointer, not the raw self+0x38 value our earlier
	registerPerform probe logged -- feeding that raw, uncast pointer to this
	function directly (Lifx::forceLightOn) produced
	"Complex::ServerObjectStorage::_getObjectMapUnsafe() - invalid super-type"
	live-tested 2026-09-12. This probe logs the REAL, correctly-cast param_1
	this function receives when the engine calls it itself, so a live test can
	copy the right value instead of guessing at replicating the RTTI cast.

	Observation only -- logs then calls through unmodified.
*/

#include "server/cm_server.h"

__CM_DECL_EXTERNAL(void*, __fastcall, _LightOn_ResolveLightObject_Orig, void* entity, void* outBuf2Qwords);

namespace Hooks
{
	namespace LightOnResolve
	{
		void* Call(void* entity, void* outBuf2Qwords);
	}
}
