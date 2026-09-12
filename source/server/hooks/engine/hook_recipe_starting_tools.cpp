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
    StartingToolsID-aware recipe selection for processing devices.

    AbilityBaseWC::_onDoPerform asks CmRecipesManager for the first recipe matching
    the ability skill and target material. The stock lookup does not compare the
    recipe's StartingToolsID, so a custom high-capacity device receives its
    vanilla counterpart's recipe. For configured device ObjectTypeIDs this
    detour repeats the lookup with that one additional comparison and returns
    the game's normal shared recipe.
*  =================================================================================== */

#include "hook_recipe_starting_tools.h"

#include "core/tinyxml2.h"

#include <Windows.h>
#include <detours/detours.h>
#include <intrin.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

__CM_INSTATNTIATE(_Engine_FindRecipeForMaterial);

namespace Hooks
{
    namespace Engine
    {
        namespace
        {
            constexpr std::uintptr_t kFindRecipeForMaterialRva = 0x30BEE0;
            constexpr std::uintptr_t kMaterialMatchesTypeRva = 0x27EB30;
            constexpr std::uintptr_t kAbilityRecipeCallRva = 0x3A1230;
            constexpr std::uintptr_t kAbilityRecipeReturnRva = 0x3A1235;

            constexpr std::uintptr_t kComplexObjectDataGetterRva = 0x0C9930;
            constexpr std::uintptr_t kObjectTypeIdGetterRva = 0x0D3790;

            constexpr std::size_t kCallerComplexObjectOffset = 0x60;
            constexpr std::size_t kRecipeListOffset = 0x10;
            constexpr std::size_t kRecipeCountOffset = 0x18;
            constexpr std::size_t kRecipeNodeObjectOffset = 0x18;
            constexpr std::size_t kRecipeNodeControlOffset = 0x20;

            constexpr std::size_t kRecipeIdOffset = 0x00;
            constexpr std::size_t kRecipeStartingToolsIdOffset = 0x04;
            constexpr std::size_t kRecipeSkillTypeIdOffset = 0x08;
            constexpr std::size_t kRecipeRequirementsBeginOffset = 0x20;
            constexpr std::size_t kRecipeRequirementsEndOffset = 0x28;

            constexpr std::size_t kRequirementStride = 0x10;
            constexpr std::size_t kRequirementMaterialIdOffset = 0x04;
            constexpr std::size_t kRequirementSkipFlagOneOffset = 0x12;
            constexpr std::size_t kRequirementSkipFlagTwoOffset = 0x13;

            constexpr std::size_t kSharedRecipeObjectOffset = 0x00;
            constexpr std::size_t kSharedRecipeControlOffset = 0x08;
            constexpr std::size_t kSharedControlStrongCountOffset = 0x08;

            using MaterialMatchesTypeFn =
                bool (__fastcall*)(LPVOID materialType, U32 requiredTypeId, U32 depth);
            using GetComplexObjectDataFn = LPVOID (__fastcall*)(LPVOID complexObject);
            using GetObjectTypeIdFn = U32 (__fastcall*)(LPVOID objectTypeData);

            struct TanningTubState
            {
                bool enabled = false;
                bool attached = false;
                std::vector<U32> objectTypeIds;

                std::uintptr_t moduleBase = 0;
                std::uintptr_t recipeReturnAddress = 0;
                MaterialMatchesTypeFn materialMatchesType = nullptr;
                GetComplexObjectDataFn getComplexObjectData = nullptr;
                GetObjectTypeIdFn getObjectTypeId = nullptr;
            };

            TanningTubState gTanningTubs;

            // ----------------------------------------------------------------- //
            template <typename T>
            T ReadField(LPVOID object, std::size_t offset)
            {
                T value{};
                std::memcpy(
                    &value,
                    static_cast<const std::uint8_t*>(object) + offset,
                    sizeof(value));
                return value;
            }

