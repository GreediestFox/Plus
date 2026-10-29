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
	New plantable crops. See hook_crop_types.h for the design.
*/

#include "hook_crop_types.h"
#include "hook_patch_util.h"

#include "core/tinyxml2.h"
#include "server/api/t3d_console.h"
#include "server/cm_offsets.h"

#include <Windows.h>
#include <detours/detours.h>

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

// AbilityManager::AbilityManager()'s "register all built-in abilities" step. Takes the AbilityManager instance,
// registers Wheat/Peas/.../Potatoes (and every other built-in ability), returns success. RVA 0x31D040.
__CM_DECL_EXTERNAL(bool, __fastcall, _RegisterBuiltinAbilities, LPVOID abilityManager);
__CM_INSTATNTIATE(_RegisterBuiltinAbilities);
// AbilityImp base-object init, called on every newly allocated ability object before its vtable is installed.
// RVA 0x317020.
typedef void(__fastcall* BaseAbilityInit_Fn)(LPVOID);
// AbilityManager::_registerAbility(this, id, ability). Stamps `id` onto ability+8 and links it into the registry.
// RVA 0x327D20.
typedef bool(__fastcall* RegisterAbility_Fn)(LPVOID abilityManager, std::uint32_t id, LPVOID ability);
// The engine's own allocator used for every ability object (0x198 bytes each). RVA 0x6DF950.
typedef LPVOID(__fastcall* Alloc_Fn)(std::size_t);
// The crop-parameters std::unordered_map<u32,Record>::operator[]-equivalent: get-or-default-insert by ability id,
// returns a pointer to the record (u32 seedItemId; u8 fertileSmallSubstance; u8 nonFertileSmallSubstance; ...).
// RVA 0x3A95C0. First argument is the (global, always-live) map object at RVA 0xB98D10.
typedef std::uint8_t*(__fastcall* CropRecordOf_Fn)(LPVOID map, const std::uint32_t* key);
// A SEPARATE get-or-default-insert map, keyed by raw substance byte (not ability id!), returning a
// pointer to a plain u32 harvest-output-item-id value. This is what AbilityImp::HarvestPlant::_onDoPerform
// (via its own small helper) actually reads to decide what item harvesting a substance gives - completely
// independent of the ability-id-keyed crop map above. RVA 0x3A94A0; the map global is at RVA 0xB98D50.
typedef std::uint8_t*(__fastcall* HarvestItemOf_Fn)(LPVOID map, const std::uint8_t* key);
// Geo::SubstanceManager's byte-ter2id-keyed lookup (returns null if the id was never registered in the compiled
// substance table). Used both by ability-requirement XML validation (Substance_EntityRequirement::_init) and by
// the terrain friction/speed-multiplier readers. RVA 0x572DE0. First argument is the manager singleton itself
// (global at RVA 0xB81668, lazily constructed - callers always fetch it live rather than caching the pointer).
__CM_DECL_EXTERNAL(std::intptr_t, __fastcall, _SubstanceLookupByTer2Id, LPVOID mgr, std::uint8_t ter2Id);
__CM_INSTATNTIATE(_SubstanceLookupByTer2Id);

// AbilityImp::Inspect's "which seed item grows in this crop block" lookup, RVA 0x3A9700. Walks the crop record list
// (fertileSmall/nonFertileSmall substance at +0x18/+0x19, seed item at +0x14) for the block's first substance id
// (stage id - stage id % 10 + 1). Its only caller is AbilityImp::Inspect::_inspectEntity (RVA 0x395AC0), which
// feeds it the block of the substance record it just looked up - for a custom crop that is the ALIASED (vanilla)
// record, so without help it reports the alias target's item (e.g. Wheat for a custom grain).
__CM_DECL_EXTERNAL(std::uint32_t, __fastcall, _InspectSeedItemOf, std::uint32_t blockFirstSubstance);
__CM_INSTATNTIATE(_InspectSeedItemOf);

// gatherables.cpp isValid(type) helper, RVA 0x379A00 (first instruction is `cmp ecx,0xDA`). Returns the validity in al.
__CM_DECL_EXTERNAL(std::uint8_t, __fastcall, _GatherableIsValid, std::uint32_t type);
__CM_INSTATNTIATE(_GatherableIsValid);

