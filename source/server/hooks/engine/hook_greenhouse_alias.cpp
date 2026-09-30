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

/* ===================================================================================
    Greenhouse alias. See hook_greenhouse_alias.h for the design.

    Engine facts (ddctd_cm_yo_server.exe, verified by decompilation + disassembly):
      - 0x1D8E20 loadObjects: push r14 / mov eax,1840h / call ...; type id arrives in r9d
        and is compared with 0x549 at 0x1D8E90.
      - 0x1D91D0 use entry: `call 0xC9930` at 0x1D9220 (returns the type record), then
        `call 0xD3790` at 0x1D9228 (mov eax,[rcx+60h]; ret) and `cmp ebx,549h` at 0x1D92AB.
      - 0xC9930 is `mov rax,[rcx+370h]; ret` - 7 bytes of code, enough for a detour.
*  =================================================================================== */

#include "hook_greenhouse_alias.h"

#include "core/tinyxml2.h"
#include "server/api/t3d_console.h"

#include <Windows.h>
#include <detours/detours.h>
#include <intrin.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <vector>

__CM_INSTATNTIATE(_Craftwork_LoadObjects);
__CM_INSTATNTIATE(_Entity_GetTypeInfo);
__CM_INSTATNTIATE(_Wc_DoPerform);
__CM_INSTATNTIATE(_Wc_FindRecipe);
__CM_INSTATNTIATE(_Type_IsDescendant);

namespace Hooks
{
    namespace Engine
    {
        namespace
        {
            constexpr std::uintptr_t kLoadObjectsRva = 0x1D8E20;
            constexpr std::uintptr_t kGetTypeInfoRva = 0xC9930;
            constexpr std::uintptr_t kWcDoPerformRva = 0x3A0F60;
            constexpr std::uintptr_t kIsDescendantRva = 0x27EB30;
            constexpr std::uintptr_t kOpenCheckCallRva = 0x38641D;      // call IsDescendant(type, 0x575, 100) in the open-inventory check
            constexpr std::uintptr_t kOpenCheckReturnRva = 0x386422;
            constexpr std::uintptr_t kWcFindRecipeRva = 0x30BEE0;
            constexpr std::uintptr_t kObjectFromEntityRva = 0x33DDB0;   // (Entity*, shared_ptr<Object>* out)
            constexpr std::uintptr_t kReleaseRefRva = 0x86D60;          // shared_ptr release (control block at +8)
            // Return address of the `call 0xC9930` at 0x1D9220 inside the use entry.
            constexpr std::uintptr_t kUseEntryTypeCallReturnRva = 0x1D9225;

            constexpr std::size_t kTypeIdOffset = 0x60;       // Type::getId reads [rec+0x60]
            constexpr U32 kGreenhouseTypeId = 1353;           // 0x549, the vanilla Herbal Garden

            // A stand-in type record. Type::getId only reads one dword, so a small zeroed
            // block with that dword set is enough for the single call site we intercept.
            // Vanilla types a custom type may be restored as at boot. Only the Herbal Garden also
            // needs the use-entry stand-in record; the Drying Frame / Tanning Tub only need the
            // loadObjects remap.
            constexpr U32 kDryingFrameTypeId = 118;           // 0x76
            constexpr U32 kTanningTubTypeId = 472;            // 0x1D8

            struct Alias
            {
                U32 fromTypeId = 0;
                U32 behavesLike = 0;
                bool finishedAsWorking = false;
                alignas(16) unsigned char fake[0x100] = {};
                long hits = 0;
            };

            // Working-container recipe override: at a building of objectTypeId, when the engine's finder returns recipe id 'from',
            // hand back recipe id 'to' instead (e.g. the Big Tanning Tub's 5-at-once recipes instead of the vanilla one).
            struct RecipeOverride
            {
                U32 objectTypeId = 0;
                int from = 0;
                int to = 0;
            };

            struct State
            {
                std::vector<RecipeOverride> overrides;
                std::vector<U32> warehouseTypes;   // extra object types the open-inventory check treats as warehouses
                bool enabled = false;
                bool attached = false;
                bool verbose = true;
                std::uintptr_t base = 0;
                std::vector<std::unique_ptr<Alias>> aliases;
                std::mutex logMutex;
            };

            // Intentionally leaked: hooks may fire during process shutdown.
            State& S()
            {
                static State* state = new State();
                return *state;
            }

