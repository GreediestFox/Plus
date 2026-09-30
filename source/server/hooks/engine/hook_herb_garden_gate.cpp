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
    Herb-garden time gate. See hook_herb_garden_gate.h for the design.

    Engine facts (ddctd_cm_yo_server.exe, verified by decompilation + disassembly):
      - craftWithDevice(mgr, ctx, isDevice, extra) @ 0x1E4B10. ctx = ability instance + 8:
          ctx[0] = GameConnection*      (type-mask bit 0x4000 at +0x10; char id at conn+0xB14)
          ctx[3] = character id (low 32 bits)
          ctx[4] = ability definition*  (ability id at +8)
          ctx[6] = target entity*       (persistent object id at entity+900, state at +0x380)
          ctx[8] = chosen recipe id (int), valid when the byte at ctx+0x44 is non-zero
        It returns a bool in the low byte. The result is created by _finallyMakeItem.
      - _finallyMakeItem @ 0x1E3C60 (mgr, conn, sp* recipe, sp* item, resultType, qty,
        quality, exceptional, abilityId, arg10). Its success tail releases the two shared
        references (helper 0x86D60) and returns 1; the plant path does exactly that.
      - Message helper @ 0x8CC10 (conn, messageId, 0, 0).
*  =================================================================================== */

#include "hook_herb_garden_gate.h"

#include "core/tinyxml2.h"
#include "server/api/t3d_console.h"

#include <Windows.h>
#include <detours/detours.h>

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <string>
#include <vector>

__CM_INSTATNTIATE(_Creation_CraftWithDevice);
__CM_INSTATNTIATE(_Creation_FinallyMakeItem);
__CM_INSTATNTIATE(_Ability_GetDuration);

namespace Hooks
{
    namespace Engine
    {
        namespace
        {
            constexpr std::uintptr_t kCraftWithDeviceRva = 0x1E4B10;
            constexpr std::uintptr_t kFinallyMakeItemRva = 0x1E3C60;
            constexpr std::uintptr_t kGetDurationRva = 0x31B3F0;
            constexpr std::uintptr_t kSendMessageRva = 0x8CC10;
            constexpr std::uintptr_t kReleaseRefRva = 0x86D60;
            // Working-container helpers (same ones the tub / drying frame abilities use):
            constexpr std::uintptr_t kObjectFromEntityRva = 0x33DDB0;   // (Entity*, shared_ptr<Object>* out)
            constexpr std::uintptr_t kStartWorkingRva = 0x1D9E40;       // (mgr, Object*, int ms): timer + row + State_Working
            constexpr std::uintptr_t kFinishWorkingRva = 0x1D9AB0;      // (mgr, Object*): timer off + row deleted + State_Complete
            constexpr std::uintptr_t kWorkingMgrGlobalRva = 0xB84E20;   // pointer to the CmCraftworkManager

            constexpr std::size_t kCtxAbilityDefIndex = 4;   // ctx[4]
            constexpr std::size_t kCtxTargetIndex = 6;       // ctx[6]
            constexpr std::size_t kCtxRecipeIdOffset = 8 * 8;    // ctx[8] as int
            constexpr std::size_t kCtxRecipeFlagOffset = 0x44;   // byte
            constexpr std::size_t kCtxCharIdIndex = 3;           // ctx[3] low 32 bits
            constexpr std::size_t kAbilityIdOffset = 8;          // *(int*)(abilityDef + 8)
            constexpr std::size_t kEntityIdOffset = 8;           // 8-byte object id at entity+8 (FUN_14033de50 looks the object up by it)

            struct Batch
            {
                int growRecipe = 0;
                std::int64_t readyAt = 0;   // unix seconds
                int plantQuality = -1;      // quality the engine computed for the planting craft (-1 = unknown)
            };

            // One "plant now, collect later" gate: an ability, a range of "grow" recipes (each with a hidden
            // "collect" recipe at +collectOffset) and an optional client-visible "collect anything" recipe.
            struct Gate
            {
                int abilityId = 328;
                std::int64_t growSeconds = 32400;
                int firstGrowRecipe = 5200;
                int lastGrowRecipe = 5265;
                int collectOffset = 100;
                U32 pendingMessageId = 3632;
                U32 emptyMessageId = 1230;      // game message "Empty"
                int collectAnyRecipe = 0;       // client-visible "Collect ..." recipe (0 = none)
                U32 timeMessageBase = 0;        // first id of the time-left message family (0 = use pendingMessageId)
                U32 timeMessageMax = 53;        // number of ids in that family (later buckets are clamped to it)
                int outputQuality = 0;          // >0: items made by the collect craft get exactly this quality
                // Logarithmic quality curve (curveEnd > 0): the collect quality is derived from the quality of the
                // planted input: gain(q) = gainEnd + (gainStart - gainEnd) * ln(r) / ln(rMax), r = (100-q)/(100-curveEnd),
                // rMax = 100/(100-curveEnd); i.e. +gainStart at q=0 falling to +gainEnd at q=curveEnd, result capped at curveEnd.
                int curveEnd = 0;               // 0 = curve off (use outputQuality)
                double curveGainStart = 20.0;
                double curveGainEnd = 1.0;
                bool workingState = false;      // put the building in State_Working (model/effects + native restart restore) while a batch runs
                bool noWorkshopBuff = false;    // crafts of this gate are exempt from the workshop crafting-quality buff
                // instant = no plant/collect timer: the grow recipes craft directly (output at once, quality curve applied), and
                // durationSeconds (> 0) makes the craft's progress bar that long (Ability::GetDuration override).
                bool instant = false;
                int durationSeconds = 0;
            };

