/* ===================================================================================
	Copyright (c) 2026, LiFx Contributors. All Rights Reserved.
	Contributed by Pabluuz.

	This file is a part of LiFx.

	LIFX IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
	EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
	MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
	IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
	DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
	ARISING FROM, OUT OF OR IN CONNECTION WITH LIFX OR THE USE OR OTHER
	DEALINGS IN LIFX.
*  =================================================================================== */

/* ===================================================================================
    Gem drop hooks for the confirmed ddctd_cm_yo_server build.

    This code does not touch SQL or inventory refresh logic. It changes:
      1. the probability threshold at the confirmed gem-roll call site;
      2. the selected itemId immediately before the normal game item lookup.
*  =================================================================================== */

#include "hook_gem_drop.h"

#include "core/tinyxml2.h"

#include <Windows.h>
#include <detours/detours.h>
#include <intrin.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <random>
#include <string>
#include <vector>

__CM_INSTATNTIATE(_Engine_ComputeDropChance);
__CM_INSTATNTIATE(_Engine_ItemTypeLookup);

namespace Hooks
{
    namespace Engine
    {
        namespace
        {
            // --------------------------------------------------------------------- //
            // Confirmed call sites for the currently analysed server executable.

            // Gem probability:
            //   +3A9258 call ComputeDropChance
            //   +3A925D movaps xmm6,xmm0
            constexpr std::uintptr_t kGemChanceCallRva = 0x3A9258;
            constexpr std::uintptr_t kGemChanceReturnRva = 0x3A925D;

            // Gem item selection:
            //   +3A931F call ItemTypeLookup
            //   +3A9324 mov r14,rax
            constexpr std::uintptr_t kGemItemLookupCallRva = 0x3A931F;
            constexpr std::uintptr_t kGemItemLookupReturnRva = 0x3A9324;

            constexpr U32 kFirstOriginalGemItemId = 481;
            constexpr U32 kLastOriginalGemItemId = 486;

            struct WeightedItem
            {
                U32 itemId = 0;
                std::uint64_t cumulativeWeight = 0;
            };

            struct GemDropState
            {
                bool enabled = false;
                bool overrideChance = false;
                bool overrideTable = false;
                bool chanceHookAttached = false;
                bool itemHookAttached = false;

                F32 chance = 0.0f; // normalized 0.0..1.0
                std::uint64_t totalWeight = 0;
                std::vector<WeightedItem> items;

                std::uintptr_t moduleBase = 0;
                std::uintptr_t gemChanceReturnAddress = 0;
                std::uintptr_t gemItemLookupReturnAddress = 0;
            };

            GemDropState gGemDrops;

            // --------------------------------------------------------------------- //
            std::uintptr_t DecodeRelativeCallTarget(std::uintptr_t callAddress)
            {
                std::int32_t displacement = 0;
                std::memcpy(
                    &displacement,
                    reinterpret_cast<const void*>(callAddress + 1),
                    sizeof(displacement));

                return callAddress + 5 + static_cast<std::intptr_t>(displacement);
            }

            // --------------------------------------------------------------------- //
            bool MatchBytes(
                std::uintptr_t address,
                const std::uint8_t* expected,
                std::size_t size)
            {
                return std::memcmp(
                    reinterpret_cast<const void*>(address),
                    expected,
                    size) == 0;
            }

            // --------------------------------------------------------------------- //
            bool MatchesGemChanceCallSite(std::uintptr_t callAddress)
            {
                // +3A924E movaps xmm2,xmm9
                // +3A9252 movaps xmm1,xmm8
                // +3A9256 mov ecx,edi
                static constexpr std::uint8_t before[] = {
                    0x41, 0x0F, 0x28, 0xD1,
                    0x41, 0x0F, 0x28, 0xC8,
                    0x8B, 0xCF
                };

                // +3A925D movaps xmm6,xmm0
                static constexpr std::uint8_t after[] = {
                    0x0F, 0x28, 0xF0
                };

                return MatchBytes(callAddress - sizeof(before), before, sizeof(before))
                    && *reinterpret_cast<const std::uint8_t*>(callAddress) == 0xE8
                    && MatchBytes(callAddress + 5, after, sizeof(after));
            }

            // --------------------------------------------------------------------- //
            bool MatchesGemItemLookupCallSite(std::uintptr_t callAddress)
            {
                // +3A9312 mov rcx,qword ptr [rip+????????]
                const auto* before = reinterpret_cast<const std::uint8_t*>(callAddress - 13);
                if (before[0] != 0x48 || before[1] != 0x8B || before[2] != 0x0D)
                    return false;

                // Ignore the four-byte RIP displacement and validate the fixed tail:
                // +3A9319 add rcx,8
                // +3A931D mov edx,ebx
                static constexpr std::uint8_t fixedTail[] = {
                    0x48, 0x83, 0xC1, 0x08,
                    0x8B, 0xD3
                };

                static constexpr std::uint8_t after[] = {
                    0x4C, 0x8B, 0xF0, // mov r14,rax
                    0x48, 0x85, 0xC0  // test rax,rax
                };

                return std::memcmp(before + 7, fixedTail, sizeof(fixedTail)) == 0
                    && *reinterpret_cast<const std::uint8_t*>(callAddress) == 0xE8
                    && MatchBytes(callAddress + 5, after, sizeof(after));
            }