            void FileLog(const char* fmt, ...)
            {
                State& s = S();
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

                std::lock_guard<std::mutex> lock(s.logMutex);
                std::FILE* f = nullptr;
                if (fopen_s(&f, "logs/greenhouse_alias.log", "a") == 0 && f)
                {
                    std::fprintf(f, "%s %s\n", stamp, msg);
                    std::fclose(f);
                }
            }

            // TLS: object type of the building the current working-container ability targets (0 = none / feature off).
            thread_local U32 t_wcObjectType = 0;

            bool PlausiblePtr(const void* p)
            {
                const std::uintptr_t v = reinterpret_cast<std::uintptr_t>(p);
                return v > 0x10000 && v < 0x00007FFFFFFF0000ull;
            }

            // Object type id of the entity the ability targets: entity -> object (engine lookup) -> type record [obj+0x370] -> id [rec+0x60].
            U32 ReadWcObjectType(LPVOID ability)
            {
                __try
                {
                    if (!PlausiblePtr(ability))
                        return 0;
                    LPVOID entity = *reinterpret_cast<LPVOID*>(static_cast<unsigned char*>(ability) + 0x38);
                    if (!PlausiblePtr(entity))
                        return 0;
                    const std::uintptr_t base = S().base;
                    using ObjFromEntityFn = LPVOID*(__fastcall*)(LPVOID, LPVOID*);
                    using ReleaseFn = void(__fastcall*)(LPVOID);
                    LPVOID out[2] = { nullptr, nullptr };
                    reinterpret_cast<ObjFromEntityFn>(base + kObjectFromEntityRva)(entity, out);
                    U32 type = 0;
                    if (PlausiblePtr(out[0]))
                    {
                        LPVOID rec = *reinterpret_cast<LPVOID*>(static_cast<unsigned char*>(out[0]) + 0x370);
                        if (PlausiblePtr(rec))
                            type = *reinterpret_cast<U32*>(static_cast<unsigned char*>(rec) + kTypeIdOffset);
                    }
                    reinterpret_cast<ReleaseFn>(base + kReleaseRefRva)(out);
                    return type;
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                    return 0;
                }
            }

            // Replace the recipe the finder returned (out = shared_ptr<Recipe>) by recipe id `to`, found in the manager's recipe list.
            // Returns true when replaced. Layout (from the finder's decompile): list head/sentinel at *(mgr+0x10), first node = *sentinel,
            // node[0] = next, node[3] = recipe*, node[4] = control block (use count at +8); recipe id = first dword of the recipe.
            bool SwapRecipe(LPVOID mgr, LPVOID* out, int toId)
            {
                __try
                {
                    if (!PlausiblePtr(mgr))
                        return false;
                    LPVOID* sentinel = *reinterpret_cast<LPVOID**>(static_cast<unsigned char*>(mgr) + 0x10);
                    if (!PlausiblePtr(sentinel))
                        return false;
                    LPVOID* node = reinterpret_cast<LPVOID*>(sentinel[0]);
                    for (int guard = 0; node != sentinel && guard < 20000; ++guard)
                    {
                        if (!PlausiblePtr(node))
                            return false;
                        LPVOID recipe = node[3];
                        LPVOID ctrl = node[4];
                        if (PlausiblePtr(recipe) && *reinterpret_cast<int*>(recipe) == toId)
                        {
                            if (PlausiblePtr(ctrl))
                                InterlockedIncrement(reinterpret_cast<volatile LONG*>(static_cast<unsigned char*>(ctrl) + 8));
                            using ReleaseFn = void(__fastcall*)(LPVOID);
                            reinterpret_cast<ReleaseFn>(S().base + kReleaseRefRva)(out);   // drop the finder's own pick
                            out[0] = recipe;
                            out[1] = ctrl;
                            return true;
                        }
                        node = reinterpret_cast<LPVOID*>(node[0]);
                    }
                    return false;
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                    return false;
                }
            }

            int ReadRecipeId(LPVOID* out)
            {
                __try
                {
                    return PlausiblePtr(out) && PlausiblePtr(out[0]) ? *reinterpret_cast<int*>(out[0]) : 0;
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                    return 0;
                }
            }

            Alias* FindAlias(U32 typeId)            {
                for (auto& a : S().aliases)
                    if (a->fromTypeId == typeId)
                        return a.get();
                return nullptr;
            }