namespace Hooks
{
    namespace Engine
    {
        namespace
        {
            constexpr std::uintptr_t kRegisterBuiltinAbilitiesRva = CmOffset::CROP_REGISTER_BUILTIN_ABILITIES;
            constexpr std::uintptr_t kBaseAbilityInitRva = CmOffset::CROP_BASE_ABILITY_INIT;
            constexpr std::uintptr_t kRegisterAbilityRva = CmOffset::CROP_REGISTER_ABILITY;
            constexpr std::uintptr_t kAllocRva = CmOffset::ENGINE_ALLOC;
            constexpr std::uintptr_t kCropRecordOfRva = CmOffset::CROP_RECORD_OF;
            constexpr std::uintptr_t kCropMapRva = CmOffset::CROP_MAP;
            constexpr std::uintptr_t kHarvestItemOfRva = CmOffset::CROP_HARVEST_ITEM_OF;
            constexpr std::uintptr_t kHarvestItemMapRva = CmOffset::CROP_HARVEST_ITEM_MAP;
            constexpr std::uintptr_t kPlantWheatAbilityVtableRva = CmOffset::CROP_PLANT_WHEAT_VTABLE; // reused as-is for every new crop
            constexpr std::size_t kAbilityObjectSize = 0x198;

            // The 8 near-identical inlined "is this substance id a growable crop" range checks in
            // cropsmaintenance.cpp (`cmp al,0x4b` = compare (substanceId-101) against the vanilla width-1, 75).
            // Patching the immediate widens the recognized range so newly added crop blocks (>=181) grow too.
            // Each site below is the RVA of the instruction's opcode byte (0x3C); the immediate we patch is +1.
            constexpr std::uintptr_t kRangeCheckSites[] = {
                CmOffset::CROP_RANGE_CHECK_0,
                CmOffset::CROP_RANGE_CHECK_1,
                CmOffset::CROP_RANGE_CHECK_2,
                CmOffset::CROP_RANGE_CHECK_3,
                CmOffset::CROP_RANGE_CHECK_4,
                CmOffset::CROP_RANGE_CHECK_5,
                CmOffset::CROP_RANGE_CHECK_6,
                CmOffset::CROP_RANGE_CHECK_7,
            };
            constexpr std::uint8_t kRangeCheckOpcode = 0x3C;         // cmp al, imm8
            constexpr std::uintptr_t kRangeCheckImmOffset = 1;
            // The check is `lea eax,[rbx-0x65]; cmp al,imm8; ja not_a_crop`: the subtraction wraps in 8 bits, so ids 0..100
            // become 155..255. An immediate of 155 or more would therefore accept low ids (0..imm-155) as crops.
            // The widest safe width is 155 (imm 154 = ids 101..255).
            constexpr int kMaxSafeSubstanceWidth = 155;
            constexpr std::uint8_t kRangeCheckOldWidthMinus1 = 0x4b; // vanilla: ids 101..176 (8 crops * 10-wide blocks)

            // gatherables.cpp: four "is this a valid gatherable type" checks (`cmp ecx, 0xDA` = 81 F9 DA 00 00 00): the isValid()
            // helper at 0x379A00 plus three accessors. Types 0..218 come from data/gatherables.xml and 219 is the engine's
            // own "none" sentinel. Raising the immediate lets extra <gatherable> entries (type >= 220, e.g. wild versions of
            // the custom crops) pass the three accessors; the type travels to the client as one byte, so up to 254 fits.
            // The client exe needs the same change (5 sites there).
            //
            // isValid() itself is NOT patched: raising its immediate would also make the sentinel 219 valid. It gets a
            // small filter hook instead (OnGatherableIsValid): 220..maxGatherableType are valid, everything else is
            // decided by the untouched original, so 219 stays invalid there.
            // Each accessor site is the RVA of the instruction; the immediate's low byte is at +2.
            constexpr std::uintptr_t kGatherableAccessorSites[] = {
                CmOffset::CROP_GATHERABLE_LIMIT_0, CmOffset::CROP_GATHERABLE_LIMIT_1, CmOffset::CROP_GATHERABLE_LIMIT_2 };
            constexpr std::uintptr_t kGatherableIsValidRva = CmOffset::CROP_GATHERABLE_IS_VALID;
            constexpr std::uint8_t kGatherableLimitInsn[] = { 0x81, 0xf9, 0xda, 0x00, 0x00, 0x00 };
            constexpr std::uintptr_t kGatherableLimitImmOffset = 2;
            constexpr int kVanillaMaxGatherableType = 0xda;
            constexpr int kGatherableNoneSentinel = 0xdb;     // 219