            struct State
            {
                bool enabled = false;
                bool attached = false;
                bool verbose = true;
                bool debug = false;
                long debugBudget = 60;      // max debug lines per server run
                std::vector<Gate> gates;
                std::string file = "config/herb_garden_batches.txt";
                std::uintptr_t base = 0;

                std::mutex mutex;
                std::map<std::uint64_t, Batch> batches;   // key: object id of the garden / washer
                std::mutex logMutex;
            };
            // Intentionally leaked: hooks may fire during process shutdown.
            State& S()
            {
                static State* state = new State();
                return *state;
            }

            // TLS flag: true only while a "plant" craft is running inside craftWithDevice.
            thread_local bool t_plantMode = false;

            // TLS: non-zero only while a wash craft (fixed output quality) runs inside craftWithDevice.
            thread_local int t_forceQuality = 0;

            // TLS: gate whose direct (instant) craft is running; _finallyMakeItem applies its quality curve.
            struct Gate;
            thread_local const void* t_curveGate = nullptr;

            // TLS: true while a craft of a gate with noWorkshopBuff runs; read by hook_workshop_buff.
            thread_local bool t_suppressBuff = false;

            struct SuppressBuffScope
            {
                bool on;
                explicit SuppressBuffScope(bool b) : on(b) { if (on) t_suppressBuff = true; }
                ~SuppressBuffScope() { if (on) t_suppressBuff = false; }
            };

            // TLS: quality the engine computed for the planting craft (captured in _finallyMakeItem), -1 = none yet.
            thread_local int t_plantQuality = -1;

            void FileLog(const char* fmt, ...)
            {
                State& s = S();
                char msg[500];
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
                if (fopen_s(&f, "logs/herb_garden_gate.log", "a") == 0 && f)
                {
                    std::fprintf(f, "%s %s\n", stamp, msg);
                    std::fclose(f);
                }
            }

            bool MatchBytes(std::uintptr_t addr, const std::uint8_t* bytes, std::size_t n)
            {
                return std::memcmp(reinterpret_cast<const void*>(addr), bytes, n) == 0;
            }

            // ----------------------------------------------------------------- //
            // Everything read from the craft context, gathered under SEH so a bad
            // pointer can never crash the server (plain C types only: __try).
            struct CtxInfo
            {
                bool ok = false;
                LPVOID conn = nullptr;
                int abilityId = 0;
                int recipeId = 0;
                bool recipeSelected = false;
                std::uint32_t charId = 0;
                std::uint64_t gardenId = 0;
            };

            bool PlausiblePtr(const void* p)
            {
                const std::uintptr_t v = reinterpret_cast<std::uintptr_t>(p);
                return v > 0x10000 && v < 0x00007FFFFFFF0000ull;
            }

            bool ReadCtxRaw(LPVOID* ctx, CtxInfo& out)
            {
                __try
                {
                    if (!PlausiblePtr(ctx))
                        return false;
                    out.conn = ctx[0];
                    const void* abilityDef = ctx[kCtxAbilityDefIndex];
                    if (!PlausiblePtr(abilityDef))
                        return false;
                    out.abilityId = *reinterpret_cast<const int*>(
                        static_cast<const unsigned char*>(abilityDef) + kAbilityIdOffset);
                    out.recipeId = *reinterpret_cast<const int*>(
                        reinterpret_cast<const unsigned char*>(ctx) + kCtxRecipeIdOffset);
                    out.recipeSelected =
                        *(reinterpret_cast<const unsigned char*>(ctx) + kCtxRecipeFlagOffset) != 0;
                    out.charId = static_cast<std::uint32_t>(
                        reinterpret_cast<std::uintptr_t>(ctx[kCtxCharIdIndex]) & 0xFFFFFFFFu);
                    const void* entity = ctx[kCtxTargetIndex];
                    if (!PlausiblePtr(entity))
                        return false;
                    out.gardenId = *reinterpret_cast<const std::uint64_t*>(
                        static_cast<const unsigned char*>(entity) + kEntityIdOffset);
                    return out.gardenId != 0 && PlausiblePtr(out.conn);
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                    return false;
                }
            }

