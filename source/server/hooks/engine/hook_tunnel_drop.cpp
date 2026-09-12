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
    Extra drops awarded only after a successful partial tunnel-dig operation.

    The vanilla material reward remains untouched. After it succeeds, this hook
    rolls every configured item independently and uses the game's generic
    inventory-item helper for each successful roll.
*  =================================================================================== */

#include "hook_tunnel_drop.h"

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
#include <vector>

__CM_INSTATNTIATE(_Engine_AddTunnelMaterialsToInventory);

namespace Hooks
{
    namespace Engine
    {
        namespace
        {
            // All three DigATunnel variants reach this call in the common
            // +0x3ADA30 implementation.
            constexpr std::uintptr_t kTunnelMaterialsCallRva = 0x3ADEE9;
            constexpr std::uintptr_t kTunnelMaterialsReturnRva = 0x3ADEEE;
            constexpr std::uintptr_t kAddMaterialsFunctionRva = 0x584120;

            // MiningBaseImp helper which accepts an arbitrary ObjectTypeID.
            constexpr std::uintptr_t kAddArbitraryItemRva = 0x3A89E0;

            constexpr std::size_t kMaterialEntryStride = 0x0A;
            constexpr std::size_t kMaterialQualityOffset = 0x06;
            constexpr std::size_t kMaterialQuantityOffset = 0x08;
            constexpr U16 kFallbackQuality = 100;

            using AddArbitraryItemFn = bool (__fastcall*)(
                LPVOID context,
                U32 playerId,
                U32 itemId,
                U16 quality,
                U32 quantity);

            struct TunnelDropItem
            {
                U32 itemId = 0;
                U32 minimumQuantity = 1;
                U32 maximumQuantity = 1;
                F32 chance = 0.0f;
            };

            struct TunnelDropState
            {
                bool enabled = false;
                bool attached = false;
                bool overrideQuality = false;

                U16 quality = kFallbackQuality;
                std::vector<TunnelDropItem> items;

                std::uintptr_t moduleBase = 0;
                std::uintptr_t tunnelMaterialsReturnAddress = 0;
                AddArbitraryItemFn addArbitraryItem = nullptr;
            };

            TunnelDropState gTunnelDrops;

            // ----------------------------------------------------------------- //
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

            // ----------------------------------------------------------------- //
            std::uintptr_t DecodeRelativeCallTarget(std::uintptr_t callAddress)
            {
                std::int32_t displacement = 0;
                std::memcpy(
                    &displacement,
                    reinterpret_cast<const void*>(callAddress + 1),
                    sizeof(displacement));

                return callAddress + 5 + static_cast<std::intptr_t>(displacement);
            }

            // ----------------------------------------------------------------- //
            bool MatchesTunnelMaterialsCallSite(std::uintptr_t callAddress)
            {
                // +3ADEDE movzx r8d,byte ptr [r13+38]
                // +3ADEE3 mov   rdx,qword ptr [r13+40]
                // +3ADEE7 mov   ecx,esi
                static constexpr std::uint8_t before[] = {
                    0x45, 0x0F, 0xB6, 0x45, 0x38,
                    0x49, 0x8B, 0x55, 0x40,
                    0x8B, 0xCE
                };

                // +3ADEEE test al,al
                // +3ADEF0 jne +7A
                static constexpr std::uint8_t after[] = {
                    0x84, 0xC0,
                    0x75, 0x7A
                };

                return MatchBytes(
                           callAddress - sizeof(before),
                           before,
                           sizeof(before))
                    && *reinterpret_cast<const std::uint8_t*>(callAddress) == 0xE8
                    && MatchBytes(callAddress + 5, after, sizeof(after))
                    && DecodeRelativeCallTarget(callAddress)
                        == gTunnelDrops.moduleBase + kAddMaterialsFunctionRva;
            }

            // ----------------------------------------------------------------- //
            bool MatchesAddMaterialsFunction(std::uintptr_t address)
            {
                // Save the six incoming arguments, then enter the large function.
                static constexpr std::uint8_t expected[] = {
                    0x48, 0x8B, 0xC4,
                    0x44, 0x88, 0x40, 0x18,
                    0x48, 0x89, 0x50, 0x10,
                    0x89, 0x48, 0x08,
                    0x55, 0x53, 0x56, 0x57,
                    0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57
                };

                return MatchBytes(address, expected, sizeof(expected));
            }