            // Geo::SubstanceManager's ter2id lookup. Every custom crop substance id in [base, base+6) gets aliased
            // to its configured alias target's corresponding stage (aliasBase + offset) when this specific lookup
            // is queried - see the header comment for why. Defaults to Wheat (101); a crop can pick a different
            // vanilla crop to alias (e.g. Grape, 161) via <crop aliasBase="161">, for cases where its friction/
            // speed profile or harvest mechanics should follow a different vanilla crop than Wheat's.
            constexpr std::uintptr_t kSubstanceLookupByTer2IdRva = CmOffset::CROP_SUBSTANCE_LOOKUP;
            constexpr std::uint8_t kWheatFertileSmallSubstance = 101;
            constexpr std::uintptr_t kInspectSeedItemOfRva = CmOffset::CROP_INSPECT_SEED_ITEM_OF;

            // Last alias applied by OnSubstanceLookupByTer2Id on this thread (0 = the last lookup was not aliased).
            // Inspect calls the substance lookup and then _InspectSeedItemOf right after it on the same thread, so
            // OnInspectSeedItemOf can swap the alias target's block back to the real crop's block.
            thread_local std::uint8_t tlsLastRawCropSubstance = 0;
            thread_local std::uint8_t tlsLastAliasSubstance = 0;

            struct CropDef
            {
                std::uint32_t abilityId = 0;
                std::uint32_t seedItemId = 0;
                std::uint8_t fertileSmallSubstance = 0;
                std::uint8_t nonFertileSmallSubstance = 0;
                std::uint8_t aliasBase = kWheatFertileSmallSubstance; // which vanilla crop's stage ids to alias to
                std::string name;
            };

            struct State
            {
                std::vector<CropDef> crops;
                bool enabled = false;
                bool attached = false;
                bool verbose = true;
                int maxSubstanceWidth = 180; // widened range: substance ids 101..(101+maxSubstanceWidth-1)
                int maxGatherableType = kVanillaMaxGatherableType; // highest valid gatherables.xml type (218 = unchanged)
                std::uintptr_t base = 0;

                BytePatchSet rangePatches;       // the 8 widened range checks
                BytePatchSet gatherPatches;      // the 3 gatherable accessor limits
                bool coreAttached = false;       // registration + substance-alias detours
                bool inspectAttached = false;
                bool gatherHookAttached = false;
            };

            State& S()
            {
                static State* state = new State(); // intentionally leaked (hooks may fire during shutdown)
                return *state;
            }

            void FileLog(const char* fmt, ...)
            {
                char msg[400];
                va_list args;
                va_start(args, fmt);
                vsnprintf(msg, sizeof(msg), fmt, args);
                va_end(args);
                std::time_t now = std::time(nullptr);
                std::tm tmv{};
                localtime_s(&tmv, &now);
                char stamp[32];
                std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tmv);
                std::FILE* f = nullptr;
                if (fopen_s(&f, "logs/crop_types.log", "a") == 0 && f)
                {
                    std::fprintf(f, "%s %s\n", stamp, msg);
                    std::fclose(f);
                }
            }

            BaseAbilityInit_Fn BaseAbilityInit = nullptr;
            RegisterAbility_Fn RegisterAbilityFn = nullptr;
            Alloc_Fn AllocFn = nullptr;
            CropRecordOf_Fn CropRecordOfFn = nullptr;
            HarvestItemOf_Fn HarvestItemOfFn = nullptr;