            // Debug: raw first 13 qwords of the craft context plus the ability id, read under SEH.
            bool DumpCtxRaw(LPVOID* ctx, unsigned long long words[13], int& abilityId, unsigned char& flag44)
            {
                __try
                {
                    if (!PlausiblePtr(ctx))
                        return false;
                    for (int i = 0; i < 13; ++i)
                        words[i] = reinterpret_cast<unsigned long long>(ctx[i]);
                    flag44 = *(reinterpret_cast<const unsigned char*>(ctx) + kCtxRecipeFlagOffset);
                    const void* def = ctx[kCtxAbilityDefIndex];
                    abilityId = PlausiblePtr(def)
                        ? *reinterpret_cast<const int*>(static_cast<const unsigned char*>(def) + kAbilityIdOffset)
                        : -1;
                    return true;
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                    return false;
                }
            }

            bool WriteRecipeId(LPVOID* ctx, int recipeId)
            {
                __try
                {
                    *reinterpret_cast<int*>(reinterpret_cast<unsigned char*>(ctx) + kCtxRecipeIdOffset) = recipeId;
                    return true;
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                    return false;
                }
            }

            void SendMessageToPlayer(LPVOID conn, U32 messageId)
            {
                using Fn = void(__fastcall*)(LPVOID, U32, U64, U64);
                const Fn fn = reinterpret_cast<Fn>(S().base + kSendMessageRva);
                __try { fn(conn, messageId, 0, 0); }
                __except (EXCEPTION_EXECUTE_HANDLER) {}
            }

            // Time-left message family (ids base+1 .. base+timeMessageMax), texts live in cm_messages.xml:
            //  +1 <5 min, +2 ~15 min, +3 ~30 min, +4 ~45 min, +5 ~1 h, +4+h = ~h hours (h = 2..48), +53 = more than 2 days.
            // Buckets past timeMessageMax are clamped to it.
            U32 TimeLeftMessageId(const Gate& g, std::int64_t secondsLeft)
            {
                const U32 base = g.timeMessageBase;
                if (base == 0)
                    return g.pendingMessageId;
                U32 idx;
                if (secondsLeft <= 300) idx = 1;
                else if (secondsLeft <= 900) idx = 2;
                else if (secondsLeft <= 1800) idx = 3;
                else if (secondsLeft <= 2700) idx = 4;
                else if (secondsLeft <= 3600) idx = 5;
                else
                {
                    const std::int64_t hours = (secondsLeft + 3599) / 3600;
                    idx = hours > 48 ? 53 : 4 + static_cast<U32>(hours);
                }
                if (idx > g.timeMessageMax) idx = g.timeMessageMax;
                return base + idx;
            }
            // Logarithmic quality curve, see Gate. Inputs at or above curveEnd are returned unchanged.
            int CurveQuality(const Gate& g, int q)
            {
                if (g.curveEnd <= 0 || g.curveEnd >= 100)
                    return q;
                if (q < 0) q = 0;
                if (q >= g.curveEnd)
                    return q;
                const double rMax = 100.0 / (100.0 - g.curveEnd);
                const double r = (100.0 - q) / (100.0 - g.curveEnd);
                double gain = g.curveGainEnd;
                if (rMax > 1.0 && r > 1.0)
                    gain += (g.curveGainStart - g.curveGainEnd) * std::log(r) / std::log(rMax);
                int out = static_cast<int>(q + gain + 0.5);
                if (out > g.curveEnd) out = g.curveEnd;
                if (out < 1) out = 1;
                return out;
            }

            void ReleaseRef(LPVOID* ref)
            {
                using Fn = void(__fastcall*)(LPVOID);
                const Fn fn = reinterpret_cast<Fn>(S().base + kReleaseRefRva);
                fn(ref);
            }

            // Start / stop the engine's own "working" state on the building the craft targets. Everything is done
            // through the same functions the Tanning Tub / Drying Frame abilities call, so the timer row in
            // `working_containers` is written / deleted by the engine and restored at boot by loadObjects.
            // ms = 0 finishes the work (State_Complete), ms > 0 starts it (State_Working).
            void SetWorkingState(LPVOID* ctx, int ms)
            {
                State& s = S();
                __try
                {
                    LPVOID entity = ctx[kCtxTargetIndex];
                    if (!PlausiblePtr(entity))
                        return;
                    LPVOID mgr = *reinterpret_cast<LPVOID*>(s.base + kWorkingMgrGlobalRva);
                    if (!PlausiblePtr(mgr))
                        return;
                    using ObjFromEntityFn = LPVOID*(__fastcall*)(LPVOID, LPVOID*);
                    using StartFn = void(__fastcall*)(LPVOID, LPVOID, int);
                    using FinishFn = void(__fastcall*)(LPVOID, LPVOID);
                    LPVOID out[2] = { nullptr, nullptr };
                    reinterpret_cast<ObjFromEntityFn>(s.base + kObjectFromEntityRva)(entity, out);
                    if (PlausiblePtr(out[0]))
                    {
                        if (ms > 0)
                            reinterpret_cast<StartFn>(s.base + kStartWorkingRva)(mgr, out[0], ms);
                        else
                            reinterpret_cast<FinishFn>(s.base + kFinishWorkingRva)(mgr, out[0]);
                    }
                    ReleaseRef(out);
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                }
            }
            // ----------------------------------------------------------------- //
            // Persistence: one line per batch, "<gardenId> <growRecipe> <readyAtUnix>".
            void SaveLocked()
            {
                State& s = S();
                const std::string tmp = s.file + ".tmp";
                std::FILE* f = nullptr;
                if (fopen_s(&f, tmp.c_str(), "w") != 0 || !f)
                {
                    FileLog("ERROR: can't write %s", tmp.c_str());
                    return;
                }
                for (const auto& kv : s.batches)
                    std::fprintf(f, "%llu %d %lld %d\n", static_cast<unsigned long long>(kv.first),
                                 kv.second.growRecipe, static_cast<long long>(kv.second.readyAt), kv.second.plantQuality);
                std::fclose(f);
                std::remove(s.file.c_str());
                if (std::rename(tmp.c_str(), s.file.c_str()) != 0)
                    FileLog("ERROR: can't replace %s", s.file.c_str());
            }