            bool MatchBytes(std::uintptr_t addr, const std::uint8_t* bytes, std::size_t n)
            {
                return std::memcmp(reinterpret_cast<const void*>(addr), bytes, n) == 0;
            }
        }

        // --------------------------------------------------------------------- //
        void __fastcall OnCraftworkLoadObjects(
            LPVOID self, U64 objectId, int timeLeftSec, int objectTypeId,
            U32 arg5, U32 arg6)
        {
            State& s = S();
            if (s.enabled && objectTypeId > 0)
            {
                if (Alias* a = FindAlias(static_cast<U32>(objectTypeId)))
                {
                    // A working container whose timer ran out while the server was down would
                    // otherwise come back as Complete (the engine only sets Working for a
                    // positive time left), which strands the finished items: "Pick up" needs
                    // Working, "Dry/Use" needs an empty slot. One second keeps it Working.
                    if (a->finishedAsWorking && timeLeftSec < 1)
                        timeLeftSec = 1;
                    if (s.verbose)
                        FileLog("loadObjects: object %llu of type %d restored as type %u (time left %d s)",
                                static_cast<unsigned long long>(objectId), objectTypeId,
                                static_cast<unsigned>(a->behavesLike), timeLeftSec);
                    objectTypeId = static_cast<int>(a->behavesLike);
                }
            }
            _Craftwork_LoadObjects(self, objectId, timeLeftSec, objectTypeId, arg5, arg6);
        }

        // --------------------------------------------------------------------- //
        LPVOID __fastcall OnEntityGetTypeInfo(LPVOID entity)
        {
            LPVOID rec = _Entity_GetTypeInfo(entity);

            State& s = S();
            if (!s.enabled || rec == nullptr)
                return rec;

            // Only the one call inside the use entry is intercepted.
            if (reinterpret_cast<std::uintptr_t>(_ReturnAddress()) != s.base + kUseEntryTypeCallReturnRva)
                return rec;

            const U32 typeId = *reinterpret_cast<const U32*>(static_cast<const unsigned char*>(rec) + kTypeIdOffset);
            Alias* a = FindAlias(typeId);
            if (a != nullptr && a->behavesLike == kGreenhouseTypeId)
            {
                if (s.verbose && a->hits < 20)
                    FileLog("use entry: type %u presented as greenhouse (%u)", typeId,
                            static_cast<unsigned>(kGreenhouseTypeId));
                ++a->hits;
                return a->fake;
            }
            return rec;
        }

        // --------------------------------------------------------------------- //
        // The inventory-open check (AbilityBaseOpenImp::openIt) accepts incomplete sites, devices, the warehouse types 131/132/516,
        // windmills and descendants of 1397 (Ruins). The last test is the call to this helper with ancestor 0x575 at RVA 0x386422
        // (return address): extra storage buildings listed in <warehouse objectTypeId=".."/> are reported as descendants there.
        U64 __fastcall OnTypeIsDescendant(LPVOID typeRec, int ancestorId, int depth)
        {
            State& s = S();
            if (ancestorId == 0x575 && !s.warehouseTypes.empty()
                && reinterpret_cast<std::uintptr_t>(_ReturnAddress()) == s.base + kOpenCheckReturnRva)
            {
                U32 typeId = 0;
                __try
                {
                    if (PlausiblePtr(typeRec))
                        typeId = *reinterpret_cast<const U32*>(static_cast<const unsigned char*>(typeRec) + 8);
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                    typeId = 0;
                }
                for (U32 t : s.warehouseTypes)
                {
                    if (t == typeId)
                    {
                        static long logged = 0;
                        if (s.verbose && logged < 10)
                        {
                            ++logged;
                            FileLog("open-inventory check: object type %u accepted as warehouse", typeId);
                        }
                        return 1;
                    }
                }
            }
            return _Type_IsDescendant(typeRec, ancestorId, depth);
        }
        // --------------------------------------------------------------------- //
        U64 __fastcall OnWcDoPerform(LPVOID ability)
        {
            State& s = S();
            if (!s.enabled || s.overrides.empty())
                return _Wc_DoPerform(ability);
            t_wcObjectType = ReadWcObjectType(ability);
            const U64 ret = _Wc_DoPerform(ability);
            t_wcObjectType = 0;
            return ret;
        }

