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
	Hook on AbilityImp::ServerManager::_registerPerform (RVA 0x3B5D80) -- the
	single generic entry point the engine calls for EVERY ability perform,
	regardless of which concrete ability class is involved. RE'd 2026-09-12
	via the diagnostic string "AbilityImp::ServerManager::_registerPerform"
	(present in the binary purely for its own log lines, not a mangled/
	exported symbol).

	Decompile shows the second argument is a request wrapper:
	    ability     = *(void**)request              // concrete AbilityImp* instance
	    abilityDesc = *(void**)(ability + 0x28)      // shared per-class descriptor
	    abilityId   = *(int*)(abilityDesc + 8)
	    abilityName = *(const char**)(abilityDesc + 0x10)

	This exists to settle the still-open question of which RVA is really
	behind ability id 268 "Light the Fire". LIGHTWORKINGOBJECT_ONDOPERFORM
	(0x3A21F0, see hook_light_working_object.cpp) was confirmed via live
	testing NOT to fire for it. Logging `*(void**)ability` here -- the
	object's own vtable pointer -- when abilityId==268 gives the real
	answer directly and empirically, instead of more blind RVA/table
	guessing (the ability-id-indexed table approach broke down between
	verified index 112 and target index 268).

	Observation only -- never touches the return value or any engine state.

	Unified two-argument signature for this family of dispatch functions:
	    unsigned long long _registerPerform(ServerManager* self, Request* request);
*/

#include "server/cm_server.h"

__CM_DECL_EXTERNAL(unsigned long long, __fastcall, _ServerManager_RegisterPerform, void* self, void* request);

namespace Hooks
{
	namespace ServerManager
	{
		unsigned long long RegisterPerform(void* self, void* request);
	}
}
