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
	Entity -> Object lookup used by the hooks that need the object type of the entity an ability targets
	(ability+0x38).

	ENGINE_OBJECT_FROM_ENTITY (RVA 0x33DDB0, the generic entity -> object lookup) must only ever be given a
	ComplexObject_Entity*. The engine's own callers (e.g. AbilityImp::LightOn::_onDoPerform) run the raw
	ability+0x38 pointer through __RTDynamicCast first; feeding the raw pointer in directly fails with
	"Complex::ServerObjectStorage::_getObjectMapUnsafe() - invalid super-type" for any other entity class
	(natural water is a Waterpos_Entity, harnessed carts are AttachedObject_Entity, ...).

	CastToComplexEntity() reproduces that cast. It returns nullptr for every other entity class, in which case the
	caller must not call the lookup.
*/

#include "server/cm_offsets.h"

#include <Windows.h>

#include <cstdint>

namespace Hooks
{
	namespace Engine
	{
		// Same shape as the compiler's own call sites:
		//     void* __RTDynamicCast(void* ptr, long vfDelta, void* srcTypeDesc, void* targetTypeDesc, int isReference)
		inline LPVOID CastToComplexEntity(std::uintptr_t base, LPVOID entity)
		{
			using RtDynamicCastFn = LPVOID(__fastcall*)(LPVOID, long, LPVOID, LPVOID, int);
			__try
			{
				return reinterpret_cast<RtDynamicCastFn>(base + CmOffset::ENGINE_RT_DYNAMIC_CAST)(
					entity, 0,
					reinterpret_cast<LPVOID>(base + CmOffset::RTTI_SIM_OBJECT_TYPE_DESCRIPTOR),
					reinterpret_cast<LPVOID>(base + CmOffset::RTTI_COMPLEX_OBJECT_ENTITY_TYPE_DESCRIPTOR),
					0);
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return nullptr;
			}
		}
	}
}