            void LoadLocked()
            {
                State& s = S();
                s.batches.clear();
                std::FILE* f = nullptr;
                if (fopen_s(&f, s.file.c_str(), "r") != 0 || !f)
                    return;
                char line[160];
                while (std::fgets(line, sizeof(line), f))
                {
                    unsigned long long id = 0;
                    int recipe = 0;
                    long long ready = 0;
                    int quality = -1;
                    // Older files have three fields per line; the plant quality was added later.
                    const int n = std::sscanf(line, "%llu %d %lld %d", &id, &recipe, &ready, &quality);
                    if (n < 3)
                        continue;
                    Batch b;
                    b.growRecipe = recipe;
                    b.readyAt = ready;
                    b.plantQuality = (n >= 4) ? quality : -1;
                    s.batches[id] = b;
                }
                std::fclose(f);
            }
        }

        // --------------------------------------------------------------------- //
        bool HerbGateSuppressesWorkshopBuff()
        {
            return t_suppressBuff;
        }

        // --------------------------------------------------------------------- //
        U64 __fastcall OnCraftWithDevice(LPVOID self, LPVOID* ctx, U8 isDevice, U64 extra)
        {
            State& s = S();
            if (!s.enabled || ctx == nullptr)
                return _Creation_CraftWithDevice(self, ctx, isDevice, extra);

            if (s.debug && s.debugBudget > 0)
            {
                unsigned long long w[13] = {};
                int dbgAbility = 0;
                unsigned char dbgFlag = 0;
                bool gateAbility = false;
                if (DumpCtxRaw(ctx, w, dbgAbility, dbgFlag))
                {
                    for (const Gate& g : s.gates)
                        if (g.abilityId == dbgAbility)
                            gateAbility = true;
                    if (gateAbility || dbgAbility == -1)
                    {
                        --s.debugBudget;
                        FileLog("DEBUG craftWithDevice isDevice=%u ability=%d flag44=%u recipe(ctx[8] low32)=%d",
                                static_cast<unsigned>(isDevice), dbgAbility, static_cast<unsigned>(dbgFlag),
                                static_cast<int>(w[8] & 0xFFFFFFFFull));
                        FileLog("DEBUG ctx[0..3]=%llx %llx %llx %llx", w[0], w[1], w[2], w[3]);
                        FileLog("DEBUG ctx[4..7]=%llx %llx %llx %llx", w[4], w[5], w[6], w[7]);
                        FileLog("DEBUG ctx[8..12]=%llx %llx %llx %llx %llx", w[8], w[9], w[10], w[11], w[12]);
                    }
                }
            }

            // abilityId / recipeId are filled before ReadCtxRaw's later (entity) checks can fail.
            CtxInfo info;
            const bool ctxOk = ReadCtxRaw(ctx, info);

            const Gate* gp = nullptr;
            for (const Gate& cand : s.gates)
            {
                if (cand.abilityId != info.abilityId)
                    continue;
                if ((info.recipeId >= cand.firstGrowRecipe && info.recipeId <= cand.lastGrowRecipe)
                    || (cand.collectAnyRecipe != 0 && info.recipeId == cand.collectAnyRecipe))
                {
                    gp = &cand;
                    break;
                }
            }
            if (gp == nullptr)
                return _Creation_CraftWithDevice(self, ctx, isDevice, extra);

            if (!ctxOk)
            {
                if (s.debug && s.debugBudget > 0)
                {
                    --s.debugBudget;
                    FileLog("DEBUG gate skipped: context unreadable (recipeSelected=%d recipeId=%d garden=%llu conn=%p)",
                            info.recipeSelected ? 1 : 0, info.recipeId,
                            static_cast<unsigned long long>(info.gardenId), info.conn);
                }
                return _Creation_CraftWithDevice(self, ctx, isDevice, extra);
            }

            const Gate& g = *gp;
            SuppressBuffScope buffScope(g.noWorkshopBuff);
            const bool isGrow = info.recipeId >= g.firstGrowRecipe && info.recipeId <= g.lastGrowRecipe;
            const bool isCollectAny = g.collectAnyRecipe != 0 && info.recipeId == g.collectAnyRecipe;

            if (s.debug && s.debugBudget > 0)
            {
                --s.debugBudget;
                FileLog("DEBUG gate engaged: garden=0x%llx char=%u recipe=%d grow=%d collectAny=%d",
                        static_cast<unsigned long long>(info.gardenId), info.charId, info.recipeId,
                        isGrow ? 1 : 0, isCollectAny ? 1 : 0);
            }

            const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));

