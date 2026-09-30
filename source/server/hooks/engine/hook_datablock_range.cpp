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
*  =================================================================================== *//*
	Datablock id range. See hook_datablock_range.h for the design.
*/

#include "hook_datablock_range.h"

#include "core/tinyxml2.h"
#include "server/api/t3d_console.h"

#include <Windows.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace Hooks
{
    namespace Engine
    {
        namespace
        {
            constexpr std::uintptr_t kCounterStartImm = 0xCEC86;   // `mov dword ptr [rcx+8], 320h` (C7 41 08 imm32)
            constexpr std::uintptr_t kWriteMaxImm = 0x13372B;      // `mov r9d, 402h` (41 B9 imm32) before the ranged write of a datablock id
            constexpr std::uintptr_t kReadMaxImm = 0x133D24;       // `mov r8d, 402h` (41 B8 imm32) before the ranged read
            constexpr std::uintptr_t kDynFirstImm = 0x4297F7;      // `mov ecx, 443h` in Sim init (B9 imm32)
            constexpr std::uintptr_t kDynCounterVar = 0xBC8FD4;    // global next-object-id counter (int)
            constexpr std::uintptr_t kIdBitsImm1 = 0x11D7D8;       // mov r8d, 0Ah (41 B8 imm32) : writeInt(id - 3, 10 bits), mounted images
            constexpr std::uintptr_t kIdBitsImm2 = 0x1389B6;       // mov r8d, 0Ah : writeInt(datablock id - 3, 10 bits)
            constexpr std::uint32_t kOldBits = 10, kNewBits = 12;

            constexpr std::uint32_t kOldCounterStart = 800, kNewCounterStart = 0x403;
            constexpr std::uint32_t kOldLast = 0x402, kNewLast = 0x1002;
            constexpr std::uint32_t kOldDynFirst = 0x443, kNewDynFirst = 0x1043;

            bool g_enabled = false;
            bool g_verbose = true;
            bool g_attached = false;

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
                if (fopen_s(&f, "logs/datablock_range.log", "a") == 0 && f)
                {
                    std::fprintf(f, "%s %s\n", stamp, msg);
                    std::fclose(f);
                }
            }

            bool CheckImm(std::uintptr_t addr, std::uint32_t expect, std::uint8_t opcodeBefore1, int which)
            {
                (void)opcodeBefore1;
                std::uint32_t cur = 0;
                std::memcpy(&cur, reinterpret_cast<const void*>(addr), 4);
                if (cur != expect)
                {
                    FileLog("site %d at %p holds %08X, expected %08X - not patching", which, reinterpret_cast<void*>(addr), cur, expect);
                    return false;
                }
                return true;
            }

            bool WriteImm(std::uintptr_t addr, std::uint32_t value)
            {
                DWORD old = 0;
                if (!VirtualProtect(reinterpret_cast<void*>(addr), 4, PAGE_EXECUTE_READWRITE, &old))
                    return false;
                std::memcpy(reinterpret_cast<void*>(addr), &value, 4);
                DWORD dummy = 0;
                VirtualProtect(reinterpret_cast<void*>(addr), 4, old, &dummy);
                FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(addr), 4);
                return true;
            }
        }

        bool ConfigureDatablockRange(const tinyxml2::XMLElement* root)
        {
            g_enabled = false;
            if (root == nullptr)
                return true;
            const tinyxml2::XMLElement* section = root->FirstChildElement("datablockRange");
            if (section == nullptr || !section->BoolAttribute("enabled", false))
                return true;
            g_verbose = section->BoolAttribute("verbose", true);
            g_enabled = true;
            return true;
        }

        void AttachDatablockRangeHook()
        {
            if (!g_enabled || g_attached)
                return;
            const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
            if (base == 0)
                return;

            // every site is checked first: all of them or none, so a different server build cannot be half patched
            if (!CheckImm(base + kCounterStartImm, kOldCounterStart, 0, 1) || !CheckImm(base + kWriteMaxImm, kOldLast, 0, 2)
                || !CheckImm(base + kReadMaxImm, kOldLast, 0, 3) || !CheckImm(base + kDynFirstImm, kOldDynFirst, 0, 4)
                || !CheckImm(base + kIdBitsImm1, kOldBits, 0, 5) || !CheckImm(base + kIdBitsImm2, kOldBits, 0, 6))
            {
                Lifx::ShowErrorMessage("Can't apply the datablock range change: expected code not found (different server build?).");
                return;
            }

            const bool ok = WriteImm(base + kCounterStartImm, kNewCounterStart) && WriteImm(base + kWriteMaxImm, kNewLast)
                && WriteImm(base + kReadMaxImm, kNewLast) && WriteImm(base + kDynFirstImm, kNewDynFirst)
                && WriteImm(base + kIdBitsImm1, kNewBits) && WriteImm(base + kIdBitsImm2, kNewBits);
            if (!ok)
            {
                Lifx::ShowErrorMessage("Datablock range change failed (VirtualProtect).");
                return;
            }

            // Sim init may already have run: lift the live counter of ordinary object ids above the new datablock range (only ever upwards).
            volatile LONG* counter = reinterpret_cast<volatile LONG*>(base + kDynCounterVar);
            LONG cur = *counter;
            int lifted = 0;
            while (cur < static_cast<LONG>(kNewDynFirst))
            {
                const LONG prev = InterlockedCompareExchange(counter, static_cast<LONG>(kNewDynFirst), cur);
                if (prev == cur) { lifted = 1; break; }
                cur = prev;
            }

            g_attached = true;
            FileLog("datablock range widened: last id %#x -> %#x, counter start %u -> %u, dynamic ids from %#x (live counter was %ld%s)",
                kOldLast, kNewLast, kOldCounterStart, kNewCounterStart, kNewDynFirst, static_cast<long>(cur), lifted ? ", lifted" : ", left");
        }
    }
}