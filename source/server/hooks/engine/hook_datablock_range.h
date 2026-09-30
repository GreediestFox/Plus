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
*  =================================================================================== *//*
	Datablock id range: the engine numbers datablocks 3..0x402 (1024 ids) and sends them over the network with a ranged integer of that width.
	The mounted-image datablocks of the complex (movable) object types get consecutive ids from a counter that starts at 800 and has no upper bound
	check, so with ~220 types it runs into the fixed ids of the script datablocks (1016+). This hook widens the range in memory (never on disk):

	  - counter start of Complex::ObjectType::TypeStorage (constructor, imm32 at RVA 0xCEC86): 800 -> 1027
	  - datablock id write in the ghost pack (`mov r9d,402h`, imm32 at RVA 0x13372B) and read (`mov r8d,402h`, imm32 at RVA 0x133D24): 0x402 -> 0x1002
	  - first ordinary object id (Sim init, imm32 at RVA 0x4297F7 and the live counter at RVA 0xBC8FD4): 0x443 -> 0x1043

	The client executable carries the same change (patched on disk), so both sides must be switched together.

	    <datablockRange enabled="1" verbose="1" />
*/

#include "server/cm_server.h"

namespace tinyxml2 { class XMLElement; }

namespace Hooks
{
	namespace Engine
	{
		bool ConfigureDatablockRange(const tinyxml2::XMLElement* root);
		void AttachDatablockRangeHook();
	}
}