            bool RegisterOneCrop(LPVOID abilityManager, const CropDef& crop)
            {
                State& s = S();
                LPVOID obj = AllocFn(kAbilityObjectSize);
                if (!obj)
                {
                    FileLog("crop %s (ability %u): allocation failed", crop.name.c_str(), crop.abilityId);
                    return false;
                }
                std::memset(obj, 0, kAbilityObjectSize);
                BaseAbilityInit(obj);
                *reinterpret_cast<std::uintptr_t*>(obj) = s.base + kPlantWheatAbilityVtableRva;
                if (!RegisterAbilityFn(abilityManager, crop.abilityId, obj))
                {
                    FileLog("crop %s: ability id %u could not be registered (already used, or id > 399?)",
                        crop.name.c_str(), crop.abilityId);
                    return false;
                }
                std::uint32_t key = crop.abilityId;
                std::uint8_t* record = CropRecordOfFn(reinterpret_cast<LPVOID>(s.base + kCropMapRva), &key);
                if (!record)
                {
                    FileLog("crop %s: registered as ability %u but its data row could not be created", crop.name.c_str(), crop.abilityId);
                    return false;
                }
                *reinterpret_cast<std::uint32_t*>(record) = crop.seedItemId;
                record[4] = crop.fertileSmallSubstance;
                record[5] = crop.nonFertileSmallSubstance;

                // Harvest reads a SEPARATE, substance-keyed map to decide what item it gives - only the
                // Big-stage substances (fertileSmall+4/+5, following the vanilla 10-wide-block convention)
                // are ever targeted by the shared "Harvest Crops" ability, so only those two need an entry.
                std::uint8_t fertileBig = static_cast<std::uint8_t>(crop.fertileSmallSubstance + 4);
                std::uint8_t nonFertileBig = static_cast<std::uint8_t>(crop.fertileSmallSubstance + 5);
                std::uint8_t* fertileItem = HarvestItemOfFn(reinterpret_cast<LPVOID>(s.base + kHarvestItemMapRva), &fertileBig);
                std::uint8_t* nonFertileItem = HarvestItemOfFn(reinterpret_cast<LPVOID>(s.base + kHarvestItemMapRva), &nonFertileBig);
                if (!fertileItem || !nonFertileItem)
                {
                    FileLog("crop %s: registered but harvest-item map rows could not be created", crop.name.c_str());
                    return false;
                }
                // The record is {u32 harvestItemId; u8 postHarvestSubstance; ...} - byte+4 is what the tile turns
                // into after harvesting (AbilityImp::HarvestPlant::_onDoPerform's second "Bad substance" check,
                // separate from the first one this map's +0 field already fixes). Rather than guess a value,
                // copy it from the crop's own alias target's already-populated entries (e.g. 105/106 for Wheat,
                // 165/166 for Grape) for the matching fertility.
                std::uint8_t aliasFertileBig = static_cast<std::uint8_t>(crop.aliasBase + 4);
                std::uint8_t aliasNonFertileBig = static_cast<std::uint8_t>(crop.aliasBase + 5);
                std::uint8_t* aliasFertileItem = HarvestItemOfFn(reinterpret_cast<LPVOID>(s.base + kHarvestItemMapRva), &aliasFertileBig);
                std::uint8_t* aliasNonFertileItem = HarvestItemOfFn(reinterpret_cast<LPVOID>(s.base + kHarvestItemMapRva), &aliasNonFertileBig);
                std::uint8_t postHarvestFertile = aliasFertileItem ? aliasFertileItem[4] : 0;
                std::uint8_t postHarvestNonFertile = aliasNonFertileItem ? aliasNonFertileItem[4] : 0;

                *reinterpret_cast<std::uint32_t*>(fertileItem) = crop.seedItemId;
                fertileItem[4] = postHarvestFertile;
                *reinterpret_cast<std::uint32_t*>(nonFertileItem) = crop.seedItemId;
                nonFertileItem[4] = postHarvestNonFertile;

                FileLog("crop %s registered: ability id %u, seed item %u, substances %u/%u (fertile/non-fertile, small stage), "
                    "harvest item %u wired for substances %u/%u (fertile/non-fertile, big stage), post-harvest substances %u/%u",
                    crop.name.c_str(), crop.abilityId, crop.seedItemId, crop.fertileSmallSubstance, crop.nonFertileSmallSubstance,
                    crop.seedItemId, fertileBig, nonFertileBig, postHarvestFertile, postHarvestNonFertile);
                return true;
            }

            bool __fastcall OnRegisterBuiltinAbilities(LPVOID abilityManager)
            {
                bool ok = _RegisterBuiltinAbilities(abilityManager);
                if (!ok)
                    return ok;
                State& s = S();
                for (const CropDef& crop : s.crops)
                    RegisterOneCrop(abilityManager, crop);
                return ok;
            }

            std::intptr_t __fastcall OnSubstanceLookupByTer2Id(LPVOID mgr, std::uint8_t ter2Id)
            {
                State& s = S();
                for (const CropDef& crop : s.crops)
                {
                    std::uint8_t base = crop.fertileSmallSubstance;
                    if (ter2Id >= base && ter2Id < base + 6)
                    {
                        std::uint8_t alias = static_cast<std::uint8_t>(crop.aliasBase + (ter2Id - base));
                        tlsLastRawCropSubstance = ter2Id;
                        tlsLastAliasSubstance = alias;
                        return _SubstanceLookupByTer2Id(mgr, alias);
                    }
                }
                tlsLastRawCropSubstance = 0;
                tlsLastAliasSubstance = 0;
                return _SubstanceLookupByTer2Id(mgr, ter2Id);
            }

