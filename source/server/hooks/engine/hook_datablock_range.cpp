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
#include "hook_patch_util.h"

#include "core/tinyxml2.h"
#include "server/api/t3d_console.h"
#include "server/cm_offsets.h"

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
            constexpr std::uintptr_t kCounterStartImm = CmOffset::DATABLOCK_COUNTER_START_IMM;   // `mov dword ptr [rcx+8], 320h` (C7 41 08 imm32)
            constexpr std::uintptr_t kWriteMaxImm = CmOffset::DATABLOCK_WRITE_MAX_IMM;      // `mov r9d, 402h` (41 B9 imm32) before the ranged write of a datablock id
            constexpr std::uintptr_t kReadMaxImm = CmOffset::DATABLOCK_READ_MAX_IMM;       // `mov r8d, 402h` (41 B8 imm32) before the ranged read
            constexpr std::uintptr_t kDynFirstImm = CmOffset::DATABLOCK_DYN_FIRST_IMM;      // `mov ecx, 443h` in Sim init (B9 imm32)
            constexpr std::uintptr_t kDynCounterVar = CmOffset::DATABLOCK_DYN_COUNTER_VAR;    // global next-object-id counter (int)
            constexpr std::uintptr_t kIdBitsImm1 = CmOffset::DATABLOCK_ID_BITS_IMM1;       // mov r8d, 0Ah (41 B8 imm32) : writeInt(id - 3, 10 bits), mounted images
            constexpr std::uintptr_t kIdBitsImm2 = CmOffset::DATABLOCK_ID_BITS_IMM2;       // mov r8d, 0Ah : writeInt(datablock id - 3, 10 bits)
            constexpr std::uintptr_t kIdBitsImm3 = CmOffset::DATABLOCK_ID_BITS_IMM3;       // mov r8d, 0Ah : Sim2DAudioEvent::pack, writeInt(datablock id - 3, 10 bits)
            constexpr std::uintptr_t kReserveAddImm8 = CmOffset::DATABLOCK_ID_RESERVE_ADD_IMM8;   // add rax, 0Ah (48 83 C0 imm8) : space check of the mounted-image id write
            constexpr std::uintptr_t kReserveImm = CmOffset::DATABLOCK_ID_RESERVE_IMM;      // mov edx, 0Ah (BA imm32) : bit count handed to the stream grow call
            constexpr std::uintptr_t kUnpackBitsImm1 = CmOffset::DATABLOCK_UNPACK_BITS_IMM1;   // mov edx, 0Ah (BA imm32) : readInt(10 bits) + 3, mounted images
            constexpr std::uintptr_t kUnpackBitsImm2 = CmOffset::DATABLOCK_UNPACK_BITS_IMM2;   // mov edx, 0Ah : Sim2DAudioEvent::unpack
            constexpr std::uintptr_t kUnpackBitsImm3 = CmOffset::DATABLOCK_UNPACK_BITS_IMM3;   // mov edx, 0Ah : unpack of the pack at 0x1389A0
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

            // One patched immediate: the opcode bytes that precede it (so we know it IS the expected instruction, not just
            // four bytes that happen to hold the same number), the value it holds, and the value to write.
            struct Site
            {
                const char* what;
                std::uintptr_t immRva;
                std::uint8_t prefix[3];     // the bytes immediately before the imm32, last one adjacent to it
                std::size_t prefixLen;
                std::uint32_t oldValue;
                std::uint32_t newValue;
                std::size_t size;           // width of the immediate in bytes: 4 (imm32) or 1 (imm8)
            };

            const Site kSites[] = {
                { "TypeStorage counter start (mov [rcx+8],imm32)", kCounterStartImm, { 0xC7, 0x41, 0x08 }, 3, kOldCounterStart, kNewCounterStart, 4 },
                { "ghost pack max (mov r9d,imm32)",                kWriteMaxImm,     { 0x41, 0xB9, 0x00 }, 2, kOldLast,         kNewLast,         4 },
                { "ghost unpack max (mov r8d,imm32)",              kReadMaxImm,      { 0x41, 0xB8, 0x00 }, 2, kOldLast,         kNewLast,         4 },
                { "Sim init first dynamic id (mov ecx,imm32)",     kDynFirstImm,     { 0xB9, 0x00, 0x00 }, 1, kOldDynFirst,     kNewDynFirst,     4 },
                { "mounted-image id bits (mov r8d,imm32)",         kIdBitsImm1,      { 0x41, 0xB8, 0x00 }, 2, kOldBits,         kNewBits,         4 },
                { "datablock id bits (mov r8d,imm32)",             kIdBitsImm2,      { 0x41, 0xB8, 0x00 }, 2, kOldBits,         kNewBits,         4 },
                { "Sim2DAudioEvent id bits (mov r8d,imm32)",       kIdBitsImm3,      { 0x41, 0xB8, 0x00 }, 2, kOldBits,         kNewBits,         4 },
                { "mounted-image id space check (add rax,imm8)",   kReserveAddImm8,  { 0x48, 0x83, 0xC0 }, 3, kOldBits,         kNewBits,         1 },
                { "mounted-image id grow request (mov edx,imm32)", kReserveImm,      { 0xBA, 0x00, 0x00 }, 1, kOldBits,         kNewBits,         4 },
                { "mounted-image id read bits (mov edx,imm32)",    kUnpackBitsImm1,  { 0xBA, 0x00, 0x00 }, 1, kOldBits,         kNewBits,         4 },
                { "Sim2DAudioEvent id read bits (mov edx,imm32)",  kUnpackBitsImm2,  { 0xBA, 0x00, 0x00 }, 1, kOldBits,         kNewBits,         4 },
                { "datablock id read bits (mov edx,imm32)",        kUnpackBitsImm3,  { 0xBA, 0x00, 0x00 }, 1, kOldBits,         kNewBits,         4 },
            };

            // The opcode prefix AND the current immediate must match, otherwise the build is not the one these RVAs belong to.
            bool CheckSite(std::uintptr_t base, const Site& site)
            {
                const std::uint8_t* imm = reinterpret_cast<const std::uint8_t*>(base + site.immRva);
                // prefix[0..prefixLen) are the opcode bytes directly in front of the imm32.
                if (std::memcmp(imm - site.prefixLen, site.prefix, site.prefixLen) != 0)
                {
                    FileLog("site '%s' at %p: unexpected opcode bytes - not patching", site.what, static_cast<const void*>(imm));
                    return false;
                }
                std::uint32_t cur = 0;
                std::memcpy(&cur, imm, site.size);
                if (cur != site.oldValue)
                {
                    FileLog("site '%s' at %p holds %08X, expected %08X - not patching", site.what, static_cast<const void*>(imm), cur, site.oldValue);
                    return false;
                }
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
            for (const Site& site : kSites)
            {
                if (!CheckSite(base, site))
                {
                    Lifx::ShowErrorMessage("Can't apply the datablock range change: expected code not found (different server build?).");
                    return;
                }
            }

            BytePatchSet patches;
            for (const Site& site : kSites)
            {
                const bool ok = site.size == 1 ? patches.WriteByte(base + site.immRva, static_cast<std::uint8_t>(site.newValue))
                                               : patches.WriteU32(base + site.immRva, site.newValue);
                if (!ok)
                {
                    Lifx::ShowErrorMessage("Datablock range change failed (VirtualProtect); nothing was changed.");
                    patches.Rollback();
                    return;
                }
            }
            patches.Release();     // not reverted at runtime: ids above the old limit may already be in use

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