        // --------------------------------------------------------------------- //
        LPVOID* __fastcall OnWcFindRecipe(LPVOID mgr, LPVOID* out, int skillId, LPVOID itemType)
        {
            LPVOID* ret = _Wc_FindRecipe(mgr, out, skillId, itemType);
            State& s = S();
            if (t_wcObjectType == 0 || out == nullptr)
                return ret;
            const int picked = ReadRecipeId(out);
            for (const RecipeOverride& o : s.overrides)
            {
                if (o.objectTypeId == t_wcObjectType && o.from == picked)
                {
                    const bool ok = SwapRecipe(mgr, out, o.to);
                    if (s.verbose)
                        FileLog("recipe override: object type %u, recipe %d -> %d (%s)",
                                static_cast<unsigned>(o.objectTypeId), o.from, o.to, ok ? "swapped" : "TARGET RECIPE NOT FOUND");
                    break;
                }
            }
            return ret;
        }

        // --------------------------------------------------------------------- //
        bool ConfigureGreenhouseAlias(const tinyxml2::XMLElement* root)
        {
            State& s = S();
            if (s.attached)
            {
                Lifx::ShowErrorMessage("Can't configure greenhouseAlias while the hook is attached.");
                return false;
            }

            s.enabled = false;
            s.aliases.clear();
            s.overrides.clear();
            s.warehouseTypes.clear();

            if (root == nullptr)
                return true;

            const tinyxml2::XMLElement* section = root->FirstChildElement("greenhouseAlias");
            if (section == nullptr || !section->BoolAttribute("enabled", false))
                return true;

            s.verbose = section->BoolAttribute("verbose", true);

            for (const tinyxml2::XMLElement* e = section->FirstChildElement("alias");
                 e != nullptr; e = e->NextSiblingElement("alias"))
            {
                unsigned from = 0, like = 0;
                if (e->QueryUnsignedAttribute("objectTypeId", &from) != tinyxml2::XML_SUCCESS || from == 0
                    || e->QueryUnsignedAttribute("behavesLike", &like) != tinyxml2::XML_SUCCESS)
                {
                    Lifx::ShowErrorMessage("Invalid <greenhouseAlias><alias>: objectTypeId and behavesLike are required.");
                    s.aliases.clear();
                    return false;
                }
                if (like != kGreenhouseTypeId && like != kDryingFrameTypeId && like != kTanningTubTypeId)
                {
                    Lifx::ShowErrorMessage(
                        "Invalid <alias objectTypeId=%u>: behavesLike must be %u (Herbal Garden), %u (Drying Frame) or %u (Tanning Tub).",
                        from, static_cast<unsigned>(kGreenhouseTypeId), static_cast<unsigned>(kDryingFrameTypeId),
                        static_cast<unsigned>(kTanningTubTypeId));
                    s.aliases.clear();
                    return false;
                }
                if (from == kGreenhouseTypeId || from == kDryingFrameTypeId || from == kTanningTubTypeId
                    || FindAlias(from) != nullptr)
                {
                    Lifx::ShowErrorMessage("Invalid or duplicate <alias objectTypeId=%u>.", from);
                    s.aliases.clear();
                    return false;
                }

                auto a = std::make_unique<Alias>();
                a->fromTypeId = from;
                a->behavesLike = like;
                a->finishedAsWorking = e->BoolAttribute("finishedAsWorking", false);
                *reinterpret_cast<U32*>(a->fake + kTypeIdOffset) = kGreenhouseTypeId;
                s.aliases.push_back(std::move(a));
            }

            for (const tinyxml2::XMLElement* e = section->FirstChildElement("recipeOverride");
                 e != nullptr; e = e->NextSiblingElement("recipeOverride"))
            {
                RecipeOverride o;
                unsigned typeId = 0;
                int from = 0, to = 0;
                if (e->QueryUnsignedAttribute("objectTypeId", &typeId) != tinyxml2::XML_SUCCESS || typeId == 0
                    || e->QueryIntAttribute("from", &from) != tinyxml2::XML_SUCCESS || from <= 0
                    || e->QueryIntAttribute("to", &to) != tinyxml2::XML_SUCCESS || to <= 0 || to == from)
                {
                    Lifx::ShowErrorMessage("Invalid <greenhouseAlias><recipeOverride>: objectTypeId, from and to (different, > 0) are required.");
                    s.aliases.clear();
                    s.overrides.clear();
                    return false;
                }
                o.objectTypeId = typeId;
                o.from = from;
                o.to = to;
                s.overrides.push_back(o);
            }

            for (const tinyxml2::XMLElement* e = section->FirstChildElement("warehouse");
                 e != nullptr; e = e->NextSiblingElement("warehouse"))
            {
                unsigned typeId = 0;
                if (e->QueryUnsignedAttribute("objectTypeId", &typeId) != tinyxml2::XML_SUCCESS || typeId == 0)
                {
                    Lifx::ShowErrorMessage("Invalid <greenhouseAlias><warehouse>: objectTypeId is required.");
                    s.aliases.clear();
                    s.overrides.clear();
                    s.warehouseTypes.clear();
                    return false;
                }
                s.warehouseTypes.push_back(typeId);
            }
            if (s.aliases.empty())
            {
                Lifx::ShowErrorMessage("greenhouseAlias is enabled, but no <alias> entries were provided.");
                return false;
            }

            s.enabled = true;
            return true;
        }

