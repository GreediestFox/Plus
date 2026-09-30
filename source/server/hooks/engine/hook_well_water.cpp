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
	Well water. See hook_well_water.h for the design.
*/

#include "hook_well_water.h"

#include "core/tinyxml2.h"
#include "server/api/t3d_console.h"

#include <Windows.h>
#include <detours/detours.h>

#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <vector>

__CM_INSTATNTIATE(_Well_GetWater);
__CM_INSTATNTIATE(_Well_Gather);

namespace Hooks
{
    namespace Engine
    {
        namespace
        {
            constexpr std::uintptr_t kGetWaterRva = 0x39F780;
            constexpr std::uintptr_t kGatherRva = 0x37B090;
            constexpr std::uintptr_t kObjectFromEntityRva = 0x33DDB0; // (Entity*, shared_ptr<Object>* out)
            constexpr std::uintptr_t kReleaseRefRva = 0x86D60;        // shared_ptr release
            constexpr std::size_t kTypeIdOffset = 0x60;               // Type::getId reads [rec+0x60]
            constexpr U32 kWaterItemType = 204;                       // 0xCC

            struct Rule
            {
                U32 typeId = 0;
                int amount = 1;
                U32 itemType = kWaterItemType;   // item a well gives (default: Water 204; Fresh Water via itemTypeId=)
            };

            // Gather swap: whenever the server gathers item `from`, with `chance` percent it gives `to` instead.
            struct Swap
            {
                U32 from = 0;
                U32 to = 0;
                int chance = 0;
            };

            struct State
            {
                std::vector<Rule> rules;
                std::vector<Swap> swaps;
                bool enabled = false;
                bool attached = false;
                bool verbose = true;
                std::uintptr_t base = 0;
                std::mutex logMutex;
            };

            State& S()
            {
                static State* state = new State();   // intentionally leaked (hooks may fire during shutdown)
                return *state;
            }

            // TLS: amount wanted for the Water gathered inside the GetWater call currently running (0 = leave alone),
            // and the item a matching well gives instead of Water.
            thread_local U32 t_amount = 0;
            thread_local U32 t_item = kWaterItemType;

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
                if (fopen_s(&f, "logs/well_water.log", "a") == 0 && f)
                {
                    std::fprintf(f, "%s %s\n", stamp, msg);
                    std::fclose(f);
                }
            }

            bool PlausiblePtr(const void* p)
            {
                const std::uintptr_t v = reinterpret_cast<std::uintptr_t>(p);
                return v > 0x10000 && v < 0x00007FFFFFFF0000ull;
            }

            // Object type id of the entity the ability targets: entity -> object (engine lookup) -> type record [obj+0x370] -> id [rec+0x60].
            // Returns 0 for anything that is not a ComplexObject_Entity (natural water, ...).
            U32 ReadTargetObjectType(LPVOID ability)
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
        }

        // --------------------------------------------------------------------- //
        U32 __fastcall OnWellGetWater(LPVOID ability)
        {
            State& s = S();
            const U32 type = ReadTargetObjectType(ability);
            U32 amount = 0;
            U32 itemType = kWaterItemType;
            for (const Rule& r : s.rules)
                if (r.typeId == type)
                {
                    amount = static_cast<U32>(r.amount);
                    itemType = r.itemType;
                }
            if (s.verbose)
            {
                static long logged = 0;
                if (logged < 30)
                {
                    ++logged;
                    FileLog("getWater: target type %u -> %u x item %u", type, amount, itemType);
                }
            }
            const U32 saved = t_amount;
            const U32 savedItem = t_item;
            t_amount = amount;
            t_item = itemType;
            const U32 rc = _Well_GetWater(ability);
            t_amount = saved;
            t_item = savedItem;
            return rc;
        }

        U64 __fastcall OnWellGather(LPVOID mgr, LPVOID player, U32 connId, LPVOID pos, U32 chance, LPVOID itemRange,
                                    U64 quantity, char flag, LPVOID func)
        {
            State& s = S();
            if (PlausiblePtr(itemRange))
            {
                U32 item = 0;
                __try { item = *reinterpret_cast<U32*>(itemRange); }
                __except (EXCEPTION_EXECUTE_HANDLER) { item = 0; }

                if (s.verbose)
                {
                    // log every distinct item id the server gathers (shows which sources go through here)
                    static std::vector<U32> seen;
                    bool known = false;
                    for (U32 v : seen) if (v == item) { known = true; break; }
                    if (!known && seen.size() < 200)
                    {
                        seen.push_back(item);
                        FileLog("gather: item %u qty %llu chance %u", item, static_cast<unsigned long long>(quantity), chance);
                    }
                }

                U32 newItem = 0;
                if (t_amount != 0 && item == kWaterItemType)
                {
                    quantity = t_amount;
                    if (t_item != kWaterItemType)
                        newItem = t_item;
                }
                else
                {
                    for (const Swap& sw : s.swaps)
                        if (sw.from == item && (std::rand() % 100) < sw.chance)
                        {
                            newItem = sw.to;
                            break;
                        }
                }

                if (newItem != 0 && newItem != item)
                {
                    // never touch the engine's own range object: gather from a private copy with the item id replaced
                    static U32 buffers[64][16];
                    static unsigned next = 0;
                    U32* buf = buffers[(next++) & 63];
                    bool copied = true;
                    __try { std::memcpy(buf, itemRange, sizeof(U32) * 16); }
                    __except (EXCEPTION_EXECUTE_HANDLER) { copied = false; }
                    if (copied)
                    {
                        buf[0] = newItem;
                        itemRange = buf;
                        if (s.verbose)
                        {
                            static long swaps = 0;
                            if (swaps < 60) { ++swaps; FileLog("gather swap: %u -> %u", item, newItem); }
                        }
                    }
                }
            }
            return _Well_Gather(mgr, player, connId, pos, chance, itemRange, quantity, flag, func);
        }

