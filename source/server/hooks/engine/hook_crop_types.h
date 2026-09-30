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
	New plantable crops (e.g. three extra grain types), reusing the engine's existing farming
	machinery end to end rather than cloning any compiled game logic. Found by decompilation of ddctd_cm_yo_server.exe:

	  Sowing/harvesting a crop (Wheat=ability id 133, Peas=134, ... Potatoes=140) is handled by ONE shared native
	  function, AbilityImp::BasePlantImp::_onDoPerform, for all 8 crops. It looks up the ability's own numeric id in a
	  runtime std::unordered_map<u32, Record> (global at RVA 0xB98D10) to learn: which item to consume as seed
	  (Record+0x00, u32), and which terrain substance (Record+0x04 / Record+0x05, u8) to paint the tile with for the
	  two "freshly tilled" soil variants. AbilityManager::_registerAbility (RVA 0x327D20) stamps the ability's own id
	  onto the ability object it registers (object+8) - the object's COMPILED CODE never hardcodes its own id, so a
	  brand new ability id can reuse an EXISTING crop's compiled vtable (we reuse PlantWheat_Ability's, RVA 0x7F7A10)
	  and behave as a fully independent crop purely by which id it is registered under and which row it has in the
	  map above. AbilityManager::AbilityManager (which builds the registry, RVA 0x31D040 is the "register all built-in
	  abilities" step called from its constructor) is wrapped: the real registration runs first, unmodified, then this
	  hook registers the configured extra crops the same way (allocate, zero, base-init via RVA 0x317020, install the
	  reused vtable, register via 0x327D20) and inserts their map rows via the SAME lookup/insert function the engine
	  itself uses (RVA 0x3A95C0).

	  Growth over time (CropsMaintenance, cropsmaintenance.cpp) is pure arithmetic on the substance id: a valid crop
	  substance is one within [101, 101+W) (checked as `(u8)(id-101) <= W-1`, 8 near-identical inlined copies across
	  that file, W=75 vanilla = exactly the 8 existing crops' 10-wide id blocks with no room left), and within that
	  range advancement is `id%10`-driven (1=FertileSmall,2=NonFertileSmall,3=FertileNormal,4=NonFertileNormal,
	  5=FertileBig,6=NonFertileBig; odd=fertile, +2 per stage) - fully crop-agnostic. New crops therefore need their
	  OWN unused 10-wide id block past the last existing one (>=181) that follows this exact odd/even +2 convention,
	  and the 8 inlined range checks widened (all patched the same way: 0x4b -> a larger max-width byte) so the new
	  blocks are recognized as growable at all. Both are done in memory only (Plus rule 1); nothing is ever written to
	  the .exe on disk.

	Client + server config, both a matching Substance/TerrainMaterial pair per stage (script data, see
	scripts/cm_substances.cs and art/terrains/materials.cs) and a client Sow ability (skill_types.xml, mirroring
	Wheat's ability 133 block under a new id) are needed for a crop to be usable; this hook only does the server-side
	native part.

	Ability requirement XML (<ent_req type="substance">...) is validated at boot against a THIRD, separate mechanism:
	a compiled-in registry, Geo::SubstanceManager (global singleton, byte-ter2id-keyed hash table, looked up via
	RVA 0x572DE0). Only substances present in that registry may be referenced there - our custom ids are not, and
	referencing them there aborts the boot ("invalid substance ter2ID: N"). Sowing/growth never consult this
	registry (they touch the terrain byte directly), so they work regardless; only ability code that explicitly
	requires "a cell with substance X" (e.g. the shared "Harvest Crops" ability, id 73) needs it. Rather than
	reverse the full registry population (never located - the server ships no cm_substances.cs, so it's most likely
	a compiled static table, not something we can straightforwardly extend by inserting real new entries), this
	hook instead intercepts the lookup function itself: for any of our crops' 6 stage-substance ids (fertileSmall,
	fertileSmall+1..+5 = the vanilla NonFertileSmall/FertileNormal/NonFertileNormal/FertileBig/NonFertileBig
	convention), the query is transparently remapped to Wheat's own analogous stage id (101 + offset) before
	calling the real lookup. This makes every consumer of that registry (ability requirement validation, and the
	terrain friction/speed-multiplier readers) treat our custom substances as behaving exactly like the
	corresponding Wheat stage, while the terrain itself still stores the real, distinct substance byte for growth
	and visuals. No config needed beyond the existing <crop fertileSmallSubstance=...> - the alias base is derived
	from it directly.

	Actually performing a harvest (AbilityImp::HarvestPlant::_onDoPerform, ability_harvestplant.cpp) does NOT use
	the ability-id-keyed crop map at all - it looks up the CURRENT substance byte in a fourth, separate
	unordered_map<u8, u32> (get-or-default-insert helper RVA 0x3A94A0, map global RVA 0xB98D50) to learn which
	item harvesting it gives; a substance absent from that map yields "Bad substance" and aborts. RegisterOneCrop
	inserts the crop's own seed item under its two Big-stage substance ids (fertileSmall+4/+5) into this map too.

	    <cropTypes enabled="1" verbose="1" maxSubstanceWidth="136" maxGatherableType="224">
	        <crop abilityId="360" seedItemId="3175" fertileSmallSubstance="191" nonFertileSmallSubstance="192" name="Rye" />
	        <crop abilityId="363" seedItemId="3751" fertileSmallSubstance="221" nonFertileSmallSubstance="222" aliasBase="161" name="Hops" />
	    </cropTypes>
*/

#include "server/cm_server.h"

namespace tinyxml2 { class XMLElement; }

namespace Hooks
{
	namespace Engine
	{
		bool ConfigureCropTypes(const tinyxml2::XMLElement* root);
		void AttachCropTypesHook();
	}
}