            // ----------------------------------------------------------------- //
            template <typename T>
            void WriteField(LPVOID object, std::size_t offset, const T& value)
            {
                std::memcpy(
                    static_cast<std::uint8_t*>(object) + offset,
                    &value,
                    sizeof(value));
            }

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
            bool MatchesRecipeSelector(std::uintptr_t address)
            {
                static constexpr std::uint8_t prologue[] = {
                    0x48, 0x8B, 0xC4,
                    0x44, 0x89, 0x40, 0x18,
                    0x48, 0x89, 0x48, 0x08,
                    0x55, 0x56, 0x57,
                    0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
                    0x48, 0x83, 0xEC, 0x40
                };

                // Inside the selector: compare requirement flags, then call the
                // material inheritance/type matcher with depth 100.
                static constexpr std::uint8_t matcherSetup[] = {
                    0x80, 0x78, 0x12, 0x00,
                    0x75, 0x1B,
                    0x80, 0x78, 0x13, 0x00,
                    0x75, 0x15,
                    0x41, 0xB8, 0x64, 0x00, 0x00, 0x00,
                    0x8B, 0x50, 0x04,
                    0x49, 0x8B, 0xCD
                };

                const std::uintptr_t matcherCall = address + 0x92;
                return MatchBytes(address, prologue, sizeof(prologue))
                    && MatchBytes(
                        address + 0x7A,
                        matcherSetup,
                        sizeof(matcherSetup))
                    && *reinterpret_cast<const std::uint8_t*>(matcherCall) == 0xE8
                    && DecodeRelativeCallTarget(matcherCall)
                        == gTanningTubs.moduleBase + kMaterialMatchesTypeRva;
            }

            // ----------------------------------------------------------------- //
            bool MatchesAbilityRecipeCallSite(std::uintptr_t callAddress)
            {
                static constexpr std::uint8_t before[] = {
                    0x4D, 0x8B, 0x4F, 0x18,
                    0x44, 0x8B, 0x00,
                    0x48, 0x8D, 0x54, 0x24, 0x50,
                    0x48, 0x8B, 0xCB
                };

                static constexpr std::uint8_t after[] = {
                    0x90,
                    0x48, 0x83, 0x7C, 0x24, 0x50, 0x00
                };

                return MatchBytes(
                           callAddress - sizeof(before),
                           before,
                           sizeof(before))
                    && *reinterpret_cast<const std::uint8_t*>(callAddress) == 0xE8
                    && MatchBytes(callAddress + 5, after, sizeof(after))
                    && DecodeRelativeCallTarget(callAddress)
                        == gTanningTubs.moduleBase + kFindRecipeForMaterialRva;
            }

            // ----------------------------------------------------------------- //
            bool MatchesObjectGetters()
            {
                static constexpr std::uint8_t complexObjectDataGetter[] = {
                    0x48, 0x8B, 0x81, 0x70, 0x03, 0x00, 0x00,
                    0xC3
                };
                static constexpr std::uint8_t objectTypeIdGetter[] = {
                    0x8B, 0x41, 0x60,
                    0xC3
                };

                return MatchBytes(
                           gTanningTubs.moduleBase
                               + kComplexObjectDataGetterRva,
                           complexObjectDataGetter,
                           sizeof(complexObjectDataGetter))
                    && MatchBytes(
                        gTanningTubs.moduleBase + kObjectTypeIdGetterRva,
                        objectTypeIdGetter,
                        sizeof(objectTypeIdGetter));
            }

            // ----------------------------------------------------------------- //
            bool IsConfiguredTub(U32 objectTypeId)
            {
                return std::find(
                           gTanningTubs.objectTypeIds.begin(),
                           gTanningTubs.objectTypeIds.end(),
                           objectTypeId)
                    != gTanningTubs.objectTypeIds.end();
            }

            // ----------------------------------------------------------------- //
            LPVOID FindMatchingRecipeNode(
                LPVOID recipesManager,
                U32 skillTypeId,
                U32 startingToolsId,
                LPVOID materialObjectType)
            {
                if (recipesManager == nullptr || materialObjectType == nullptr)
                    return nullptr;

                LPVOID sentinel = ReadField<LPVOID>(
                    recipesManager,
                    kRecipeListOffset);
                if (sentinel == nullptr)
                    return nullptr;

                const std::size_t recipeCount = ReadField<std::size_t>(
                    recipesManager,
                    kRecipeCountOffset);
                if (recipeCount == 0
                    || recipeCount
                        > static_cast<std::size_t>((std::numeric_limits<U32>::max)()))
                {
                    return nullptr;
                }

                LPVOID node = ReadField<LPVOID>(sentinel, 0);
                for (std::size_t index = 0;
                     node != nullptr && node != sentinel && index < recipeCount;
                     ++index)
                {
                    LPVOID recipe = ReadField<LPVOID>(
                        node,
                        kRecipeNodeObjectOffset);

                    if (recipe != nullptr
                        && ReadField<U32>(
                               recipe,
                               kRecipeStartingToolsIdOffset)
                            == startingToolsId
                        && ReadField<U32>(
                               recipe,
                               kRecipeSkillTypeIdOffset)
                            == skillTypeId)
                    {
                        const auto requirementsBegin =
                            ReadField<std::uintptr_t>(
                                recipe,
                                kRecipeRequirementsBeginOffset);
                        const auto requirementsEnd =
                            ReadField<std::uintptr_t>(
                                recipe,
                                kRecipeRequirementsEndOffset);

                        if (requirementsBegin <= requirementsEnd
                            && (requirementsEnd - requirementsBegin)
                                % kRequirementStride == 0)
                        {
                            for (std::uintptr_t entry = requirementsBegin;
                                 entry < requirementsEnd;
                                 entry += kRequirementStride)
                            {
                                LPVOID requirement =
                                    ReadField<LPVOID>(
                                        reinterpret_cast<LPVOID>(entry),
                                        0);
                                if (requirement == nullptr
                                    || ReadField<std::uint8_t>(
                                           requirement,
                                           kRequirementSkipFlagOneOffset)
                                        != 0
                                    || ReadField<std::uint8_t>(
                                           requirement,
                                           kRequirementSkipFlagTwoOffset)
                                        != 0)
                                {
                                    continue;
                                }

                                const U32 requiredMaterialId =
                                    ReadField<U32>(
                                        requirement,
                                        kRequirementMaterialIdOffset);
                                if (gTanningTubs.materialMatchesType(
                                        materialObjectType,
                                        requiredMaterialId,
                                        100))
                                {
                                    return node;
                                }
                            }
                        }
                    }

                    node = ReadField<LPVOID>(node, 0);
                }

                return nullptr;
            }

