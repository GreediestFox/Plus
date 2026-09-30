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
	Greenhouse alias: lets a custom object type behave as the vanilla Herbal Garden.

	The server's "greenhouse" (WorkingGreenhouse) logic is keyed to object type 1353
	(0x549) in exactly two places, both found by decompilation and a byte scan of .text:

	  1. CmCraftworkManager::loadObjects (RVA 0x1D8E20) - restores working objects from
	     the `working_containers` table at boot; any other type is rejected with
	     "Bad object type". Fixed by remapping its 4th argument.
	  2. The "use" entry point (RVA 0x1D91D0) - reads the object's type through
	     GetTypeInfo (RVA 0xC9930) then Type::getId (RVA 0xD3790, a 4-byte getter that
	     cannot be detoured) and compares it to 0x549; other types fall into a generic
	     furnace-like branch. Fixed by returning a stand-in type record whose id is 1353,
	     but ONLY for the single call at RVA 0x1D9220 (the return address is checked),
	     so every other user of the real record is untouched.

	Configured from lifxpluss.xml:
	    <greenhouseAlias enabled="1" verbose="1">
	        <alias objectTypeId="2835" behavesLike="1353" />
	    </greenhouseAlias>

	The same loadObjects remap also lets a custom Drying Frame / Tanning Tub keep its working
	state across a server restart (behavesLike="118" / "472"): the engine only restores
	working containers for the vanilla types and drops any other with "Bad object type",
	so the model and timer of a Big Drying Frame (2829) / Big Tanning Tub (2017) were lost
	at every boot. Those aliases only use the loadObjects remap (the use-entry hook is
	attached only when a 1353 alias exists). Optional finishedAsWorking="1" makes a container
	whose timer ran out while the server was down come back Working with 1 s left, instead of
	Complete with its finished items stuck in the slot:
	        <alias objectTypeId="2829" behavesLike="118" finishedAsWorking="1" />
	        <alias objectTypeId="2017" behavesLike="472" finishedAsWorking="1" />
*/

#include "server/cm_server.h"

namespace tinyxml2 { class XMLElement; }

// CmCraftworkManager::loadObjects (Win64 __fastcall). RVA 0x1D8E20.
// The 4th parameter is the placed object's type id.
__CM_DECL_EXTERNAL(void, __fastcall, _Craftwork_LoadObjects,
                   LPVOID self, U64 objectId, int timeLeftSec, int objectTypeId,
                   U32 arg5, U32 arg6);

// AbilityBaseWC::_onDoPerform: shared by the Tanning Tub / Drying Frame abilities (Use Tanning Tub, Dry a Hide).
// RVA 0x3A0F60. The ability instance keeps the target entity at +0x38.
__CM_DECL_EXTERNAL(U64, __fastcall, _Wc_DoPerform, LPVOID ability);

// Working-container recipe finder: first recipe of the ability's skill whose plain ingredient matches the target item.
// RVA 0x30BEE0. out is a shared_ptr<Recipe> (recipe ptr, control block); the recipe id is its first dword.
__CM_DECL_EXTERNAL(LPVOID*, __fastcall, _Wc_FindRecipe, LPVOID mgr, LPVOID* out, int skillId, LPVOID itemType);

// Object type "is descendant of" helper (type record, ancestor id, max depth) -> 1/0. RVA 0x27EB30 (97 callers).
// The inventory-open check calls it with ancestor 0x575 (Ruins) at RVA 0x386422 (return address); a hook there lets extra
// storage buildings pass as "warehouse".
__CM_DECL_EXTERNAL(U64, __fastcall, _Type_IsDescendant, LPVOID typeRec, int ancestorId, int depth);

// Entity -> type record accessor (mov rax,[rcx+370h]; ret). RVA 0xC9930.
__CM_DECL_EXTERNAL(LPVOID, __fastcall, _Entity_GetTypeInfo, LPVOID entity);

namespace Hooks
{
	namespace Engine
	{
		void __fastcall OnCraftworkLoadObjects(
			LPVOID self, U64 objectId, int timeLeftSec, int objectTypeId,
			U32 arg5, U32 arg6);

		LPVOID __fastcall OnEntityGetTypeInfo(LPVOID entity);

		U64 __fastcall OnTypeIsDescendant(LPVOID typeRec, int ancestorId, int depth);

		U64 __fastcall OnWcDoPerform(LPVOID ability);
		LPVOID* __fastcall OnWcFindRecipe(LPVOID mgr, LPVOID* out, int skillId, LPVOID itemType);

		bool ConfigureGreenhouseAlias(const tinyxml2::XMLElement* root);
		void AttachGreenhouseAliasHook();
	}
}