        // --------------------------------------------------------------------- //
        void AttachGreenhouseAliasHook()
        {
            State& s = S();
            if (!s.enabled || s.attached)
                return;

            s.base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
            if (s.base == 0)
            {
                Lifx::ShowErrorMessage("Can't attach greenhouse-alias hook: main module base is null.");
                return;
            }

            // push r14; mov eax,1840h; call rel32
            static constexpr std::uint8_t loadObjectsPrologue[] = {
                0x41, 0x56, 0xB8, 0x40, 0x18, 0x00, 0x00, 0xE8
            };
            // mov rax,[rcx+370h]; ret
            static constexpr std::uint8_t getTypeInfoBody[] = {
                0x48, 0x8B, 0x81, 0x70, 0x03, 0x00, 0x00, 0xC3
            };

            const std::uintptr_t loadObjects = s.base + kLoadObjectsRva;
            const std::uintptr_t getTypeInfo = s.base + kGetTypeInfoRva;
            const std::uintptr_t useEntry = s.base + 0x1D91D0;
            // The call we intercept: e8 rel32 -> 0xC9930 at 0x1D9220.
            static constexpr std::uint8_t useCallOpcode = 0xE8;

            if (!MatchBytes(loadObjects, loadObjectsPrologue, sizeof(loadObjectsPrologue))
                || !MatchBytes(getTypeInfo, getTypeInfoBody, sizeof(getTypeInfoBody))
                || !MatchBytes(s.base + 0x1D9220, &useCallOpcode, 1))
            {
                Lifx::ShowErrorMessage(
                    "Can't attach greenhouse-alias hook: expected code not found (different server build?).");
                return;
            }
            // The intercepted call must really target GetTypeInfo.
            const std::int32_t rel = *reinterpret_cast<const std::int32_t*>(s.base + 0x1D9221);
            if (s.base + 0x1D9225 + rel != getTypeInfo)
            {
                Lifx::ShowErrorMessage("Can't attach greenhouse-alias hook: use-entry call does not target GetTypeInfo.");
                return;
            }
            (void)useEntry;

            _Craftwork_LoadObjects = reinterpret_cast<_Craftwork_LoadObjects_Fn>(loadObjects);
            _Entity_GetTypeInfo = reinterpret_cast<_Entity_GetTypeInfo_Fn>(getTypeInfo);

            LONG rc = DetourAttach(&(PVOID&)_Craftwork_LoadObjects, OnCraftworkLoadObjects);
            if (rc != NO_ERROR)
            {
                Lifx::ShowErrorMessage("DetourAttach failed for greenhouse-alias loadObjects hook. Error: %ld", rc);
                return;
            }
            // GetTypeInfo is a hot 7-byte accessor; detour it only when a Herbal Garden alias needs
            // the use-entry stand-in record (Drying Frame / Tanning Tub aliases don't).
            bool needGetTypeInfo = false;
            for (const auto& a : s.aliases)
                if (a->behavesLike == kGreenhouseTypeId)
                    needGetTypeInfo = true;
            if (needGetTypeInfo)
            {
                rc = DetourAttach(&(PVOID&)_Entity_GetTypeInfo, OnEntityGetTypeInfo);
                if (rc != NO_ERROR)
                {
                    Lifx::ShowErrorMessage("DetourAttach failed for greenhouse-alias GetTypeInfo hook. Error: %ld", rc);
                    return;
                }
            }

            // Working-container recipe override (only when configured).
            if (!s.overrides.empty())
            {
                // mov rax,rsp; mov [rax+18h],r8d; mov [rax+8],rcx; push rbp/rsi/rdi/r12-r14 (recipe finder)
                static constexpr std::uint8_t findRecipePrologue[] = {
                    0x48, 0x8B, 0xC4, 0x44, 0x89, 0x40, 0x18, 0x48, 0x89, 0x48, 0x08, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56
                };
                // push rbp/rsi/rdi/r12-r15; lea rbp,[rsp-5FB0h] (AbilityBaseWC::_onDoPerform)
                static constexpr std::uint8_t doPerformPrologue[] = {
                    0x40, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xAC, 0x24, 0x50, 0xA0, 0xFF, 0xFF
                };
                // mov [rsp+10h],rbx; push rdi; sub rsp,30h; mov rdi,rdx (entity -> object)
                static constexpr std::uint8_t objFromEntityPrologue[] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B, 0xFA };
                // push rbx; sub rsp,20h; mov rbx,[rcx+8] (shared-reference release)
                static constexpr std::uint8_t releasePrologue[] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x59, 0x08 };

