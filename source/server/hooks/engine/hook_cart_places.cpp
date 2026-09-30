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
	Cart places. See hook_cart_places.h for the design.
*/

#include "hook_cart_places.h"

#include "core/tinyxml2.h"
#include "server/api/t3d_console.h"

#include <Windows.h>
#include <detours/detours.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <vector>

__CM_INSTATNTIATE(_Cart_PutInCart);
__CM_INSTATNTIATE(_Cart_PullFromCart);

namespace Hooks
{
    namespace Engine
    {
        namespace
        {
            constexpr std::uintptr_t kPutInCartRva = 0x37E1D0;
            constexpr std::uintptr_t kPullFromCartRva = 0x37D880;
            constexpr std::uintptr_t kCompareImmRva = 0x37E33D;       // imm8 of `cmp r14d,0Ch` (41 83 FE 0C) at 0x37E33A
            constexpr std::uintptr_t kMaxDisplayRva = 0x81D3E0;       // global int, the maximum printed by message 2678
            constexpr std::uintptr_t kObjectFromEntityRva = 0x33DDB0; // (Entity*, shared_ptr<Object>* out)
            constexpr std::uintptr_t kReleaseRefRva = 0x86D60;        // shared_ptr release
            constexpr std::uintptr_t kObjectByIdRva = 0x3E7750;       // (u32 id) -> Object* from the global id map (AttachedObject_Entity+8 holds the id)
            constexpr std::size_t kTypeIdOffset = 0x60;               // Type::getId reads [rec+0x60]
            constexpr std::size_t kDatablockIdOffset = 0x90;          // AttachedShapeData id inside the harness object's record (probe 2026-09-24)
            constexpr int kVanillaPlaces = 12;

            struct Rule
            {
                U32 typeId = 0;        // object type (0 = match by datablock only)
                U32 datablockId = 0;   // AttachedShapeData id of a harnessed cart (0 = match by type only)
                int places = kVanillaPlaces;
            };

            struct State
            {
                std::vector<Rule> rules;
                bool enabled = false;
                bool attached = false;
                bool verbose = true;
                std::uintptr_t base = 0;
                int current = kVanillaPlaces;   // value currently written to memory
                std::recursive_mutex callMutex;
                std::mutex logMutex;
            };

            State& S()
            {
                static State* state = new State();   // intentionally leaked (hooks may fire during shutdown)
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
                if (fopen_s(&f, "logs/cart_places.log", "a") == 0 && f)
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
            U32 ReadTargetObjectType(LPVOID ability, U32* datablockId)
            {
                *datablockId = 0;
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
                    if (type == 0)
                    {
                        // Harnessed carts are AttachedObject_Entity, not ComplexObject_Entity: the entity holds the object id at +8.
                        const U32 id = *reinterpret_cast<U32*>(static_cast<unsigned char*>(entity) + 8);
                        using ObjectByIdFn = LPVOID(__fastcall*)(U32);
                        LPVOID obj = reinterpret_cast<ObjectByIdFn>(base + kObjectByIdRva)(id);
                        LPVOID rec = nullptr;
                        if (PlausiblePtr(obj))
                        {
                            rec = *reinterpret_cast<LPVOID*>(static_cast<unsigned char*>(obj) + 0x370);
                            if (PlausiblePtr(rec))
                            {
                                type = *reinterpret_cast<U32*>(static_cast<unsigned char*>(rec) + kTypeIdOffset);
                                // For an attached harness object the record is its AttachedShapeData: datablock id at +0x90.
                                *datablockId = *reinterpret_cast<U32*>(static_cast<unsigned char*>(rec) + kDatablockIdOffset);
                            }
                        }
                        static long probes = 0;
                        if (probes < 10)
                        {
                            ++probes;
                            FileLog("probe: entity=%p id=%u obj=%p rec=%p type=%u datablock=%u", entity, id, obj, rec, type, *datablockId);
}
                    }
                    return type;
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                    return 0;
                }
            }

            bool WriteMemory(std::uintptr_t addr, const void* data, std::size_t n)
            {
                DWORD old = 0;
                if (!VirtualProtect(reinterpret_cast<LPVOID>(addr), n, PAGE_EXECUTE_READWRITE, &old))
                    return false;
                std::memcpy(reinterpret_cast<void*>(addr), data, n);
                DWORD tmp = 0;
                VirtualProtect(reinterpret_cast<LPVOID>(addr), n, old, &tmp);
                FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<LPCVOID>(addr), n);
                return true;
            }

            bool SetPlaces(int places)
            {
                State& s = S();
                if (places == s.current)
                    return true;
                const std::uint8_t imm = static_cast<std::uint8_t>(places);
                const std::int32_t disp = places;
                if (!WriteMemory(s.base + kCompareImmRva, &imm, 1) || !WriteMemory(s.base + kMaxDisplayRva, &disp, sizeof(disp)))
                {
                    FileLog("SetPlaces(%d) failed (VirtualProtect %lu)", places, GetLastError());
                    return false;
                }
                s.current = places;
                return true;
            }