            // --------------------------------------------------------------------- //
            std::uint64_t MakeSeed()
            {
                std::random_device rd;
                const auto now = static_cast<std::uint64_t>(
                    std::chrono::high_resolution_clock::now().time_since_epoch().count());
                const auto tid = static_cast<std::uint64_t>(GetCurrentThreadId());
                const auto address = reinterpret_cast<std::uintptr_t>(&gGemDrops);

                std::seed_seq seed{
                    rd(), rd(),
                    static_cast<std::uint32_t>(now),
                    static_cast<std::uint32_t>(now >> 32),
                    static_cast<std::uint32_t>(tid),
                    static_cast<std::uint32_t>(address),
                    static_cast<std::uint32_t>(address >> 32)
                };

                std::uint32_t values[2]{};
                seed.generate(std::begin(values), std::end(values));
                return (static_cast<std::uint64_t>(values[0]) << 32) | values[1];
            }

            // --------------------------------------------------------------------- //
            U32 SelectGemItemId()
            {
                thread_local std::mt19937_64 rng(MakeSeed());
                std::uniform_int_distribution<std::uint64_t> distribution(
                    1,
                    gGemDrops.totalWeight);

                const std::uint64_t ticket = distribution(rng);
                const auto it = std::lower_bound(
                    gGemDrops.items.begin(),
                    gGemDrops.items.end(),
                    ticket,
                    [](const WeightedItem& item, std::uint64_t value)
                    {
                        return item.cumulativeWeight < value;
                    });

                return it != gGemDrops.items.end()
                    ? it->itemId
                    : gGemDrops.items.back().itemId;
            }

            // --------------------------------------------------------------------- //
            bool IsOriginalGemItemId(U32 itemId)
            {
                return itemId >= kFirstOriginalGemItemId
                    && itemId <= kLastOriginalGemItemId;
            }
        }

        // ------------------------------------------------------------------------- //
        F32 __fastcall OnComputeDropChance(U32 context, F32 baseChance, F32 bonusChance)
        {
            const auto returnAddress =
                reinterpret_cast<std::uintptr_t>(_ReturnAddress());

            // Preserve any side effects and original behaviour for all other callers.
            const F32 originalResult =
                _Engine_ComputeDropChance(context, baseChance, bonusChance);

            if (gGemDrops.overrideChance && returnAddress == gGemDrops.gemChanceReturnAddress)
            {
                return gGemDrops.chance;
            }

            return originalResult;
        }

        // ------------------------------------------------------------------------- //
        LPVOID __fastcall OnItemTypeLookup(LPVOID manager, U32 itemId)
        {
            const auto returnAddress =
                reinterpret_cast<std::uintptr_t>(_ReturnAddress());

            if (gGemDrops.overrideTable
                && returnAddress == gGemDrops.gemItemLookupReturnAddress
                && IsOriginalGemItemId(itemId))
            {
                itemId = SelectGemItemId();
            }

            return _Engine_ItemTypeLookup(manager, itemId);
        }

        // ------------------------------------------------------------------------- //
        bool ConfigureGemDrops(const tinyxml2::XMLElement* root)
        {
            gGemDrops = {};

            if (root == nullptr)
            {
                Lifx::ShowErrorMessage(
                    "Can't configure gem drops: XML root element is null.");
                return false;
            }

            const tinyxml2::XMLElement* section =
                root->FirstChildElement("gemDrops");

            // Missing section intentionally keeps the original game behaviour.
            if (section == nullptr)
                return true;

            gGemDrops.enabled = section->BoolAttribute("enabled", false);
            if (!gGemDrops.enabled)
                return true;

            // Explicit percentage is easier to read in XML than normalized floats.
            // Example: chancePercent="5" means 5%, internally stored as 0.05.
            F32 chancePercent = 0.0f;
            const tinyxml2::XMLError chanceResult =
                section->QueryFloatAttribute("chancePercent", &chancePercent);

            if (chanceResult == tinyxml2::XML_SUCCESS)
            {
                if (!std::isfinite(chancePercent)
                    || chancePercent < 0.0f
                    || chancePercent > 100.0f)
                {
                    Lifx::ShowErrorMessage(
                        "Invalid gemDrops chancePercent. Expected a number from 0 to 100.");
                    gGemDrops = {};
                    return false;
                }

                gGemDrops.overrideChance = true;
                gGemDrops.chance = chancePercent / 100.0f;
            }
            else if (chanceResult != tinyxml2::XML_NO_ATTRIBUTE)
            {
                Lifx::ShowErrorMessage(
                    "Invalid gemDrops chancePercent attribute.");
                gGemDrops = {};
                return false;
            }

            std::uint64_t cumulativeWeight = 0;
            for (const tinyxml2::XMLElement* item = section->FirstChildElement("item");
                 item != nullptr;
                 item = item->NextSiblingElement("item"))
            {
                unsigned itemId = 0;
                unsigned weight = 0;

                if (item->QueryUnsignedAttribute("id", &itemId) != tinyxml2::XML_SUCCESS
                    || itemId == 0)
                {
                    Lifx::ShowErrorMessage(
                        "Invalid <gemDrops><item>: id must be a positive integer.");
                    gGemDrops = {};
                    return false;
                }

                if (item->QueryUnsignedAttribute("weight", &weight) != tinyxml2::XML_SUCCESS
                    || weight == 0)
                {
                    Lifx::ShowErrorMessage(
                        "Invalid <gemDrops><item>: weight must be a positive integer.");
                    gGemDrops = {};
                    return false;
                }

                if (cumulativeWeight
                    > (std::numeric_limits<std::uint64_t>::max)() - weight)
                {
                    Lifx::ShowErrorMessage(
                        "Gem drop weight total is too large.");
                    gGemDrops = {};
                    return false;
                }

                cumulativeWeight += weight;
                gGemDrops.items.push_back({
                    static_cast<U32>(itemId),
                    cumulativeWeight
                });
            }

            if (!gGemDrops.items.empty())
            {
                gGemDrops.overrideTable = true;
                gGemDrops.totalWeight = cumulativeWeight;
            }

            if (!gGemDrops.overrideChance && !gGemDrops.overrideTable)
            {
                Lifx::ShowErrorMessage(
                    "gemDrops is enabled, but neither chancePercent nor item entries were provided.");
                gGemDrops = {};
                return false;
            }

            return true;
        }

