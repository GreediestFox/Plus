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
	LiFx geo utility commands exposed to TorqueScript (Con::AddCommand, not Detours hooks).

	    Lifx::worldToGeoId(x, y)  -> int   the raw GeoID of the map cell at world position (x, y), or 0 when it can't be resolved
	    Lifx::setTerrainQuality(q) -> int  quality (0..100) of the layer TestRaiseTerrain raises; the engine hard-codes 100 (code byte at RVA 0x3AFE14)

	Uses the engine's own routines, found by decompilation of ddctd_cm_yo_server.exe: the world position is snapped to its cell
	(RVA 0x58B2F0, the same step CharacterParameters::SavePlayer uses) and converted by Geo::WorldToGeoId (RVA 0x57AAE0).
	The result feeds the engine's own script functions, e.g. CreateTestMovable(typeId, geoId, playerId, state),
	CreateTestUnmovable(typeId, geoId, playerId, state) and Player::TeleportTo(geoId).
*/

namespace Lifx
{
	namespace Api
	{
		namespace Geo
		{
			// Call once from Hooks::Engine::ConsoleInit after Torque's Con::Init has run.
			void Register();
		}
	}
}