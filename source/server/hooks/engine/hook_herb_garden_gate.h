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
	Herb-garden time gate ("plant now, collect later") for a crafting-based garden.

	The vanilla Herbal Garden has a growth timer, but only for object type 1353 (the
	client hard-codes its window to that type). A custom garden that works through the
	ordinary Craft window is instantaneous. This hook adds a per-garden timer on top:

	  * Crafting one of the configured "grow" recipes at a garden with NO batch consumes
	    the ingredients normally, but the produced items are suppressed and a batch
	    (recipe, ready-time) is recorded for that garden.
	  * Crafting again while the batch is still growing fails with a message and consumes
	    nothing.
	  * Crafting again once the batch is ready swaps in the matching "collect" recipe
	    (grow recipe id + collectOffset), which only wears the garden and hands over the
	    herbs, then clears the batch.
	  * A separate client-visible "Collect Herbs" recipe (collectAnyRecipe), which needs no
	    ingredients, does the same for whatever is growing, and says "Empty" if nothing is.

	Hooks: CmCreationManager::craftWithDevice (RVA 0x1E4B10) decides plant / pending /
	collect from the craft context; CmCreationManager::_finallyMakeItem (RVA 0x1E3C60)
	skips item creation, the same way the original's success path ends, while a plant
	craft is in progress. Anything unexpected falls straight through to vanilla.

	Configured from lifxpluss.xml (the element's own attributes are the first gate; every <gate> child adds another,
	e.g. the Ore Washer: same plant-then-collect logic, opposite direction of yield, fixed output quality):
	    <herbGardenGate enabled="1" verbose="1" abilityId="328" growSeconds="32400"
	                    firstGrowRecipe="5200" lastGrowRecipe="5265" collectOffset="100"
	                    pendingMessageId="3632" emptyMessageId="5160" collectAnyRecipe="5299"
	                    timeMessageBase="5099" file="config/herb_garden_batches.txt">
	        <gate abilityId="64" growSeconds="7200" firstGrowRecipe="5610" lastGrowRecipe="5613"
	              collectOffset="100" collectAnyRecipe="5699" emptyMessageId="5176"
	              timeMessageBase="5160" timeMessageMax="15" outputQuality="30" />
	    </herbGardenGate>
	instant="1" (optional) turns a gate into a plain craft: no plant/collect timer, the grow recipes craft directly at the end of
	their progress bar and the curve/outputQuality is applied to the result; durationSeconds="N" makes that bar N seconds long
	(Ability::GetDuration override, applies to GM characters too). A batch already stored for an object is still collected first.
	workingState="1" (optional) also puts the building in the engine's State_Working while a batch runs (start/finish are the
	same CmCraftworkManager calls the Tanning Tub uses, so the timer row is persisted and restored at boot; pair it with a
	greenhouseAlias entry behavesLike="472" for that building and a <state type="Working"> in cm_objects.xml).
	curveEnd/curveGainStart/curveGainEnd (optional): logarithmic quality curve on the planted input quality.
	outputQuality (optional) fixes the quality of the items made by the collect craft (via _finallyMakeItem);
	timeMessageMax limits the time-left message family (ids base+1..base+max).
*/

#include "server/cm_server.h"

namespace tinyxml2 { class XMLElement; }

// CmCreationManager::craftWithDevice (Win64 __fastcall). RVA 0x1E4B10.
// ctx points at the ability's craft context (ability instance + 8).
__CM_DECL_EXTERNAL(U64, __fastcall, _Creation_CraftWithDevice,
                   LPVOID self, LPVOID* ctx, U8 isDevice, U64 extra);

// CmCreationManager::_finallyMakeItem (Win64 __fastcall). RVA 0x1E3C60.
__CM_DECL_EXTERNAL(U64, __fastcall, _Creation_FinallyMakeItem,
                   LPVOID self, LPVOID conn, LPVOID* recipeRef, LPVOID* itemRef,
                   LPVOID resultType, U32 quantity, U16 quality, U8 exceptional,
                   int abilityId, int arg10);

// Ability::GetDuration (Win64 __fastcall). RVA 0x31B3F0. Returns the perform (progress bar) length in ms; the craft
// recipe id is at ctx+0x40, the player id at ctx+0x18, the ability id at ability+8.
__CM_DECL_EXTERNAL(U64, __fastcall, _Ability_GetDuration, LPVOID ability, LPVOID ctx);

namespace Hooks
{
	namespace Engine
	{
		U64 __fastcall OnCraftWithDevice(LPVOID self, LPVOID* ctx, U8 isDevice, U64 extra);

		U64 __fastcall OnAbilityGetDuration(LPVOID ability, LPVOID ctx);

		U64 __fastcall OnFinallyMakeItem(
			LPVOID self, LPVOID conn, LPVOID* recipeRef, LPVOID* itemRef,
			LPVOID resultType, U32 quantity, U16 quality, U8 exceptional,
			int abilityId, int arg10);

		// True while a craft of a gate flagged noWorkshopBuff="1" is running on this thread (hook_workshop_buff skips its buff).
		bool HerbGateSuppressesWorkshopBuff();

		bool ConfigureHerbGardenGate(const tinyxml2::XMLElement* root);
		void AttachHerbGardenGateHooks();
	}
}
