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
	Hook on AbilityImp::LightWorkingObject::_onDoPerform (vtable slot 3 of its own
	object vtable, RVA 0x3A21F0) -- the real implementation behind ability id 268
	"Light the Fire". Observation only: logs the resolved target entity pointer
	(self+0x38) so it can be fed into Lifx::forceLightAt for testing, then calls
	through to the original unmodified.

	Unified slot-3 signature for this AbilityBaseImp-derived family:
	    longlong _onDoPerform(Self* self);
*/

#include "server/cm_server.h"

__CM_DECL_EXTERNAL(unsigned long long, __fastcall, _LightWorkingObject_OnDoPerform, void* self);

namespace Hooks
{
	namespace LightWorkingObject
	{
		unsigned long long OnDoPerform(void* self);
	}
}
