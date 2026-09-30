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

#include "hook_proc_desc.h"
#include "engine_internals.h"

__CM_INSTATNTIATE(_Furnace_LookupProcDesc);

// ============================================================================
// ENGINE REFERENCE — WorkingFurnace's process-descriptor lookup @ RVA 0x1DB7C0
// Walks DAT_140ACFA60 (the 59-row furnace descriptor table). For each row,
// tests hasParent(item, row.typeId, 100); returns the first matching row.
// ----------------------------------------------------------------------------
// undefined4 * FUN_1401db7c0(undefined8 param_1)
// {
//   char cVar1;
//   longlong lVar2;
//   int iVar3;
//   longlong lVar4;
//
//   iVar3 = 0;
//   if (DAT_140acfa64 != 0) {                            // table not empty
//     lVar4 = 0;
//     lVar2 = 0;
//     do {
//       cVar1 = FUN_14027eb30(param_1,
//                             *(undefined4 *)((longlong)&DAT_140acfa60 + lVar2),
//                             100);                     // hasParent(item, row.typeId, 100)
//       if (cVar1 != '\0') {
//         return &DAT_140acfa60 + (longlong)iVar3 * 7;  // matching row
//       }
//       lVar4 = lVar4 + 1;
//       iVar3 = iVar3 + 1;
//       lVar2 = lVar4 * 0x1c;                            // step by 28 bytes
//     } while ((&DAT_140acfa64)[lVar4 * 7] != 0);        // terminator = next-row's typeId == 0
//   }
//   return (undefined4 *)0x0;
// }
// ============================================================================

// ---------------------------------------------------------------------------- //
// LiFx furnace process-descriptor extension table.
//
// Each row has the same on-the-wire layout as a row in the engine's hardcoded
// table at DAT_140ACFA60 (28 bytes, 7 x uint32). The hook walks the rows
// below first; first hasParent match wins. Unmatched lookups fall through to
// the engine (or to LifxImpl's reimplementation, when enabled).
//
// Row layout (matches the engine — see docs/bloomery.md for the full table):
//   +0x00  typeId          hasParent anchor — which type-hierarchy this rule applies to
//   +0x04  kind            recalcTick branch selector (see below)
//   +0x08  factor          IEEE-754 float; the engine reads this only for some
//                          paths. 0x3F800000 == 1.0f is the universal safe value.
//   +0x0C  outputTypeId    transformation target type. Read by kind=6 branch.
//   +0x10  flag (byte)     if non-zero, force the engine into Path A (recipe-driven)
//                          regardless of `kind`. We keep this 0 so our kind value
//                          is honored.
//   +0x14  field5          consumed by WorkingFurnace::slot 11 when kind==1
//   +0x18  tempThreshold   minimum furnace temperature/state to advance.
//
// Useful kind values (see WorkingFurnace::recalcTick walkthrough in
// docs/bloomery.md for the full semantics):
//   kind=2,3            instant: set the slot's quality to 100 in one tick
//   kind=4              single-stage bake: tick progress at 1x rate while
//                       furnace.state >= tempThreshold
//   kind=5              **vostaskus heat/cool/heat cycle** — 5 hot/cool/hot/
//                       cool/hot state transitions, each advancing quality by 20
//   kind=6              kiln-style table-driven bake; outputTypeId at +0xC is the
//                       finished type
//
// Worked example: adding a vostaskus-style "damascus copper billet" recipe:
//
//     { 9001, 5, 0x3F800000, 9002, 0, 0, 2000 }
//        ^   ^      ^         ^   ^  ^   ^
//        |   |      |         |   |  |   '--- requires furnace state==2000 (hot)
//        |   |      |         |   |  '------- field5 unused for kind=5
//        |   |      |         |   '---------- flag=0: respect `kind`, don't force
//        |   |      |         |               into recipe-driven Path A
//        |   |      |         '-------------- finished item is type 9002
//        |   |      '------------------------ factor 1.0f
//        |   '------------------------------- kind 5: vostaskus heat/cool/heat
//        '----------------------------------- match anything descending from type 9001

struct LifxFurnaceRow
{
	U32 typeId;
	U32 kind;
	U32 factor;        // float bits, usually 0x3F800000 (= 1.0f)
	U32 outputTypeId;
	U32 flag;          // engine reads the low byte; rest is padding
	U32 field5;
	U32 tempThreshold;
};
static_assert(sizeof(LifxFurnaceRow) == 28, "row size must match engine layout");

static const LifxFurnaceRow kExtensionRows[] =
{
	// ===== example #1: new vostaskus-style item =====
	// Uncomment and edit type IDs to match your DB rows.
	// { /* typeId */ 9001, /* kind */ 5, 0x3F800000, /* output */ 9002, 0, 0, 2000 },

	// ===== example #2: kiln-style new clay brick =====
	// { /* typeId */ 9100, /* kind */ 6, 0x3F800000, /* output */ 9101, 0, 0, 1000 },

	// Trailing sentinel -- keeps the array non-empty so it compiles under
	// /permissive- on newer MSVC (a zero-length array is a non-standard
	// extension; older toolsets accepted it, this one rejects it as C2466).
	// Never read: kExtensionRowCount below excludes it, and the lookup loop
	// is bounds-limited to kExtensionRowCount, not the raw array. Add real
	// rows above this line.
	LifxFurnaceRow{},
};

static constexpr size_t kExtensionRowCount =
	sizeof(kExtensionRows) / sizeof(kExtensionRows[0]) - 1;

// ----------------------------------------------------------------------------
// LiFx C++ reimplementation of the engine's lookup (alternative to trampoline).
// Walks DAT_140ACFA60 ourselves using the same algorithm. Identical observable
// behavior to the engine; useful when you want to insert logic *between* the
// hasParent check and the row return (e.g. logging, conditional skipping).
// ----------------------------------------------------------------------------
namespace LifxImpl
{
	namespace Furnace
	{
		void* ProcDescLookup(void* itemTypeInfo)
		{
			auto* rows = ::Engine::DataAtRva<LifxFurnaceRow>(0xACFA60);
			for (unsigned i = 0; rows[i].typeId != 0; ++i)
			{
				if (::Engine::Type_HasParent(itemTypeInfo, static_cast<int>(rows[i].typeId), 100))
					return &rows[i];
			}
			return nullptr;
		}
	}
}

// ----------------------------------------------------------------------------
// Dispatch.
//
// Order: (1) LiFx extension rows ALWAYS run first — that's the customization
// surface, independent of the toggle. (2) For unmatched lookups, either
// LifxImpl's reimplementation or the engine's trampoline runs, controlled by
// kUseLifxReimplementation.
// ----------------------------------------------------------------------------
namespace { constexpr bool kUseLifxReimplementation = false; }

void* Hooks::Furnace::ProcDescLookup(void* itemTypeInfo)
{
	if (itemTypeInfo != nullptr && kExtensionRowCount > 0)
	{
		for (size_t i = 0; i < kExtensionRowCount; ++i)
		{
			const auto& row = kExtensionRows[i];
			if (::Engine::Type_HasParent(itemTypeInfo, static_cast<int>(row.typeId), 100))
				return const_cast<LifxFurnaceRow*>(&row);
		}
	}

	if constexpr (kUseLifxReimplementation)
		return LifxImpl::Furnace::ProcDescLookup(itemTypeInfo);
	return _Furnace_LookupProcDesc(itemTypeInfo);
}