        // --------------------------------------------------------------------- //
        bool ConfigureWellWater(const tinyxml2::XMLElement* root)
        {
            State& s = S();
            if (s.attached)
            {
                Lifx::ShowErrorMessage("Can't configure wellWater while the hook is attached.");
                return false;
            }
            s.rules.clear();
            s.enabled = false;
            if (root == nullptr)
                return true;

            const tinyxml2::XMLElement* section = root->FirstChildElement("wellWater");
            if (section == nullptr || !section->BoolAttribute("enabled", false))
                return true;
            s.verbose = section->BoolAttribute("verbose", true);

            for (const tinyxml2::XMLElement* e = section->FirstChildElement("well"); e != nullptr; e = e->NextSiblingElement("well"))
            {
                unsigned typeId = 0;
                int amount = 0;
                if (e->QueryUnsignedAttribute("objectTypeId", &typeId) != tinyxml2::XML_SUCCESS || typeId == 0
                    || e->QueryIntAttribute("amount", &amount) != tinyxml2::XML_SUCCESS || amount < 1 || amount > 1000)
                {
                    Lifx::ShowErrorMessage("Invalid <wellWater><well>: objectTypeId is required and amount must be 1..1000.");
                    s.rules.clear();
                    return false;
                }
                Rule r;
                r.typeId = typeId;
                r.amount = amount;
                unsigned itemType = kWaterItemType;
                if (e->QueryUnsignedAttribute("itemTypeId", &itemType) == tinyxml2::XML_SUCCESS && itemType != 0)
                    r.itemType = itemType;
                s.rules.push_back(r);
            }
            for (const tinyxml2::XMLElement* e = section->FirstChildElement("swap"); e != nullptr; e = e->NextSiblingElement("swap"))
            {
                unsigned from = 0, to = 0;
                int chance = 0;
                if (e->QueryUnsignedAttribute("from", &from) != tinyxml2::XML_SUCCESS || from == 0
                    || e->QueryUnsignedAttribute("to", &to) != tinyxml2::XML_SUCCESS || to == 0
                    || e->QueryIntAttribute("chance", &chance) != tinyxml2::XML_SUCCESS || chance < 1 || chance > 100)
                {
                    Lifx::ShowErrorMessage("Invalid <wellWater><swap>: from and to are required and chance must be 1..100.");
                    s.rules.clear();
                    s.swaps.clear();
                    return false;
                }
                Swap sw;
                sw.from = from;
                sw.to = to;
                sw.chance = chance;
                s.swaps.push_back(sw);
            }
            if (s.rules.empty() && s.swaps.empty())
                return true;   // enabled but empty: nothing to do

            s.enabled = true;
            return true;
        }

        void AttachWellWaterHook()
        {
            State& s = S();
            if (!s.enabled || s.attached)
                return;

            s.base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
            if (s.base == 0)
            {
                Lifx::ShowErrorMessage("Can't attach well-water hook: main module base is null.");
                return;
            }

            // GetWater::_onDoPerform starts push rbp; push rbx; push rsi; push rdi; push r12; push r14; push r15
            // Gathering::Manager::gather starts mov rax,rsp; mov [rax+20h],r9; push rbp; push rdi
            static constexpr std::uint8_t getWaterPrologue[] = { 0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57 };
            static constexpr std::uint8_t gatherPrologue[] = { 0x48, 0x8B, 0xC4, 0x4C, 0x89, 0x48, 0x20, 0x55, 0x57 };
            if (std::memcmp(reinterpret_cast<const void*>(s.base + kGetWaterRva), getWaterPrologue, sizeof(getWaterPrologue)) != 0
                || std::memcmp(reinterpret_cast<const void*>(s.base + kGatherRva), gatherPrologue, sizeof(gatherPrologue)) != 0)
            {
                Lifx::ShowErrorMessage("Can't attach well-water hook: expected code not found (different server build?).");
                return;
            }

            _Well_GetWater = reinterpret_cast<_Well_GetWater_Fn>(s.base + kGetWaterRva);
            _Well_Gather = reinterpret_cast<_Well_Gather_Fn>(s.base + kGatherRva);

            LONG rc = DetourAttach(&(PVOID&)_Well_GetWater, OnWellGetWater);
            if (rc == NO_ERROR) rc = DetourAttach(&(PVOID&)_Well_Gather, OnWellGather);
            if (rc != NO_ERROR)
            {
                Lifx::ShowErrorMessage("DetourAttach failed for the well-water hooks. Error: %ld", rc);
                return;
            }

            s.attached = true;
            std::srand(static_cast<unsigned>(std::time(nullptr)));
            FileLog("hooks attached: %u well rule(s), %u swap rule(s)", static_cast<unsigned>(s.rules.size()), static_cast<unsigned>(s.swaps.size()));
        }
    }
}