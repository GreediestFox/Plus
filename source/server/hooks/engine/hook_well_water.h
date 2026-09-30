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
	Well water: a well gives more than one Water per "Get Water" action.

	Found by decompilation of ddctd_cm_yo_server.exe:

	  AbilityImp::GetWater::_onDoPerform (RVA 0x39F780) resolves the targeted entity (ability+0x38; a well is a ComplexObject_Entity,
	  natural water is a Waterpos_Entity) and then calls Gathering::Manager::gather (RVA 0x37B090) with a one-element item range
	  {0xCC = 204, Water} and the literal quantity 1 as the 7th argument (chance 100 is the 5th).

	Both functions are wrapped. The GetWater wrapper reads the object type of the targeted well (same lookup as the greenhouse
	alias) and, when a rule matches, records the wanted amount in thread-local storage for the duration of the call. The gather
	wrapper replaces the quantity by that amount when the item is Water. Natural water (rivers, lakes) and every other gathering
	ability are left alone.

	Configured from lifxpluss.xml:
	    <wellWater enabled="1" verbose="1">
	        <well objectTypeId="95" amount="20" />      <!-- Well (stone) -->
	        <well objectTypeId="1712" amount="20" />    <!-- Wooden Well -->
	    </wellWater>
	(amount 1..1000; the Water item stacks to 1000)
*/

#include "server/cm_server.h"

namespace tinyxml2 { class XMLElement; }

// AbilityImp::GetWater::_onDoPerform(ability) -> result code. RVA 0x39F780.
__CM_DECL_EXTERNAL(U32, __fastcall, _Well_GetWater, LPVOID ability);
// Gathering::Manager::gather(mgr, player, connId, pos*, chancePct, itemRange*, quantity, flag, func*) -> success. RVA 0x37B090.
__CM_DECL_EXTERNAL(U64, __fastcall, _Well_Gather, LPVOID mgr, LPVOID player, U32 connId, LPVOID pos, U32 chance, LPVOID itemRange,
                   U64 quantity, char flag, LPVOID func);

namespace Hooks
{
	namespace Engine
	{
		U32 __fastcall OnWellGetWater(LPVOID ability);
		U64 __fastcall OnWellGather(LPVOID mgr, LPVOID player, U32 connId, LPVOID pos, U32 chance, LPVOID itemRange,
		                            U64 quantity, char flag, LPVOID func);

		bool ConfigureWellWater(const tinyxml2::XMLElement* root);
		void AttachWellWaterHook();
	}
}