            // ----------------------------------------------------------------- //
            LPVOID MakeSharedRecipeFromNode(
                LPVOID sharedRecipeOut,
                LPVOID recipeNode)
            {
                if (sharedRecipeOut == nullptr || recipeNode == nullptr)
                    return sharedRecipeOut;

                LPVOID recipe = ReadField<LPVOID>(
                    recipeNode,
                    kRecipeNodeObjectOffset);
                LPVOID control = ReadField<LPVOID>(
                    recipeNode,
                    kRecipeNodeControlOffset);

                if (control != nullptr)
                {
                    InterlockedIncrement(reinterpret_cast<volatile LONG*>(
                        static_cast<std::uint8_t*>(control)
                        + kSharedControlStrongCountOffset));
                }

                WriteField<LPVOID>(
                    sharedRecipeOut,
                    kSharedRecipeObjectOffset,
                    recipe);
                WriteField<LPVOID>(
                    sharedRecipeOut,
                    kSharedRecipeControlOffset,
                    control);
                return sharedRecipeOut;
            }
        }

        // --------------------------------------------------------------------- //
        LPVOID __fastcall OnFindRecipeForMaterial(
            LPVOID recipesManager,
            LPVOID sharedRecipeOut,
            U32 skillTypeId,
            LPVOID materialObjectType)
        {
            const auto returnAddress =
                reinterpret_cast<std::uintptr_t>(_ReturnAddress());

            if (returnAddress == gTanningTubs.recipeReturnAddress)
            {
                const auto callerStack = reinterpret_cast<std::uintptr_t>(
                    _AddressOfReturnAddress()) + sizeof(void*);
                LPVOID complexObject = ReadField<LPVOID>(
                    reinterpret_cast<LPVOID>(callerStack),
                    kCallerComplexObjectOffset);

                if (complexObject != nullptr)
                {
                    LPVOID objectTypeData =
                        gTanningTubs.getComplexObjectData(complexObject);
                    if (objectTypeData != nullptr)
                    {
                        const U32 objectTypeId =
                            gTanningTubs.getObjectTypeId(objectTypeData);
                        if (IsConfiguredTub(objectTypeId))
                        {
                            LPVOID recipeNode = FindMatchingRecipeNode(
                                recipesManager,
                                skillTypeId,
                                objectTypeId,
                                materialObjectType);
                            if (recipeNode != nullptr)
                            {
                                return MakeSharedRecipeFromNode(
                                    sharedRecipeOut,
                                    recipeNode);
                            }
                        }
                    }
                }
            }

            return _Engine_FindRecipeForMaterial(
                recipesManager,
                sharedRecipeOut,
                skillTypeId,
                materialObjectType);
        }