        // ------------------------------------------------------------------------- //
        void AttachGemDropHooks()
        {
            if (!gGemDrops.enabled)
                return;

            gGemDrops.moduleBase =
                reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));

            if (gGemDrops.moduleBase == 0)
            {
                Lifx::ShowErrorMessage(
                    "Can't attach gem drop hooks: main module base is null.");
                return;
            }

            if (gGemDrops.overrideChance && !gGemDrops.chanceHookAttached)
            {
                const std::uintptr_t callAddress =
                    gGemDrops.moduleBase + kGemChanceCallRva;

                if (!MatchesGemChanceCallSite(callAddress))
                {
                    Lifx::ShowErrorMessage(
                        "Can't attach gem chance hook: expected code was not found at +0x3A9258.");
                }
                else
                {
                    gGemDrops.gemChanceReturnAddress =
                        gGemDrops.moduleBase + kGemChanceReturnRva;

                    _Engine_ComputeDropChance =
                        reinterpret_cast<_Engine_ComputeDropChance_Fn>(
                            DecodeRelativeCallTarget(callAddress));

                    const LONG result = DetourAttach(
                        &(PVOID&)_Engine_ComputeDropChance,
                        OnComputeDropChance);

                    if (result == NO_ERROR)
                    {
                        gGemDrops.chanceHookAttached = true;
                    }
                    else
                    {
                        Lifx::ShowErrorMessage(
                            "DetourAttach failed for gem chance hook. Error: %ld",
                            result);
                    }
                }
            }

            if (gGemDrops.overrideTable && !gGemDrops.itemHookAttached)
            {
                const std::uintptr_t callAddress =
                    gGemDrops.moduleBase + kGemItemLookupCallRva;

                if (!MatchesGemItemLookupCallSite(callAddress))
                {
                    Lifx::ShowErrorMessage(
                        "Can't attach gem item hook: expected code was not found at +0x3A931F.");
                }
                else
                {
                    gGemDrops.gemItemLookupReturnAddress =
                        gGemDrops.moduleBase + kGemItemLookupReturnRva;

                    _Engine_ItemTypeLookup =
                        reinterpret_cast<_Engine_ItemTypeLookup_Fn>(
                            DecodeRelativeCallTarget(callAddress));

                    const LONG result = DetourAttach(
                        &(PVOID&)_Engine_ItemTypeLookup,
                        OnItemTypeLookup);

                    if (result == NO_ERROR)
                    {
                        gGemDrops.itemHookAttached = true;
                    }
                    else
                    {
                        Lifx::ShowErrorMessage(
                            "DetourAttach failed for gem item hook. Error: %ld",
                            result);
                    }
                }
            }
        }

        // ------------------------------------------------------------------------- //
        void DetachGemDropHooks()
        {
            // Reverse order of attachment.
            if (gGemDrops.itemHookAttached && _Engine_ItemTypeLookup != nullptr)
            {
                DetourDetach(
                    &(PVOID&)_Engine_ItemTypeLookup,
                    OnItemTypeLookup);

                gGemDrops.itemHookAttached = false;
            }

            if (gGemDrops.chanceHookAttached && _Engine_ComputeDropChance != nullptr)
            {
                DetourDetach(
                    &(PVOID&)_Engine_ComputeDropChance,
                    OnComputeDropChance);

                gGemDrops.chanceHookAttached = false;
            }
        }
    }
}
