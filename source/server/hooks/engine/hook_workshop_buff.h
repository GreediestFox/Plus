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
	Workshop crafting-quality buff.

	The vanilla "crafting bonus building" buff (x1.2 quality) only exists in
	CmCreationManager::craftWithDevice, and only for six hard-coded building types,
	so a custom workshop can never get it and the Craft-window path (craftWithTool)
	has no buff at all. This hook adds one, driven by config.

	CmCreationManager::checkExceptionalChance (RVA 0x1E46B0) is called by BOTH craft
	paths just before the item is created, with the crafter's Player object and a
	writable quality. After the original runs, if the crafter is standing within a
	configured number of tiles of a placed workshop of a configured type (and the
	ability matches), the quality is multiplied and re-capped at 100.

	"Near" is decided WITHOUT any scene/container query (those crashed the server in
	earlier experiments): a background thread polls the database for placed workshops
	(unmovable_objects.GeoDataID) and the hook compares that with the Player's current
	geoId (Player+0x2464). GeoID layout: terrainBlock = id / 262144,
	row = (id % 262144) / 512, col = id % 512; one tile is one build-grid cell.
	Workshops in a different terrain block than the player are treated as out of range.

	Configured from lifxpluss.xml:
	    <workshopBuff enabled="1" refreshSeconds="30" verbose="1">
	        <db host="127.0.0.1" port="3306" user="root" password="..." name="lif_1" />
	        <workshop objectTypeId="2894" radiusTiles="6" multiplier="1.2" abilityIds="201" />
	    </workshopBuff>
*/

#include "server/cm_server.h"

namespace tinyxml2 { class XMLElement; }

// CmCreationManager::checkExceptionalChance (Win64 __fastcall). RVA 0x1E46B0.
// Returns false on failure; on success may raise *quality and set *exceptionalFlag.
__CM_DECL_EXTERNAL(U8, __fastcall, _Creation_CheckExceptionalChance,
                   LPVOID self, U32 charId, LPVOID player, U32 skillAmount,
                   int abilityId, float chance, LPVOID resultType,
                   U16* quality, U8* exceptionalFlag);

namespace Hooks
{
	namespace Engine
	{
		U8 __fastcall OnCheckExceptionalChance(
			LPVOID self, U32 charId, LPVOID player, U32 skillAmount,
			int abilityId, float chance, LPVOID resultType,
			U16* quality, U8* exceptionalFlag);

		bool ConfigureWorkshopBuff(const tinyxml2::XMLElement* root);
		void AttachWorkshopBuffHook();
	}
}