            std::uint8_t __fastcall OnGatherableIsValid(std::uint32_t type)
            {
                const State& s = S();
                if (type > static_cast<std::uint32_t>(kGatherableNoneSentinel) && type <= static_cast<std::uint32_t>(s.maxGatherableType))
                    return 1;
                return _GatherableIsValid(type);
            }

            std::uint32_t __fastcall OnInspectSeedItemOf(std::uint32_t blockFirstSubstance)
            {
                std::uint8_t raw = tlsLastRawCropSubstance, alias = tlsLastAliasSubstance;
                tlsLastRawCropSubstance = 0;
                tlsLastAliasSubstance = 0;
                if (raw != 0 && blockFirstSubstance == static_cast<std::uint32_t>(alias - alias % 10 + 1))
                {
                    std::uint32_t own = static_cast<std::uint32_t>(raw - raw % 10 + 1);
                    std::uint32_t item = _InspectSeedItemOf(own);
                    if (item != 0)
                        return item;
                }
                return _InspectSeedItemOf(blockFirstSubstance);
            }
        }

        bool ConfigureCropTypes(const tinyxml2::XMLElement* root)
        {
            State& s = S();
            if (s.attached)
            {
                Lifx::ShowErrorMessage("Can't configure cropTypes while the hook is attached.");
                return false;
            }
            s.crops.clear();
            s.enabled = false;
            if (root == nullptr)
                return true;

            const tinyxml2::XMLElement* section = root->FirstChildElement("cropTypes");
            if (section == nullptr || !section->BoolAttribute("enabled", false))
                return true;
            s.verbose = section->BoolAttribute("verbose", true);
            // maxSubstanceWidth is optional: when absent it is derived from the configured crops (below).
            const bool widthGiven = section->Attribute("maxSubstanceWidth") != nullptr;
            s.maxSubstanceWidth = section->IntAttribute("maxSubstanceWidth", 0);
            if (widthGiven && (s.maxSubstanceWidth < 76 || s.maxSubstanceWidth > kMaxSafeSubstanceWidth))
            {
                Lifx::ShowErrorMessage("Invalid <cropTypes maxSubstanceWidth=...>: must be 76..%d (the engine's range check is 8-bit; "
                    "a wider value would accept substance ids 0..100 as crops).", kMaxSafeSubstanceWidth);
                return false;
            }
            s.maxGatherableType = section->IntAttribute("maxGatherableType", kVanillaMaxGatherableType);
            if (s.maxGatherableType != kVanillaMaxGatherableType && (s.maxGatherableType <= kGatherableNoneSentinel || s.maxGatherableType > 254))
            {
                Lifx::ShowErrorMessage("Invalid <cropTypes maxGatherableType=...>: must be 218 (unchanged) or 220..254 "
                    "(219 is the engine's 'none' sentinel).");
                return false;
            }

            for (const tinyxml2::XMLElement* e = section->FirstChildElement("crop"); e != nullptr; e = e->NextSiblingElement("crop"))
            {
                CropDef crop;
                unsigned abilityId = 0, seedItemId = 0, fertile = 0, nonFertile = 0;
                const char* name = e->Attribute("name");
                if (e->QueryUnsignedAttribute("abilityId", &abilityId) != tinyxml2::XML_SUCCESS || abilityId < 1 || abilityId > 399
                    || e->QueryUnsignedAttribute("seedItemId", &seedItemId) != tinyxml2::XML_SUCCESS || seedItemId == 0
                    || e->QueryUnsignedAttribute("fertileSmallSubstance", &fertile) != tinyxml2::XML_SUCCESS
                    || e->QueryUnsignedAttribute("nonFertileSmallSubstance", &nonFertile) != tinyxml2::XML_SUCCESS
                    || name == nullptr)
                {
                    Lifx::ShowErrorMessage("Invalid <cropTypes><crop>: name, abilityId (141..399), seedItemId, "
                        "fertileSmallSubstance and nonFertileSmallSubstance (181..250) are all required.");
                    s.crops.clear();
                    return false;
                }
                // The growth arithmetic is id%10 driven (1 = FertileSmall, 2 = NonFertileSmall, ... 6 = NonFertileBig), so the
                // crop needs its own 10-wide block past the vanilla ones: fertileSmall = 10k+1 (>= 181), nonFertileSmall = +1.
                // Six stage ids are used (fertileSmall..+5) and must fit in a byte.
                if (fertile < 181 || fertile % 10 != 1 || fertile + 5 > 255 || nonFertile != fertile + 1)
                {
                    Lifx::ShowErrorMessage("Invalid <crop %s>: fertileSmallSubstance must be 181, 191, ... (id %% 10 == 1, stages up to +5 must be <= 255) "
                        "and nonFertileSmallSubstance must be fertileSmallSubstance + 1.", name);
                    s.crops.clear();
                    return false;
                }
                bool overlap = false;
                for (const CropDef& other : s.crops)
                    if (other.abilityId == abilityId || (fertile / 10) == (other.fertileSmallSubstance / 10))
                        overlap = true;
                if (overlap)
                {
                    Lifx::ShowErrorMessage("Invalid <crop %s>: abilityId or 10-wide substance block already used by another crop.", name);
                    s.crops.clear();
                    return false;
                }
                unsigned aliasBase = kWheatFertileSmallSubstance;
                e->QueryUnsignedAttribute("aliasBase", &aliasBase); // optional; defaults to Wheat (101)
                if (aliasBase != 101 && aliasBase != 111 && aliasBase != 121 && aliasBase != 131
                    && aliasBase != 141 && aliasBase != 151 && aliasBase != 161 && aliasBase != 171)
                {
                    Lifx::ShowErrorMessage("Invalid <crop aliasBase=...>: must be one of the 8 vanilla crops' "
                        "FertileSmall ids (101/111/121/131/141/151/161/171).");
                    s.crops.clear();
                    return false;
                }
                crop.abilityId = abilityId;
                crop.seedItemId = seedItemId;
                crop.fertileSmallSubstance = static_cast<std::uint8_t>(fertile);
                crop.nonFertileSmallSubstance = static_cast<std::uint8_t>(nonFertile);
                crop.aliasBase = static_cast<std::uint8_t>(aliasBase);
                crop.name = name;
                s.crops.push_back(crop);
            }
            if (s.crops.empty())
                return true; // enabled but empty: nothing to do

            // Width needed to recognise every stage id of every crop: ids 101..(highest fertileSmall + 5).
            int needed = 76;
            for (const CropDef& crop : s.crops)
                needed = std::max(needed, static_cast<int>(crop.fertileSmallSubstance) + 5 - 100);
            if (!widthGiven)
                s.maxSubstanceWidth = needed;
            else if (s.maxSubstanceWidth < needed)
            {
                Lifx::ShowErrorMessage("<cropTypes maxSubstanceWidth=%d> is too small for the configured crops: at least %d is needed.",
                    s.maxSubstanceWidth, needed);
                s.crops.clear();
                return false;
            }

            s.enabled = true;
            return true;
        }