            // ----------------------------------------------------------------- //
            bool MatchesAddArbitraryItemFunction(std::uintptr_t address)
            {
                static constexpr std::uint8_t expected[] = {
                    0x48, 0x8B, 0xC4,
                    0x55, 0x41, 0x56, 0x41, 0x57,
                    0x48, 0x8D, 0x68, 0xA9,
                    0x48, 0x81, 0xEC, 0xE0, 0x00, 0x00, 0x00
                };

                return MatchBytes(address, expected, sizeof(expected));
            }

            // ----------------------------------------------------------------- //
            std::uint64_t MakeSeed()
            {
                std::random_device rd;
                const auto now = static_cast<std::uint64_t>(
                    std::chrono::high_resolution_clock::now()
                        .time_since_epoch()
                        .count());
                const auto tid = static_cast<std::uint64_t>(GetCurrentThreadId());
                const auto address =
                    reinterpret_cast<std::uintptr_t>(&gTunnelDrops);

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

            // ----------------------------------------------------------------- //
            std::mt19937_64& ThreadRng()
            {
                thread_local std::mt19937_64 rng(MakeSeed());
                return rng;
            }

            // ----------------------------------------------------------------- //
            bool PassesChanceRoll(F32 chance)
            {
                if (chance <= 0.0f)
                    return false;
                if (chance >= 1.0f)
                    return true;

                return std::generate_canonical<F64, 53>(ThreadRng())
                    < static_cast<F64>(chance);
            }

            // ----------------------------------------------------------------- //
            U32 SelectQuantity(const TunnelDropItem& item)
            {
                if (item.minimumQuantity == item.maximumQuantity)
                    return item.minimumQuantity;

                std::uniform_int_distribution<U32> distribution(
                    item.minimumQuantity,
                    item.maximumQuantity);
                return distribution(ThreadRng());
            }

            // ----------------------------------------------------------------- //
            U16 ExtractMaterialQuality(LPVOID materials)
            {
                if (materials == nullptr)
                    return kFallbackQuality;

                const std::uint8_t* begin = nullptr;
                const std::uint8_t* end = nullptr;
                std::memcpy(&begin, materials, sizeof(begin));
                std::memcpy(
                    &end,
                    reinterpret_cast<const std::uint8_t*>(materials)
                        + sizeof(begin),
                    sizeof(end));

                const std::uintptr_t beginAddress =
                    reinterpret_cast<std::uintptr_t>(begin);
                const std::uintptr_t endAddress =
                    reinterpret_cast<std::uintptr_t>(end);
                if (beginAddress == 0
                    || endAddress == 0
                    || endAddress < beginAddress)
                {
                    return kFallbackQuality;
                }

                const std::size_t bytes =
                    static_cast<std::size_t>(endAddress - beginAddress);
                if (bytes == 0
                    || bytes > 1024 * 1024
                    || bytes % kMaterialEntryStride != 0)
                {
                    return kFallbackQuality;
                }

                std::uint64_t weightedQuality = 0;
                std::uint64_t totalQuantity = 0;

                for (std::size_t offset = 0;
                     offset < bytes;
                     offset += kMaterialEntryStride)
                {
                    const std::uint8_t* entry = begin + offset;
                    U16 quantity = 0;
                    std::memcpy(
                        &quantity,
                        entry + kMaterialQuantityOffset,
                        sizeof(quantity));

                    const U16 quality = entry[kMaterialQualityOffset];
                    weightedQuality += static_cast<std::uint64_t>(quality)
                        * quantity;
                    totalQuantity += quantity;
                }

                if (totalQuantity == 0)
                    return kFallbackQuality;

                const std::uint64_t rounded =
                    (weightedQuality + totalQuantity / 2) / totalQuantity;
                return static_cast<U16>(
                    (std::clamp)(rounded, std::uint64_t{1}, std::uint64_t{100}));
            }

            // ----------------------------------------------------------------- //
            void AwardTunnelDrop(U32 playerId, LPVOID materials)
            {
                const U16 quality = gTunnelDrops.overrideQuality
                    ? gTunnelDrops.quality
                    : ExtractMaterialQuality(materials);

                for (const TunnelDropItem& item : gTunnelDrops.items)
                {
                    // Every configured item has an independent probability roll.
                    // Consequently one tunnelling action may award no configured
                    // item, one item, or several different items.
                    if (!PassesChanceRoll(item.chance))
                        continue;

                    const U32 quantity = SelectQuantity(item);

                    // Static analysis and the first live test confirm that the
                    // helper does not read its first parameter.
                    gTunnelDrops.addArbitraryItem(
                        nullptr,
                        playerId,
                        item.itemId,
                        quality,
                        quantity);
                }
            }
        }