                if (!MatchBytes(s.base + kWcFindRecipeRva, findRecipePrologue, sizeof(findRecipePrologue))
                    || !MatchBytes(s.base + kWcDoPerformRva, doPerformPrologue, sizeof(doPerformPrologue))
                    || !MatchBytes(s.base + kObjectFromEntityRva, objFromEntityPrologue, sizeof(objFromEntityPrologue))
                    || !MatchBytes(s.base + kReleaseRefRva, releasePrologue, sizeof(releasePrologue)))
                {
                    Lifx::ShowErrorMessage("Can't attach working-container recipe override: expected code not found (different server build?).");
                    s.overrides.clear();
                }
                else
                {
                    _Wc_DoPerform = reinterpret_cast<_Wc_DoPerform_Fn>(s.base + kWcDoPerformRva);
                    _Wc_FindRecipe = reinterpret_cast<_Wc_FindRecipe_Fn>(s.base + kWcFindRecipeRva);
                    rc = DetourAttach(&(PVOID&)_Wc_DoPerform, OnWcDoPerform);
                    if (rc == NO_ERROR)
                        rc = DetourAttach(&(PVOID&)_Wc_FindRecipe, OnWcFindRecipe);
                    if (rc != NO_ERROR)
                    {
                        Lifx::ShowErrorMessage("DetourAttach failed for working-container recipe override hooks. Error: %ld", rc);
                        s.overrides.clear();
                    }
                }
            }

            // Extra warehouse types for the open-inventory check (only when configured).
            if (!s.warehouseTypes.empty())
            {
                // mov [rsp+8],rbx; mov [rsp+10h],rsi; push rdi; sub rsp,20h; mov edi,r8d; mov esi,edx (IsDescendant)
                static constexpr std::uint8_t isDescPrologue[] = {
                    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x41, 0x8B, 0xF8, 0x8B, 0xF2
                };
                const std::uintptr_t callSite = s.base + kOpenCheckCallRva;
                bool siteOk = MatchBytes(s.base + kIsDescendantRva, isDescPrologue, sizeof(isDescPrologue))
                    && *reinterpret_cast<const std::uint8_t*>(callSite) == 0xE8
                    && callSite + 5 + *reinterpret_cast<const std::int32_t*>(callSite + 1) == s.base + kIsDescendantRva;
                if (!siteOk)
                {
                    Lifx::ShowErrorMessage("Can't attach extra-warehouse hook: expected code not found (different server build?).");
                    s.warehouseTypes.clear();
                }
                else
                {
                    _Type_IsDescendant = reinterpret_cast<_Type_IsDescendant_Fn>(s.base + kIsDescendantRva);
                    rc = DetourAttach(&(PVOID&)_Type_IsDescendant, OnTypeIsDescendant);
                    if (rc != NO_ERROR)
                    {
                        Lifx::ShowErrorMessage("DetourAttach failed for extra-warehouse hook. Error: %ld", rc);
                        s.warehouseTypes.clear();
                    }
                }
            }
            s.attached = true;
            FileLog("hooks attached (loadObjects RVA 0x%X, GetTypeInfo RVA 0x%X), %u alias(es), %u recipe override(s), %u extra warehouse type(s)",
                    static_cast<unsigned>(kLoadObjectsRva), static_cast<unsigned>(kGetTypeInfoRva),
                    static_cast<unsigned>(s.aliases.size()), static_cast<unsigned>(s.overrides.size()),
                    static_cast<unsigned>(s.warehouseTypes.size()));
        }
    }
}