        void AttachCropTypesHook()
        {
            State& s = S();
            if (!s.enabled || s.attached)
                return;

            s.base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
            if (s.base == 0)
            {
                Lifx::ShowErrorMessage("Can't attach crop-types hook: main module base is null.");
                return;
            }

            // Verify every patch/call site belongs to the expected build before touching anything.
            static constexpr std::uint8_t registerFnPrologue[] = { 0x40, 0x55, 0x48, 0x8b, 0xec, 0x48, 0x83, 0xec, 0x30 };
            static constexpr std::uint8_t recordOfPrologue[] = { 0x40, 0x55, 0x57, 0x48, 0x83, 0xec, 0x48 };
            static constexpr std::uint8_t registerAbilityPrologue[] = { 0x89, 0x54, 0x24, 0x10, 0x56, 0x48, 0x83, 0xec, 0x40 };
            static constexpr std::uint8_t allocPrologue[] = { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20 };
            static constexpr std::uint8_t baseInitPrologue[] = { 0x48, 0x89, 0x4c, 0x24, 0x08, 0x55, 0x56, 0x57 };
            static constexpr std::uint8_t substanceLookupPrologue[] = {
                0x4c, 0x8b, 0x49, 0x10, 0x44, 0x0f, 0xb6, 0xc2, 0x4d, 0x85, 0xc9, 0x74, 0x23, 0x0f, 0xb6, 0xc2 };
            static constexpr std::uint8_t harvestItemOfPrologue[] = {
                0x40, 0x53, 0x55, 0x56, 0x48, 0x83, 0xec, 0x40 };
            bool ok = std::memcmp(reinterpret_cast<const void*>(s.base + kRegisterBuiltinAbilitiesRva), registerFnPrologue, sizeof(registerFnPrologue)) == 0
                && std::memcmp(reinterpret_cast<const void*>(s.base + kCropRecordOfRva), recordOfPrologue, sizeof(recordOfPrologue)) == 0
                && std::memcmp(reinterpret_cast<const void*>(s.base + kRegisterAbilityRva), registerAbilityPrologue, sizeof(registerAbilityPrologue)) == 0
                && std::memcmp(reinterpret_cast<const void*>(s.base + kAllocRva), allocPrologue, sizeof(allocPrologue)) == 0
                && std::memcmp(reinterpret_cast<const void*>(s.base + kBaseAbilityInitRva), baseInitPrologue, sizeof(baseInitPrologue)) == 0
                && std::memcmp(reinterpret_cast<const void*>(s.base + kSubstanceLookupByTer2IdRva), substanceLookupPrologue, sizeof(substanceLookupPrologue)) == 0
                && std::memcmp(reinterpret_cast<const void*>(s.base + kHarvestItemOfRva), harvestItemOfPrologue, sizeof(harvestItemOfPrologue)) == 0;
            for (std::uintptr_t site : kRangeCheckSites)
            {
                // Opcode AND immediate: the instruction must really be `cmp al,0x4b`.
                if (*reinterpret_cast<const std::uint8_t*>(s.base + site) != kRangeCheckOpcode
                    || *reinterpret_cast<const std::uint8_t*>(s.base + site + kRangeCheckImmOffset) != kRangeCheckOldWidthMinus1)
                    ok = false;
            }
            if (!ok)
            {
                Lifx::ShowErrorMessage("Can't attach crop-types hook: expected code not found (different server build?).");
                return;
            }

            // Widen the 8 inlined "is this a growable crop substance" range checks. Recorded in rangePatches so a failure
            // anywhere below puts the original bytes back.
            const std::uint8_t newWidthMinus1 = static_cast<std::uint8_t>(s.maxSubstanceWidth - 1);
            for (std::uintptr_t site : kRangeCheckSites)
            {
                if (!s.rangePatches.WriteByte(s.base + site + kRangeCheckImmOffset, newWidthMinus1))
                {
                    Lifx::ShowErrorMessage("Crop-types range-widen failed (VirtualProtect) at one of 8 sites; nothing was changed.");
                    s.rangePatches.Rollback();
                    return;
                }
            }

            BaseAbilityInit = reinterpret_cast<BaseAbilityInit_Fn>(s.base + kBaseAbilityInitRva);
            RegisterAbilityFn = reinterpret_cast<RegisterAbility_Fn>(s.base + kRegisterAbilityRva);
            AllocFn = reinterpret_cast<Alloc_Fn>(s.base + kAllocRva);
            CropRecordOfFn = reinterpret_cast<CropRecordOf_Fn>(s.base + kCropRecordOfRva);
            HarvestItemOfFn = reinterpret_cast<HarvestItemOf_Fn>(s.base + kHarvestItemOfRva);
            _RegisterBuiltinAbilities = reinterpret_cast<_RegisterBuiltinAbilities_Fn>(s.base + kRegisterBuiltinAbilitiesRva);
            _SubstanceLookupByTer2Id = reinterpret_cast<_SubstanceLookupByTer2Id_Fn>(s.base + kSubstanceLookupByTer2IdRva);

            // Core detours (registration + substance alias) in their own transaction; failure rolls the range patches back.
            {
                DetourTx tx;
                if (!tx.Attach(&(PVOID&)_RegisterBuiltinAbilities, OnRegisterBuiltinAbilities)
                    || !tx.Attach(&(PVOID&)_SubstanceLookupByTer2Id, OnSubstanceLookupByTer2Id)
                    || !tx.Commit())
                {
                    Lifx::ShowErrorMessage("Detours attach failed for the crop-types hooks. Error: %ld", tx.LastError());
                    s.rangePatches.Rollback();
                    return;
                }
                s.coreAttached = true;
            }

            // Inspect shows the real crop's name instead of the alias target's (optional: skipped on mismatch).
            static constexpr std::uint8_t inspectSeedItemPrologue[] = { 0x48, 0x8b, 0x15, 0x11, 0xf6, 0x7e, 0x00, 0x48, 0x8b, 0x02 };
            if (std::memcmp(reinterpret_cast<const void*>(s.base + kInspectSeedItemOfRva), inspectSeedItemPrologue, sizeof(inspectSeedItemPrologue)) == 0)
            {
                _InspectSeedItemOf = reinterpret_cast<_InspectSeedItemOf_Fn>(s.base + kInspectSeedItemOfRva);
                DetourTx tx;
                s.inspectAttached = tx.Attach(&(PVOID&)_InspectSeedItemOf, OnInspectSeedItemOf) && tx.Commit();
                FileLog(s.inspectAttached ? "inspect crop-name fix attached" : "inspect crop-name fix NOT attached (Detours %ld)", tx.LastError());
            }
            else
            {
                FileLog("inspect crop-name fix NOT attached: expected code not found at 0x3A9700");
            }

            // Optionally raise the highest valid gatherable type: three accessor immediates plus the isValid filter hook.
            // Independent of the crop hooks: a mismatch or failure only leaves the extra gatherables.xml entries invalid.
            if (s.maxGatherableType != kVanillaMaxGatherableType)
            {
                bool gatherOk = true;
                for (std::uintptr_t site : kGatherableAccessorSites)
                    if (std::memcmp(reinterpret_cast<const void*>(s.base + site), kGatherableLimitInsn, sizeof(kGatherableLimitInsn)) != 0)
                        gatherOk = false;
                if (std::memcmp(reinterpret_cast<const void*>(s.base + kGatherableIsValidRva), kGatherableLimitInsn, sizeof(kGatherableLimitInsn)) != 0)
                    gatherOk = false;
                if (!gatherOk)
                {
                    FileLog("gatherable type limit NOT raised: expected code not found at the 4 sites");
                }
                else
                {
                    for (std::uintptr_t site : kGatherableAccessorSites)
                        if (!s.gatherPatches.WriteByte(s.base + site + kGatherableLimitImmOffset, static_cast<std::uint8_t>(s.maxGatherableType)))
                            gatherOk = false;
                    if (gatherOk)
                    {
                        _GatherableIsValid = reinterpret_cast<_GatherableIsValid_Fn>(s.base + kGatherableIsValidRva);
                        DetourTx tx;
                        gatherOk = tx.Attach(&(PVOID&)_GatherableIsValid, OnGatherableIsValid) && tx.Commit();
                    }
                    if (gatherOk)
                    {
                        s.gatherHookAttached = true;
                        FileLog("gatherable type limit raised: valid types 0..218 and 220..%d (219 stays the 'none' sentinel)", s.maxGatherableType);
                    }
                    else
                    {
                        s.gatherPatches.Rollback();
                        FileLog("gatherable type limit NOT raised: patch or isValid hook failed, accessors restored");
                    }
                }
            }
            FlushInstructionCache(GetCurrentProcess(), nullptr, 0);

            // The patch sets keep their original bytes so DetachCropTypesHook can restore them.
            s.attached = true;
            FileLog("hooks attached: substance range widened to 101..%d, %u crop(s) configured",
                101 + s.maxSubstanceWidth - 1, static_cast<unsigned>(s.crops.size()));
        }

        void DetachCropTypesHook()
        {
            State& s = S();
            if (!s.attached)
                return;
            if (s.gatherHookAttached)
            {
                DetourTx tx;
                if (tx.Detach(&(PVOID&)_GatherableIsValid, OnGatherableIsValid) && tx.Commit())
                {
                    s.gatherHookAttached = false;
                    s.gatherPatches.Rollback();
                }
            }
            if (s.inspectAttached)
            {
                DetourTx tx;
                if (tx.Detach(&(PVOID&)_InspectSeedItemOf, OnInspectSeedItemOf) && tx.Commit())
                    s.inspectAttached = false;
            }
            if (s.coreAttached)
            {
                DetourTx tx;
                if (tx.Detach(&(PVOID&)_RegisterBuiltinAbilities, OnRegisterBuiltinAbilities)
                    && tx.Detach(&(PVOID&)_SubstanceLookupByTer2Id, OnSubstanceLookupByTer2Id) && tx.Commit())
                {
                    s.coreAttached = false;
                    s.rangePatches.Rollback();
                }
            }
            // Crops registered in the engine's maps stay registered; only the code changes are undone.
            s.attached = s.coreAttached || s.inspectAttached || s.gatherHookAttached;
        }
    }
}
