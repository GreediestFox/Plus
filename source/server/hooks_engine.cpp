
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

#include "hooks_engine.h"
#include "server/cm_server.h"
#include "server/api/lifx_character.h"
#include "server/api/lifx_outpost.h"
#include "server/api/lifx_effects.h"
#include "server/api/lifx_timers.h"
#include "server/api/lifx_dispatcher.h"
#include "server/api/lifx_hostile.h"
#include "server/api/lifx_battlezone.h"
#include "server/api/lifx_geo.h"
#include "server/hooks/character/hook_setanimation.h"
#include "server/hooks/character/hook_animal_death.h"
#include "server/hooks/character/hook_animal_create.h"
#include "server/hooks/character/hook_container_init.h"

__CM_DECL_INTERNAL(void, __fastcall, _Engine_Con_Init, void);

__CM_INSTATNTIATE(_Engine_Con_LookupNamespace);
__CM_INSTATNTIATE(_Engine_StringTableInsert);

namespace Hooks
{
	namespace Engine
	{
		// ---------------------------------------------------------------------------- //
		const char* StringTableInsert(void* _this, const char* val, const bool caseSens)
		{
			// here we only got ptr to gStringTable
			// todo: find gStringTable better that shit!
			if (gSpace.Pointers().Get("StringTable") == nullptr)
			{
				gSpace.Pointers().Set("StringTable", _this);
			}

			const char* ret = _Engine_StringTableInsert(gSpace.Pointers().Get("StringTable"), val, caseSens);

			return ret;
		}

		// ---------------------------------------------------------------------------- //
		bool InitConsoleAddCommandHooks()
		{
			if (gSpace.Pointers().Get("StringTable") == nullptr)
			{
				// we must get _StringTable ptr before register any commands!
				return false;
			}

			// Implement INT command
			__CM_FIND(CmOffset::CON_ADD_INT_COMMAND, _Engine_AddCommand_Int);
			if (_Engine_AddCommand_Int == nullptr) return false;

			// Implement FLOAT command
			__CM_FIND(CmOffset::CON_ADD_FLOAT_COMMAND, _Engine_AddCommand_Float);
			if (_Engine_AddCommand_Float == nullptr) return false;

			// Implement STRING command
			__CM_FIND(CmOffset::CON_ADD_STRING_COMMAND, _Engine_AddCommand_String);
			if (_Engine_AddCommand_String == nullptr) return false;

			// Implement VOID command
			__CM_FIND(CmOffset::CON_ADD_VOID_COMMAND, _Engine_AddCommand_Void);
			if (_Engine_AddCommand_Void == nullptr) return false;

			// Implement bool command
			__CM_FIND(CmOffset::CON_ADD_BOOL_COMMAND, _Engine_AddCommand_Bool);
			if (_Engine_AddCommand_Bool == nullptr) return false;

			return true;
		}

		// ---------------------------------------------------------------------------- //
		void ConsoleInit()
		{
			__CM_FIND(CmOffset::CON_LOOKUP_NAMESPACE, _Engine_Con_LookupNamespace);

			if (_Engine_Con_LookupNamespace == nullptr)
			{
				Lifx::ShowErrorMessage("Can't hook Con::LookupNamespace()!");
			}

			// global pointers
			MODULEINFO mi;
			GetModuleInformation(GetCurrentProcess(), GetModuleHandle(NULL), &mi, sizeof(MODULEINFO));

			auto global_vars_ptr = reinterpret_cast<LPVOID>((char*)mi.lpBaseOfDll + 0x0BE5AC8); // don't get value. Just pointer itself!
			gSpace.Pointers().Set("GlobalVars", global_vars_ptr);

			if (gSpace.Pointers().Get("GlobalVars") == nullptr)
			{
				Lifx::ShowErrorMessage("Can't find global variables dictionary pointer!");
			}

			// call torque's con::Init() before we can spawn ourself api
			_Engine_Con_Init();

			// implement con::addVariable()
			__CM_FIND(CmOffset::CON_ADD_VARIABLE, _Engine_Con_AddVariable);

			if (_Engine_Con_AddVariable == nullptr)
			{
				Lifx::ShowErrorMessage("Can't hook Con::addVariable()!");
			}

			// implement con::addConstant()
			__CM_FIND(CmOffset::CON_ADD_CONSTANT, _Engine_Con_AddConstant);

			if (_Engine_Con_AddConstant == nullptr)
			{
				Lifx::ShowErrorMessage("Can't hook Con::addConstant()!");
			}

			// implement con::addCommand() functions
			if (!InitConsoleAddCommandHooks())
			{
				Lifx::ShowErrorMessage("Can't hook Con::addCommand()!");
			}

			// implement con::evaluate()
			__CM_FIND(CmOffset::CON_EVALUATE, _Engine_Con_Evaluate);

			if (_Engine_Con_Evaluate == nullptr)
			{
				Lifx::ShowErrorMessage("Can't hook Con::Evaluate()!");
			}

			// implement con::setVariable()
			__CM_FIND(CmOffset::CON_SET_VARIABLE, _Engine_Con_SetVariable);

			if (_Engine_Con_SetVariable == nullptr)
			{
				Lifx::ShowErrorMessage("Can't hook Con::setVariable()!");
			}

			// implement con::getVariable()
			__CM_FIND(CmOffset::CON_GET_VARIABLE, _Engine_Con_GetVariable);

			if (_Engine_Con_GetVariable == nullptr)
			{
				Lifx::ShowErrorMessage("Can't hook Con::getVariable()!");
			}

			// Start server
			gServer.Start();

			// register basic LiFx constants
			Con::AddConstant("$lifx::Version", Con::ConsoleType::String, &kCoreVersionString);
			Con::AddConstant("$cm_globals::IsYOServer", Con::ConsoleType::Boolean, &kIsYourOwnDefaultServer);
			Con::AddConstant("$cm_globals::YOVersion", Con::ConsoleType::Integer, &kYOVersion);
			Con::AddConstant("$cm_globals::YOVersionString", Con::ConsoleType::String, &kYOVersionString);

			// register LiFx Con::AddCommand entries (TorqueScript-callable)
			Lifx::Api::Character::Register();
			Lifx::Api::Outpost::Register();
			Lifx::Api::Effects::Register();
			Lifx::Api::Timers::Register();
			Lifx::Api::Dispatcher::Register();
			Lifx::Api::Hostile::Register();
			Lifx::Api::BattleZone::Register();
			Lifx::Api::Geo::Register();

			Con::Echo(" ==== Powered by %s", kCoreVersionString);
		}
	}