        // --------------------------------------------------------------------- //
        bool __fastcall OnAddTunnelMaterialsToInventory(
            U32 playerId,
            LPVOID inventoryContext,
            U8 selectedMaterial,
            LPVOID materials,
            F32 qualityFactor,
            U32 containerId)
        {
            const auto returnAddress =
                reinterpret_cast<std::uintptr_t>(_ReturnAddress());

            const bool originalResult =
                _Engine_AddTunnelMaterialsToInventory(
                    playerId,
                    inventoryContext,
                    selectedMaterial,
                    materials,
                    qualityFactor,
                    containerId);

            if (originalResult
                && returnAddress == gTunnelDrops.tunnelMaterialsReturnAddress)
            {
                AwardTunnelDrop(playerId, materials);
            }

            return originalResult;
        }

        // --------------------------------------------------------------------- //
        bool ConfigureTunnelDrops(const tinyxml2::XMLElement* root)
        {
            if (gTunnelDrops.attached)
            {
                Lifx::ShowErrorMessage(
                    "Can't configure tunnel drops while the hook is attached.");
                return false;
            }

            gTunnelDrops = {};

            if (root == nullptr)
            {
                Lifx::ShowErrorMessage(
                    "Can't configure tunnel drops: XML root element is null.");
                return false;
            }

            const tinyxml2::XMLElement* section =
                root->FirstChildElement("tunnelDrops");
            if (section == nullptr)
                return true;

            gTunnelDrops.enabled = section->BoolAttribute("enabled", false);
            if (!gTunnelDrops.enabled)
                return true;

            unsigned configuredQuality = 0;
            const tinyxml2::XMLError qualityResult =
                section->QueryUnsignedAttribute(
                    "quality",
                    &configuredQuality);
            if (qualityResult == tinyxml2::XML_SUCCESS)
            {
                if (configuredQuality < 1 || configuredQuality > 100)
                {
                    Lifx::ShowErrorMessage(
                        "Invalid tunnelDrops quality. Expected an integer from 1 to 100.");
                    gTunnelDrops = {};
                    return false;
                }

                gTunnelDrops.overrideQuality = true;
                gTunnelDrops.quality = static_cast<U16>(configuredQuality);
            }
            else if (qualityResult != tinyxml2::XML_NO_ATTRIBUTE)
            {
                Lifx::ShowErrorMessage(
                    "Invalid tunnelDrops quality attribute.");
                gTunnelDrops = {};
                return false;
            }

            for (const tinyxml2::XMLElement* item =
                     section->FirstChildElement("item");
                 item != nullptr;
                item = item->NextSiblingElement("item"))
            {
                unsigned itemId = 0;
                unsigned minimumQuantity = 1;
                unsigned maximumQuantity = 1;
                F32 chancePercent = 0.0f;

                if (item->QueryUnsignedAttribute("id", &itemId)
                        != tinyxml2::XML_SUCCESS
                    || itemId == 0)
                {
                    Lifx::ShowErrorMessage(
                        "Invalid <tunnelDrops><item>: id must be a positive integer.");
                    gTunnelDrops = {};
                    return false;
                }

                if (item->QueryFloatAttribute(
                        "chancePercent",
                        &chancePercent) != tinyxml2::XML_SUCCESS
                    || !std::isfinite(chancePercent)
                    || chancePercent < 0.0f
                    || chancePercent > 100.0f)
                {
                    Lifx::ShowErrorMessage(
                        "Invalid <tunnelDrops><item> chancePercent. Expected a number from 0 to 100.");
                    gTunnelDrops = {};
                    return false;
                }

                const tinyxml2::XMLError minimumResult =
                    item->QueryUnsignedAttribute(
                        "minQuantity",
                        &minimumQuantity);
                if (minimumResult != tinyxml2::XML_SUCCESS
                    && minimumResult != tinyxml2::XML_NO_ATTRIBUTE)
                {
                    Lifx::ShowErrorMessage(
                        "Invalid <tunnelDrops><item> minQuantity attribute.");
                    gTunnelDrops = {};
                    return false;
                }

                const tinyxml2::XMLError maximumResult =
                    item->QueryUnsignedAttribute(
                        "maxQuantity",
                        &maximumQuantity);
                if (maximumResult == tinyxml2::XML_NO_ATTRIBUTE)
                {
                    maximumQuantity = minimumQuantity;
                }
                else if (maximumResult != tinyxml2::XML_SUCCESS)
                {
                    Lifx::ShowErrorMessage(
                        "Invalid <tunnelDrops><item> maxQuantity attribute.");
                    gTunnelDrops = {};
                    return false;
                }

                if (minimumQuantity == 0
                    || maximumQuantity < minimumQuantity)
                {
                    Lifx::ShowErrorMessage(
                        "Invalid <tunnelDrops><item> quantity range.");
                    gTunnelDrops = {};
                    return false;
                }

                const bool duplicate = std::any_of(
                    gTunnelDrops.items.begin(),
                    gTunnelDrops.items.end(),
                    [itemId](const TunnelDropItem& existing)
                    {
                        return existing.itemId == itemId;
                    });
                if (duplicate)
                {
                    Lifx::ShowErrorMessage(
                        "Duplicate <tunnelDrops><item> id %u.",
                        itemId);
                    gTunnelDrops = {};
                    return false;
                }

                gTunnelDrops.items.push_back({
                    static_cast<U32>(itemId),
                    static_cast<U32>(minimumQuantity),
                    static_cast<U32>(maximumQuantity),
                    chancePercent / 100.0f
                });
            }

            if (gTunnelDrops.items.empty())
            {
                Lifx::ShowErrorMessage(
                    "tunnelDrops is enabled, but no <item> entries were provided.");
                gTunnelDrops = {};
                return false;
            }

            return true;
        }