            // Sets the capacity for the cart type the ability targets. Returns the type (for logging).
            U32 ApplyForAbility(LPVOID ability, const char* what)
            {
                State& s = S();
                U32 datablockId = 0;
                const U32 type = ReadTargetObjectType(ability, &datablockId);
                int places = kVanillaPlaces;
                for (const Rule& r : s.rules)
                {
                    const bool byType = r.typeId != 0 && r.typeId == type;
                    const bool byDatablock = r.datablockId != 0 && r.datablockId == datablockId;
                    if (byType || byDatablock)
                        places = r.places;
                }
                SetPlaces(places);
                if (s.verbose)
                {
                    static long logged = 0;
                    if (logged < 40)
                    {
                        ++logged;
                        FileLog("%s: cart type %u datablock %u -> %d places", what, type, datablockId, places);
                    }
                }
                return type;
            }
        }

        // --------------------------------------------------------------------- //
        U32 __fastcall OnCartPutInCart(LPVOID ability)
        {
            State& s = S();
            std::lock_guard<std::recursive_mutex> lock(s.callMutex);
            ApplyForAbility(ability, "putInCart");
            return _Cart_PutInCart(ability);
        }

        U32 __fastcall OnCartPullFromCart(LPVOID ability)
        {
            State& s = S();
            std::lock_guard<std::recursive_mutex> lock(s.callMutex);
            ApplyForAbility(ability, "pullFromCart");
            return _Cart_PullFromCart(ability);
        }

        // --------------------------------------------------------------------- //
        bool ConfigureCartPlaces(const tinyxml2::XMLElement* root)
        {
            State& s = S();
            if (s.attached)
            {
                Lifx::ShowErrorMessage("Can't configure cartPlaces while the hook is attached.");
                return false;
            }
            s.rules.clear();
            s.enabled = false;
            if (root == nullptr)
                return true;

            const tinyxml2::XMLElement* section = root->FirstChildElement("cartPlaces");
            if (section == nullptr || !section->BoolAttribute("enabled", false))
                return true;
            s.verbose = section->BoolAttribute("verbose", true);

            for (const tinyxml2::XMLElement* e = section->FirstChildElement("cart"); e != nullptr; e = e->NextSiblingElement("cart"))
            {
                unsigned typeId = 0, datablockId = 0;
                int places = 0;
                e->QueryUnsignedAttribute("objectTypeId", &typeId);
                e->QueryUnsignedAttribute("datablockId", &datablockId);
                if ((typeId == 0 && datablockId == 0)
                    || e->QueryIntAttribute("places", &places) != tinyxml2::XML_SUCCESS || places < 1 || places > 127)
                {
                    Lifx::ShowErrorMessage("Invalid <cartPlaces><cart>: objectTypeId or datablockId is required and places must be 1..127.");
                    s.rules.clear();
                    return false;
                }
                Rule r;
                r.typeId = typeId;
                r.datablockId = datablockId;
                r.places = places;
                s.rules.push_back(r);
            }
            if (s.rules.empty())
                return true;   // enabled but empty: nothing to do

            s.enabled = true;
            return true;
        }

        void AttachCartPlacesHook()
        {
            State& s = S();
            if (!s.enabled || s.attached)
                return;

            s.base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
            if (s.base == 0)
            {
                Lifx::ShowErrorMessage("Can't attach cart-places hook: main module base is null.");
                return;
            }

            // Verify the patch sites belong to the expected build: cmp r14d,0Ch ; jbe +17h, the displayed maximum (12), and the two
            // ability prologues (push rbp; push rbx; push rsi; push rdi).
            static constexpr std::uint8_t cmpBytes[] = { 0x41, 0x83, 0xFE, 0x0C, 0x76, 0x17 };
            static constexpr std::uint8_t prologue[] = { 0x40, 0x55, 0x53, 0x56, 0x57 };
            const std::int32_t shown = *reinterpret_cast<const std::int32_t*>(s.base + kMaxDisplayRva);
            if (std::memcmp(reinterpret_cast<const void*>(s.base + kCompareImmRva - 3), cmpBytes, sizeof(cmpBytes)) != 0
                || shown != kVanillaPlaces
                || std::memcmp(reinterpret_cast<const void*>(s.base + kPutInCartRva), prologue, sizeof(prologue)) != 0
                || std::memcmp(reinterpret_cast<const void*>(s.base + kPullFromCartRva), prologue, sizeof(prologue)) != 0)
            {
                Lifx::ShowErrorMessage("Can't attach cart-places hook: expected code not found (different server build?).");
                return;
            }

            _Cart_PutInCart = reinterpret_cast<_Cart_PutInCart_Fn>(s.base + kPutInCartRva);
            _Cart_PullFromCart = reinterpret_cast<_Cart_PullFromCart_Fn>(s.base + kPullFromCartRva);

            LONG rc = DetourAttach(&(PVOID&)_Cart_PutInCart, OnCartPutInCart);
            if (rc == NO_ERROR) rc = DetourAttach(&(PVOID&)_Cart_PullFromCart, OnCartPullFromCart);
            if (rc != NO_ERROR)
            {
                Lifx::ShowErrorMessage("DetourAttach failed for the cart-places hooks. Error: %ld", rc);
                return;
            }

            s.attached = true;
            FileLog("hooks attached: %u cart rule(s)", static_cast<unsigned>(s.rules.size()));
        }
    }
}