            std::unique_lock<std::mutex> lock(s.mutex);
            auto it = s.batches.find(info.gardenId);

            // ---- nothing planted here ----
            if (it == s.batches.end() && isCollectAny)
            {
                lock.unlock();
                SendMessageToPlayer(info.conn, g.emptyMessageId);
                if (s.verbose)
                    FileLog("COLLECT with nothing planted garden=%llu char=%u",
                            static_cast<unsigned long long>(info.gardenId), info.charId);
                return 0;
            }

            // ---- instant gate: a grow recipe is a direct craft (output at once, quality curve applied) ----
            if (it == s.batches.end() && g.instant && isGrow)
            {
                lock.unlock();
                t_curveGate = &g;
                const U64 directRet = _Creation_CraftWithDevice(self, ctx, isDevice, extra);
                t_curveGate = nullptr;
                if (s.verbose)
                    FileLog("DIRECT craft object=%llu char=%u recipe=%d ok=%d",
                            static_cast<unsigned long long>(info.gardenId), info.charId, info.recipeId,
                            (directRet & 0xFF) != 0 ? 1 : 0);
                return directRet;
            }

            // ---- nothing planted here: a grow craft plants ----
            if (it == s.batches.end())
            {
                lock.unlock();
                t_plantQuality = -1;
                t_plantMode = true;
                const U64 ret = _Creation_CraftWithDevice(self, ctx, isDevice, extra);
                t_plantMode = false;

                if ((ret & 0xFF) != 0)
                {
                    std::lock_guard<std::mutex> relock(s.mutex);
                    Batch b;
                    b.growRecipe = info.recipeId;
                    b.readyAt = now + g.growSeconds;
                    b.plantQuality = t_plantQuality;
                    s.batches[info.gardenId] = b;
                    SaveLocked();
                    if (g.workingState)
                        SetWorkingState(ctx, static_cast<int>(g.growSeconds * 1000));
                    if (g.timeMessageBase != 0)
                        SendMessageToPlayer(info.conn, TimeLeftMessageId(g, g.growSeconds));
                    if (s.verbose)
                        FileLog("PLANT garden=%llu char=%u recipe=%d quality=%d ready in %lld s",
                                static_cast<unsigned long long>(info.gardenId), info.charId,
                                info.recipeId, t_plantQuality, static_cast<long long>(g.growSeconds));
                }
                else if (s.verbose)
                {
                    FileLog("plant craft failed (ingredients missing?) garden=%llu char=%u recipe=%d",
                            static_cast<unsigned long long>(info.gardenId), info.charId, info.recipeId);
                }
                return ret;
            }

            // ---- something is growing here ----
            const Batch batch = it->second;
            if (now < batch.readyAt)
            {
                lock.unlock();
                SendMessageToPlayer(info.conn, TimeLeftMessageId(g, batch.readyAt - now));
                if (s.verbose)
                    FileLog("NOT READY garden=%llu char=%u %lld s left",
                            static_cast<unsigned long long>(info.gardenId), info.charId,
                            static_cast<long long>(batch.readyAt - now));
                return 0;
            }

            // ---- ready: collect ----
            lock.unlock();
            const int collectRecipe = batch.growRecipe + g.collectOffset;
            if (!WriteRecipeId(ctx, collectRecipe))
                return _Creation_CraftWithDevice(self, ctx, isDevice, extra);

            int collectQuality = g.outputQuality;
            if (g.curveEnd > 0 && batch.plantQuality >= 0)
                collectQuality = CurveQuality(g, batch.plantQuality);
            t_forceQuality = collectQuality;
            const U64 ret = _Creation_CraftWithDevice(self, ctx, isDevice, extra);
            t_forceQuality = 0;
            WriteRecipeId(ctx, info.recipeId);   // restore the client's own selection