        // --------------------------------------------------------------------- //
        void AttachTunnelDropHook()
        {
            if (!gTunnelDrops.enabled || gTunnelDrops.attached)
                return;

            gTunnelDrops.moduleBase =
                reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
            if (gTunnelDrops.moduleBase == 0)
            {
                Lifx::ShowErrorMessage(
                    "Can't attach tunnel drop hook: main module base is null.");
                return;
            }

            const std::uintptr_t callAddress =
                gTunnelDrops.moduleBase + kTunnelMaterialsCallRva;
            const std::uintptr_t functionAddress =
                gTunnelDrops.moduleBase + kAddMaterialsFunctionRva;
            const std::uintptr_t helperAddress =
                gTunnelDrops.moduleBase + kAddArbitraryItemRva;

            if (!MatchesTunnelMaterialsCallSite(callAddress)
                || !MatchesAddMaterialsFunction(functionAddress))
            {
                Lifx::ShowErrorMessage(
                    "Can't attach tunnel drop hook: expected tunnel material code was not found.");
                return;
            }

            if (!MatchesAddArbitraryItemFunction(helperAddress))
            {
                Lifx::ShowErrorMessage(
                    "Can't attach tunnel drop hook: expected arbitrary-item helper was not found.");
                return;
            }

            gTunnelDrops.tunnelMaterialsReturnAddress =
                gTunnelDrops.moduleBase + kTunnelMaterialsReturnRva;
            gTunnelDrops.addArbitraryItem =
                reinterpret_cast<AddArbitraryItemFn>(helperAddress);
            _Engine_AddTunnelMaterialsToInventory =
                reinterpret_cast<_Engine_AddTunnelMaterialsToInventory_Fn>(
                    functionAddress);

            const LONG result = DetourAttach(
                &(PVOID&)_Engine_AddTunnelMaterialsToInventory,
                OnAddTunnelMaterialsToInventory);
            if (result != NO_ERROR)
            {
                Lifx::ShowErrorMessage(
                    "DetourAttach failed for tunnel drop hook. Error: %ld",
                    result);
                gTunnelDrops.addArbitraryItem = nullptr;
                return;
            }

            gTunnelDrops.attached = true;
        }

        // --------------------------------------------------------------------- //
        void DetachTunnelDropHook()
        {
            if (!gTunnelDrops.attached
                || _Engine_AddTunnelMaterialsToInventory == nullptr)
            {
                return;
            }

            const LONG result = DetourDetach(
                &(PVOID&)_Engine_AddTunnelMaterialsToInventory,
                OnAddTunnelMaterialsToInventory);
            if (result != NO_ERROR)
            {
                Lifx::ShowErrorMessage(
                    "DetourDetach failed for tunnel drop hook. Error: %ld",
                    result);
                return;
            }

            gTunnelDrops.attached = false;
            gTunnelDrops.addArbitraryItem = nullptr;
        }
    }
}