	// ---------------------------------------------------------------------------- //
	void AttachHooks()
	{
		__CM_ATTACH_HOOK(CmOffset::CON_INTERNAL_PRINTF, _Engine_Con_InternalConsolePrintf, Hooks::Engine::OnInternalPrintf);
		__CM_ATTACH_HOOK(CmOffset::CON_INIT, _Engine_Con_Init, Hooks::Engine::ConsoleInit);
		__CM_ATTACH_HOOK(CmOffset::STRING_TABLE_INSERT, _Engine_StringTableInsert, Hooks::Engine::StringTableInsert);

		// Universal LFXE decrypt at the FileStream layer (issue #116) -- replaces
		// the openFileStream-factory hook; covers every loose asset incl. *.cs.dso.
		__CM_ATTACH_HOOK(CmOffset::FILESTREAM_OPEN, _Engine_FS_Open, Hooks::Engine::FileStream_OnOpen);
		__CM_ATTACH_HOOK(CmOffset::FILESTREAM_READ, _Engine_FS_Read, Hooks::Engine::FileStream_OnRead);
		__CM_ATTACH_HOOK(CmOffset::FILESTREAM_GETSIZE, _Engine_FS_GetStreamSize, Hooks::Engine::FileStream_OnGetStreamSize);
		__CM_ATTACH_HOOK(CmOffset::FILESTREAM_GETPOS, _Engine_FS_GetPosition, Hooks::Engine::FileStream_OnGetPosition);
		__CM_ATTACH_HOOK(CmOffset::FILESTREAM_SETPOS, _Engine_FS_SetPosition, Hooks::Engine::FileStream_OnSetPosition);
		__CM_ATTACH_HOOK(CmOffset::FILESTREAM_DTOR, _Engine_FS_Dtor, Hooks::Engine::FileStream_OnDtor);

		// #145: gate AnimatedNPC::setAnimation for our male.dts hostiles (suppresses
		// the wolf-tree's missing-sequence flood; wolf->male remap layers on later).
		__CM_ATTACH_HOOK(CmOffset::ANIMATED_NPC_SET_ANIMATION, _AnimatedNpc_SetAnimation, Hooks::AnimRemap::OnSetAnimation);

		// #145: redirect our animals' death from carcass -> Player tombstone (worn loot).
		__CM_ATTACH_HOOK(CmOffset::ANIMAL_CREATE_CORPSE, _Animal_CreateCorpse, Hooks::AnimalDeath::OnCreateCorpse);

		// #145: auto-tag every Bandit (type 755) animal at creation so our hooks apply
		// to engine-spawned (killable, navmesh-integrated) animals, not just ours.
		__CM_ATTACH_HOOK(CmOffset::CREATE_ANIMAL, _Animal_CreateAnimal, Hooks::AnimalCreate::OnCreateAnimal);

		// #145 Step 2: capture grave (type 1070) containers as they load, so the
		// post-death SQL loot-move can repopulate the in-memory grave container live.
		__CM_ATTACH_HOOK(CmOffset::CONTAINER_TRYINIT, _Container_TryInit, Hooks::ContainerInit::OnTryInit);
	}

	// ---------------------------------------------------------------------------- //
	void DetachHooks()
	{
		__CM_DETACH_HOOK(_Container_TryInit, Hooks::ContainerInit::OnTryInit);
		__CM_DETACH_HOOK(_Animal_CreateAnimal, Hooks::AnimalCreate::OnCreateAnimal);
		__CM_DETACH_HOOK(_Animal_CreateCorpse, Hooks::AnimalDeath::OnCreateCorpse);
		__CM_DETACH_HOOK(_AnimatedNpc_SetAnimation, Hooks::AnimRemap::OnSetAnimation);
		__CM_DETACH_HOOK(_Engine_FS_Dtor, Hooks::Engine::FileStream_OnDtor);
		__CM_DETACH_HOOK(_Engine_FS_SetPosition, Hooks::Engine::FileStream_OnSetPosition);
		__CM_DETACH_HOOK(_Engine_FS_GetPosition, Hooks::Engine::FileStream_OnGetPosition);
		__CM_DETACH_HOOK(_Engine_FS_GetStreamSize, Hooks::Engine::FileStream_OnGetStreamSize);
		__CM_DETACH_HOOK(_Engine_FS_Read, Hooks::Engine::FileStream_OnRead);
		__CM_DETACH_HOOK(_Engine_FS_Open, Hooks::Engine::FileStream_OnOpen);
		__CM_DETACH_HOOK(_Engine_StringTableInsert, Hooks::Engine::StringTableInsert);
		__CM_DETACH_HOOK(_Engine_Con_Init, Hooks::Engine::ConsoleInit);
		__CM_DETACH_HOOK(_Engine_Con_InternalConsolePrintf, Hooks::Engine::OnInternalPrintf);
	}
}