            if ((ret & 0xFF) != 0)
            {
                std::lock_guard<std::mutex> relock(s.mutex);
                s.batches.erase(info.gardenId);
                SaveLocked();
                if (g.workingState)
                    SetWorkingState(ctx, 0);
                if (s.verbose)
                    FileLog("COLLECT garden=%llu char=%u recipe=%d -> collect recipe %d (plant quality %d -> output quality %d)",
                            static_cast<unsigned long long>(info.gardenId), info.charId,
                            info.recipeId, collectRecipe, batch.plantQuality, collectQuality);
            }
            else
            {
                FileLog("collect craft FAILED garden=%llu char=%u collect recipe %d (batch kept)",
                        static_cast<unsigned long long>(info.gardenId), info.charId, collectRecipe);
            }
            return ret;
        }
        // --------------------------------------------------------------------- //
        namespace
        {
            // Ability id (ability+8), recipe id (ctx+0x40) read under SEH.
            bool ReadDurationKey(LPVOID ability, LPVOID ctx, int& abilityId, int& recipeId)
            {
                __try
                {
                    if (!PlausiblePtr(ability) || !PlausiblePtr(ctx))
                        return false;
                    abilityId = *reinterpret_cast<const int*>(static_cast<const unsigned char*>(ability) + 8);
                    recipeId = *reinterpret_cast<const int*>(static_cast<const unsigned char*>(ctx) + 0x40);
                    return true;
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                    return false;
                }
            }
        }

        // Ability::GetDuration override: the progress bar of the instant gate's recipes lasts durationSeconds
        // (also for GM characters, whose normal bar is a flat 1 s).
        U64 __fastcall OnAbilityGetDuration(LPVOID ability, LPVOID ctx)
        {
            const U64 orig = _Ability_GetDuration(ability, ctx);
            State& s = S();
            if (!s.enabled)
                return orig;
            int aid = 0, rid = 0;
            if (!ReadDurationKey(ability, ctx, aid, rid))
                return orig;
            for (const Gate& g : s.gates)
            {
                if (g.durationSeconds > 0 && g.instant && g.abilityId == aid
                    && rid >= g.firstGrowRecipe && rid <= g.lastGrowRecipe)
                {
                    static long logged = 0;
                    if (s.verbose && logged < 30)
                    {
                        ++logged;
                        FileLog("duration: ability %d recipe %d: %llu ms -> %d ms", aid, rid,
                                static_cast<unsigned long long>(orig), g.durationSeconds * 1000);
                    }
                    return static_cast<U64>(g.durationSeconds) * 1000ull;
                }
            }
            return orig;
        }

        // --------------------------------------------------------------------- //
        U64 __fastcall OnFinallyMakeItem(
            LPVOID self, LPVOID conn, LPVOID* recipeRef, LPVOID* itemRef,
            LPVOID resultType, U32 quantity, U16 quality, U8 exceptional,
            int abilityId, int arg10)
        {
            State& s = S();
            if (t_curveGate != nullptr && s.enabled)
            {
                const Gate& cg = *static_cast<const Gate*>(t_curveGate);
                const int q0 = static_cast<int>(quality);
                int q1 = q0;
                if (cg.curveEnd > 0)
                    q1 = CurveQuality(cg, q0);
                else if (cg.outputQuality > 0)
                    q1 = cg.outputQuality;
                if (s.verbose)
                    FileLog("direct: output quality %d -> %d (qty %u, ability %d)", q0, q1,
                            static_cast<unsigned>(quantity), abilityId);
                quality = static_cast<U16>(q1);
            }
            if (t_forceQuality > 0 && s.enabled)
            {
                if (s.verbose)
                    FileLog("wash: output quality %u -> %d (qty %u, ability %d)",
                            static_cast<unsigned>(quality), t_forceQuality,
                            static_cast<unsigned>(quantity), abilityId);
                quality = static_cast<U16>(t_forceQuality);
            }
            if (t_plantMode && s.enabled)
            {
                t_plantQuality = static_cast<int>(quality);
                // Mirror the original's success tail: drop the two shared references,
                // report success, and create nothing.
                ReleaseRef(recipeRef);
                ReleaseRef(itemRef);
                if (s.verbose)
                    FileLog("plant: item creation suppressed (qty %u)", static_cast<unsigned>(quantity));
                return 1;
            }
            return _Creation_FinallyMakeItem(self, conn, recipeRef, itemRef, resultType,
                                             quantity, quality, exceptional, abilityId, arg10);
        }

        // --------------------------------------------------------------------- //
        bool ConfigureHerbGardenGate(const tinyxml2::XMLElement* root)
        {
            State& s = S();
            if (s.attached)
            {
                Lifx::ShowErrorMessage("Can't configure herbGardenGate while the hooks are attached.");
                return false;
            }

            s.enabled = false;
            if (root == nullptr)
                return true;

            const tinyxml2::XMLElement* e = root->FirstChildElement("herbGardenGate");
            if (e == nullptr || !e->BoolAttribute("enabled", false))
                return true;

            s.verbose = e->BoolAttribute("verbose", true);
            s.debug = e->BoolAttribute("debug", false);
            if (const char* v = e->Attribute("file")) s.file = v;
            s.gates.clear();

            // The element's own attributes define the first gate (the herb garden); each <gate> child adds another.
            auto parseGate = [](const tinyxml2::XMLElement* el, Gate& g) -> bool
            {
                g.abilityId = el->IntAttribute("abilityId", g.abilityId);
                g.growSeconds = el->Int64Attribute("growSeconds", g.growSeconds);
                g.firstGrowRecipe = el->IntAttribute("firstGrowRecipe", g.firstGrowRecipe);
                g.lastGrowRecipe = el->IntAttribute("lastGrowRecipe", g.lastGrowRecipe);
                g.collectOffset = el->IntAttribute("collectOffset", g.collectOffset);
                g.pendingMessageId = el->UnsignedAttribute("pendingMessageId", g.pendingMessageId);
                g.emptyMessageId = el->UnsignedAttribute("emptyMessageId", g.emptyMessageId);
                g.collectAnyRecipe = el->IntAttribute("collectAnyRecipe", g.collectAnyRecipe);
                g.timeMessageBase = el->UnsignedAttribute("timeMessageBase", g.timeMessageBase);
                g.timeMessageMax = el->UnsignedAttribute("timeMessageMax", g.timeMessageMax);
                g.outputQuality = el->IntAttribute("outputQuality", g.outputQuality);
                g.curveEnd = el->IntAttribute("curveEnd", g.curveEnd);
                g.curveGainStart = el->DoubleAttribute("curveGainStart", g.curveGainStart);
                g.curveGainEnd = el->DoubleAttribute("curveGainEnd", g.curveGainEnd);
                g.workingState = el->BoolAttribute("workingState", g.workingState);
                g.noWorkshopBuff = el->BoolAttribute("noWorkshopBuff", g.noWorkshopBuff);
                g.instant = el->BoolAttribute("instant", g.instant);
                g.durationSeconds = el->IntAttribute("durationSeconds", g.durationSeconds);
                return g.abilityId > 0 && g.growSeconds >= 0 && g.growSeconds <= 60ll * 60 * 24 * 30
                    && g.firstGrowRecipe > 0 && g.lastGrowRecipe >= g.firstGrowRecipe && g.collectOffset > 0
                    && g.timeMessageMax >= 1 && g.timeMessageMax <= 53 && g.outputQuality >= 0 && g.outputQuality <= 100
                    && g.curveEnd >= 0 && g.curveEnd < 100 && g.curveGainEnd >= 0.0 && g.curveGainStart >= g.curveGainEnd
                    && g.durationSeconds >= 0 && g.durationSeconds <= 3600;
            };

            Gate first;
            if (!parseGate(e, first))
            {
                Lifx::ShowErrorMessage("Invalid <herbGardenGate> attributes.");
                return false;
            }
            s.gates.push_back(first);
            for (const tinyxml2::XMLElement* c = e->FirstChildElement("gate"); c != nullptr; c = c->NextSiblingElement("gate"))
            {
                Gate g;
                g.collectAnyRecipe = 0; g.timeMessageBase = 0; g.outputQuality = 0;
                g.pendingMessageId = 3632; g.emptyMessageId = 1230;
                if (!parseGate(c, g))
                {
                    Lifx::ShowErrorMessage("Invalid <herbGardenGate><gate> attributes.");
                    s.gates.clear();
                    return false;
                }
                s.gates.push_back(g);
            }
            {
                std::lock_guard<std::mutex> lock(s.mutex);
                LoadLocked();
            }
            s.enabled = true;
            return true;
        }

        // --------------------------------------------------------------------- //
        void AttachHerbGardenGateHooks()
        {
            State& s = S();
            if (!s.enabled || s.attached)
                return;

            s.base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
            if (s.base == 0)
            {
                Lifx::ShowErrorMessage("Can't attach herb-garden-gate hooks: main module base is null.");
                s.enabled = false;
                return;
            }

            // mov [rsp+20h],r9; mov [rsp+8],rcx; push rbp/rsi/rdi/r12-r15; lea rbp,[rsp-6150h]; mov eax,6260h
            static constexpr std::uint8_t craftPrologue[] = {
                0x4C, 0x89, 0x4C, 0x24, 0x20, 0x48, 0x89, 0x4C, 0x24, 0x08,
                0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
                0x48, 0x8D, 0xAC, 0x24, 0xB0, 0x9E, 0xFF, 0xFF, 0xB8, 0x60, 0x62, 0x00, 0x00
            };
            // mov rax,rsp; mov [rax+20h],r9; mov [rax+18h],r8; mov [rax+10h],rdx; push rbp/rsi/rdi/r12-r15
            static constexpr std::uint8_t makeItemPrologue[] = {
                0x48, 0x8B, 0xC4, 0x4C, 0x89, 0x48, 0x20, 0x4C, 0x89, 0x40, 0x18, 0x48, 0x89, 0x50, 0x10,
                0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57
            };
            // push rdi ; sub rsp,50h  (message helper)
            static constexpr std::uint8_t msgPrologue[] = { 0x40, 0x57, 0x48, 0x83, 0xEC, 0x50 };
            // push rbx ; sub rsp,20h ; mov rbx,[rcx+8] (shared-reference release)
            static constexpr std::uint8_t releasePrologue[] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x59, 0x08 };

            // Working-container helpers (only needed by gates with workingState=1).
            static constexpr std::uint8_t objFromEntityPrologue[] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B, 0xFA };
            static constexpr std::uint8_t startWorkingPrologue[] = { 0x48, 0x85, 0xD2, 0x74, 0x65, 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57 };
            static constexpr std::uint8_t finishWorkingPrologue[] = { 0x48, 0x85, 0xD2, 0x74, 0x63, 0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20 };
            bool needWorking = false;
            for (const Gate& g : s.gates)
                if (g.workingState)
                    needWorking = true;
            if (needWorking
                && (!MatchBytes(s.base + kObjectFromEntityRva, objFromEntityPrologue, sizeof(objFromEntityPrologue))
                    || !MatchBytes(s.base + kStartWorkingRva, startWorkingPrologue, sizeof(startWorkingPrologue))
                    || !MatchBytes(s.base + kFinishWorkingRva, finishWorkingPrologue, sizeof(finishWorkingPrologue))))
            {
                Lifx::ShowErrorMessage("Can't attach herb-garden-gate hooks: working-container helpers not found (different server build?).");
                s.enabled = false;
                return;
            }

            const std::uintptr_t craft = s.base + kCraftWithDeviceRva;
            const std::uintptr_t make = s.base + kFinallyMakeItemRva;
            if (!MatchBytes(craft, craftPrologue, sizeof(craftPrologue))
                || !MatchBytes(make, makeItemPrologue, sizeof(makeItemPrologue))
                || !MatchBytes(s.base + kSendMessageRva, msgPrologue, sizeof(msgPrologue))
                || !MatchBytes(s.base + kReleaseRefRva, releasePrologue, sizeof(releasePrologue)))
            {
                Lifx::ShowErrorMessage(
                    "Can't attach herb-garden-gate hooks: expected code not found (different server build?).");
                s.enabled = false;
                return;
            }

            _Creation_CraftWithDevice = reinterpret_cast<_Creation_CraftWithDevice_Fn>(craft);
            _Creation_FinallyMakeItem = reinterpret_cast<_Creation_FinallyMakeItem_Fn>(make);

            LONG rc = DetourAttach(&(PVOID&)_Creation_CraftWithDevice, OnCraftWithDevice);
            if (rc != NO_ERROR)
            {
                Lifx::ShowErrorMessage("DetourAttach failed for herb-garden-gate craftWithDevice hook. Error: %ld", rc);
                s.enabled = false;
                return;
            }
            rc = DetourAttach(&(PVOID&)_Creation_FinallyMakeItem, OnFinallyMakeItem);
            if (rc != NO_ERROR)
            {
                Lifx::ShowErrorMessage("DetourAttach failed for herb-garden-gate _finallyMakeItem hook. Error: %ld", rc);
                s.enabled = false;
                return;
            }

            // Progress-bar length override (only when a gate asks for one).
            bool needDuration = false;
            for (const Gate& g : s.gates)
                if (g.instant && g.durationSeconds > 0)
                    needDuration = true;
            if (needDuration)
            {
                // push rbp-ish prologue of Ability::GetDuration: rex push rbp/rbx/rsi/rdi/r14; lea rbp,[rsp+40h]
                static constexpr std::uint8_t durationPrologue[] = { 0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x56, 0x48, 0x8D, 0xAC, 0x24, 0x40 };
                const std::uintptr_t dur = s.base + kGetDurationRva;
                if (!MatchBytes(dur, durationPrologue, sizeof(durationPrologue)))
                {
                    Lifx::ShowErrorMessage("Can't attach herb-garden-gate duration hook: expected code not found (different server build?).");
                }
                else
                {
                    _Ability_GetDuration = reinterpret_cast<_Ability_GetDuration_Fn>(dur);
                    rc = DetourAttach(&(PVOID&)_Ability_GetDuration, OnAbilityGetDuration);
                    if (rc != NO_ERROR)
                        Lifx::ShowErrorMessage("DetourAttach failed for herb-garden-gate GetDuration hook. Error: %ld", rc);
                }
            }

            s.attached = true;
            FileLog("hooks attached (craftWithDevice RVA 0x%X, _finallyMakeItem RVA 0x%X); %u gate(s), %u batch(es) restored",
                    static_cast<unsigned>(kCraftWithDeviceRva), static_cast<unsigned>(kFinallyMakeItemRva),
                    static_cast<unsigned>(s.gates.size()), static_cast<unsigned>(s.batches.size()));
            for (const Gate& g : s.gates)
                FileLog("  gate: ability %d, recipes %d-%d, collect offset %d, collect-any %d, time %lld s, output quality %d, curve end %d (+%.1f..+%.1f), instant %d, bar %d s",
                        g.abilityId, g.firstGrowRecipe, g.lastGrowRecipe, g.collectOffset, g.collectAnyRecipe,
                        static_cast<long long>(g.growSeconds), g.outputQuality, g.curveEnd, g.curveGainStart, g.curveGainEnd,
                        g.instant ? 1 : 0, g.durationSeconds);
        }
    }
}