        // --------------------------------------------------------------------- //
        bool ConfigureRecipeStartingTools(const tinyxml2::XMLElement* root)
        {
            if (gTanningTubs.attached)
            {
                Lifx::ShowErrorMessage(
                    "Can't configure recipe starting tools while the hook is attached.");
                return false;
            }

            gTanningTubs = {};

            if (root == nullptr)
            {
                Lifx::ShowErrorMessage(
                    "Can't configure recipe starting tools: XML root element is null.");
                return false;
            }

            const tinyxml2::XMLElement* section =
                root->FirstChildElement("recipeStartingTools");
            if (section == nullptr)
                return true;

            gTanningTubs.enabled = section->BoolAttribute("enabled", false);
            if (!gTanningTubs.enabled)
                return true;

            for (const tinyxml2::XMLElement* tool =
                     section->FirstChildElement("tool");
                 tool != nullptr;
                 tool = tool->NextSiblingElement("tool"))
            {
                unsigned objectTypeId = 0;
                if (tool->QueryUnsignedAttribute(
                        "objectTypeId",
                        &objectTypeId) != tinyxml2::XML_SUCCESS
                    || objectTypeId == 0)
                {
                    Lifx::ShowErrorMessage(
                        "Invalid <recipeStartingTools><tool>: objectTypeId must be a positive integer.");
                    gTanningTubs = {};
                    return false;
                }

                const bool duplicate = std::find(
                                           gTanningTubs.objectTypeIds.begin(),
                                           gTanningTubs.objectTypeIds.end(),
                                           static_cast<U32>(objectTypeId))
                    != gTanningTubs.objectTypeIds.end();
                if (duplicate)
                {
                    Lifx::ShowErrorMessage(
                        "Duplicate <recipeStartingTools><tool> objectTypeId %u.",
                        objectTypeId);
                    gTanningTubs = {};
                    return false;
                }

                gTanningTubs.objectTypeIds.push_back(
                    static_cast<U32>(objectTypeId));
            }

            if (gTanningTubs.objectTypeIds.empty())
            {
                Lifx::ShowErrorMessage(
                    "recipeStartingTools is enabled, but no <tool> entries were provided.");
                gTanningTubs = {};
                return false;
            }

            return true;
        }

        // --------------------------------------------------------------------- //
        void AttachRecipeStartingToolsHook()
        {
            if (!gTanningTubs.enabled || gTanningTubs.attached)
                return;

            gTanningTubs.moduleBase =
                reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
            if (gTanningTubs.moduleBase == 0)
            {
                Lifx::ShowErrorMessage(
                    "Can't attach recipe-starting-tools hook: main module base is null.");
                return;
            }

            const std::uintptr_t selectorAddress =
                gTanningTubs.moduleBase + kFindRecipeForMaterialRva;
            const std::uintptr_t callAddress =
                gTanningTubs.moduleBase + kAbilityRecipeCallRva;

            if (!MatchesRecipeSelector(selectorAddress)
                || !MatchesAbilityRecipeCallSite(callAddress)
                || !MatchesObjectGetters())
            {
                Lifx::ShowErrorMessage(
                    "Can't attach recipe-starting-tools hook: expected recipe-selection code was not found.");
                return;
            }

            gTanningTubs.recipeReturnAddress =
                gTanningTubs.moduleBase + kAbilityRecipeReturnRva;
            gTanningTubs.materialMatchesType =
                reinterpret_cast<MaterialMatchesTypeFn>(
                    gTanningTubs.moduleBase + kMaterialMatchesTypeRva);
            gTanningTubs.getComplexObjectData =
                reinterpret_cast<GetComplexObjectDataFn>(
                    gTanningTubs.moduleBase + kComplexObjectDataGetterRva);
            gTanningTubs.getObjectTypeId =
                reinterpret_cast<GetObjectTypeIdFn>(
                    gTanningTubs.moduleBase + kObjectTypeIdGetterRva);
            _Engine_FindRecipeForMaterial =
                reinterpret_cast<_Engine_FindRecipeForMaterial_Fn>(
                    selectorAddress);

            const LONG result = DetourAttach(
                &(PVOID&)_Engine_FindRecipeForMaterial,
                OnFindRecipeForMaterial);
            if (result != NO_ERROR)
            {
                Lifx::ShowErrorMessage(
                    "DetourAttach failed for recipe-starting-tools hook. Error: %ld",
                    result);
                gTanningTubs.materialMatchesType = nullptr;
                gTanningTubs.getComplexObjectData = nullptr;
                gTanningTubs.getObjectTypeId = nullptr;
                return;
            }

            gTanningTubs.attached = true;
        }

        // --------------------------------------------------------------------- //
        void DetachRecipeStartingToolsHook()
        {
            if (!gTanningTubs.attached
                || _Engine_FindRecipeForMaterial == nullptr)
            {
                return;
            }

            const LONG result = DetourDetach(
                &(PVOID&)_Engine_FindRecipeForMaterial,
                OnFindRecipeForMaterial);
            if (result != NO_ERROR)
            {
                Lifx::ShowErrorMessage(
                    "DetourDetach failed for recipe-starting-tools hook. Error: %ld",
                    result);
                return;
            }

            gTanningTubs.attached = false;
            gTanningTubs.materialMatchesType = nullptr;
            gTanningTubs.getComplexObjectData = nullptr;
            gTanningTubs.getObjectTypeId = nullptr;
        }
    }
}
