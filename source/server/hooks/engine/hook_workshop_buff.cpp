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
    Workshop crafting-quality buff. See hook_workshop_buff.h for the design.

    Engine facts this relies on (ddctd_cm_yo_server.exe, verified by decompilation):
      - checkExceptionalChance @ 0x1E46B0 is called by craftWithTool (0x1E5820) and
        craftWithDevice (0x1E4B10) right before _finallyMakeItem, with the Player
        object (found by char id, ShapeBase type-mask bit 0x4000 at +0x10) and a
        writable U16 quality. The caller clamps the final quality to 1..100.
      - Player::_checkSteps (player.cpp) keeps the current geoId at Player+0x2464.
*  =================================================================================== */

#include "hook_workshop_buff.h"
#include "hook_herb_garden_gate.h"

#include "core/tinyxml2.h"
#include "server/api/t3d_console.h"

#include <Windows.h>
#include <detours/detours.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

__CM_INSTATNTIATE(_Creation_CheckExceptionalChance);

namespace Hooks
{
    namespace Engine
    {
        namespace
        {
            constexpr std::uintptr_t kCheckExceptionalChanceRva = 0x1E46B0;

            constexpr std::size_t kPlayerTypeMaskOffset = 0x10;
            constexpr U32 kShapeBaseTypeBit = 0x4000;
            // checkExceptionalChance's 3rd argument is the crafter's GameConnection, NOT the Player.
            // GameConnection::getControlObject (RVA 0x135C00) returns **(conn + 0xB38).
            constexpr std::size_t kConnControlObjectOffset = 0xB38;
            constexpr std::size_t kPlayerGeoIdOffset = 0x2464;   // cached by Player::_checkSteps
            constexpr std::size_t kPlayerCharIdOffset = 0x1B44;  // char id stamp, read by _checkSteps
            // World position of a Player (translation of its transform), same offsets the
            // AI hook uses in production for ScanNearestPlayer.
            constexpr std::size_t kPlayerPosXOffset = 0x284;
            constexpr std::size_t kPlayerPosYOffset = 0x294;
            constexpr std::size_t kPlayerPosZOffset = 0x2A4;
            // Geo::WorldToGeoId(U32* outGeoId, const float* worldPos /*x,y,z*/)
            constexpr std::uintptr_t kWorldToGeoIdRva = 0x57AAE0;

            constexpr U32 kTilesPerRow = 512;
            constexpr U32 kTilesPerBlock = 512u * 512u;

            struct WorkshopRule
            {
                U32 objectTypeId = 0;
                float radiusTiles = 6.0f;
                float multiplier = 1.2f;
                std::vector<int> abilityIds;
            };

            struct PlacedWorkshop
            {
                U32 objectTypeId = 0;
                U32 geoId = 0;
            };

            using Snapshot = std::shared_ptr<const std::vector<PlacedWorkshop>>;

            struct State
            {
                bool enabled = false;
                bool attached = false;
                bool verbose = true;
                bool debug = false;
                unsigned refreshSeconds = 30;

                std::string dbHost = "127.0.0.1";
                unsigned dbPort = 3306;
                std::string dbUser;
                std::string dbPassword;
                std::string dbName = "lif_1";

                std::vector<WorkshopRule> rules;

                std::mutex snapshotMutex;
                Snapshot snapshot;

                std::mutex logMutex;
            };

            // Intentionally leaked: the poller thread and hook may outlive static
            // destruction at process exit.
            State& S()
            {
                static State* state = new State();
                return *state;
            }

            // ----------------------------------------------------------------- //
            void FileLog(const char* fmt, ...)
            {
                State& s = S();
                char msg[600];
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
                if (fopen_s(&f, "logs/workshop_buff.log", "a") == 0 && f)
                {
                    std::fprintf(f, "%s %s\n", stamp, msg);
                    std::fclose(f);
                }
            }

            // ----------------------------------------------------------------- //
            // libmariadb (already loaded by the server) bound dynamically so no
            // headers or import library are needed.
            struct MariaApi
            {
                void* (__cdecl* init)(void*) = nullptr;
                void* (__cdecl* realConnect)(void*, const char*, const char*, const char*,
                                             const char*, unsigned int, const char*,
                                             unsigned long) = nullptr;
                int (__cdecl* query)(void*, const char*) = nullptr;
                void* (__cdecl* storeResult)(void*) = nullptr;
                char** (__cdecl* fetchRow)(void*) = nullptr;
                void (__cdecl* freeResult)(void*) = nullptr;
                void (__cdecl* close)(void*) = nullptr;
                const char* (__cdecl* error)(void*) = nullptr;
                int (__cdecl* options)(void*, int, const void*) = nullptr;
                int (__cdecl* threadInit)() = nullptr;
            };

            bool LoadMariaApi(MariaApi& api)
            {
                HMODULE lib = GetModuleHandleA("libmariadb.dll");
                if (!lib)
                    lib = LoadLibraryA("libmariadb.dll");
                if (!lib)
                    return false;

                auto get = [&](const char* name) { return GetProcAddress(lib, name); };
                api.init = reinterpret_cast<decltype(api.init)>(get("mysql_init"));
                api.realConnect = reinterpret_cast<decltype(api.realConnect)>(get("mysql_real_connect"));
                api.query = reinterpret_cast<decltype(api.query)>(get("mysql_query"));
                api.storeResult = reinterpret_cast<decltype(api.storeResult)>(get("mysql_store_result"));
                api.fetchRow = reinterpret_cast<decltype(api.fetchRow)>(get("mysql_fetch_row"));
                api.freeResult = reinterpret_cast<decltype(api.freeResult)>(get("mysql_free_result"));
                api.close = reinterpret_cast<decltype(api.close)>(get("mysql_close"));
                api.error = reinterpret_cast<decltype(api.error)>(get("mysql_error"));
                api.options = reinterpret_cast<decltype(api.options)>(get("mysql_options"));
                api.threadInit = reinterpret_cast<decltype(api.threadInit)>(get("mysql_thread_init"));

                return api.init && api.realConnect && api.query && api.storeResult
                    && api.fetchRow && api.freeResult && api.close && api.error;
            }

            // ----------------------------------------------------------------- //
            std::string BuildQuery()
            {
                std::ostringstream q;
                q << "SELECT ObjectTypeID, GeoDataID FROM unmovable_objects "
                     "WHERE IsComplete=1 AND ObjectTypeID IN (";
                bool first = true;
                for (const WorkshopRule& r : S().rules)
                {
                    if (!first) q << ",";
                    q << r.objectTypeId;
                    first = false;
                }
                q << ")";
                return q.str();
            }

            // ----------------------------------------------------------------- //
            // Returns true on success. On failure `errorOut` explains why.
            bool PollOnce(const MariaApi& api, std::string& errorOut)
            {
                State& s = S();
                void* conn = api.init(nullptr);
                if (!conn)
                {
                    errorOut = "mysql_init failed";
                    return false;
                }

                if (api.options)
                {
                    const unsigned int timeoutSeconds = 5;
                    api.options(conn, 0 /*MYSQL_OPT_CONNECT_TIMEOUT*/, &timeoutSeconds);
                    api.options(conn, 11 /*MYSQL_OPT_READ_TIMEOUT*/, &timeoutSeconds);
                    api.options(conn, 12 /*MYSQL_OPT_WRITE_TIMEOUT*/, &timeoutSeconds);
                }

                if (!api.realConnect(conn, s.dbHost.c_str(), s.dbUser.c_str(),
                                     s.dbPassword.c_str(), s.dbName.c_str(), s.dbPort,
                                     nullptr, 0))
                {
                    errorOut = std::string("connect: ") + api.error(conn);
                    api.close(conn);
                    return false;
                }

                const std::string sql = BuildQuery();
                if (api.query(conn, sql.c_str()) != 0)
                {
                    errorOut = std::string("query: ") + api.error(conn);
                    api.close(conn);
                    return false;
                }

                void* result = api.storeResult(conn);
                if (!result)
                {
                    errorOut = std::string("store_result: ") + api.error(conn);
                    api.close(conn);
                    return false;
                }

                auto rows = std::make_shared<std::vector<PlacedWorkshop>>();
                while (char** row = api.fetchRow(result))
                {
                    if (!row[0] || !row[1])
                        continue;
                    PlacedWorkshop w;
                    w.objectTypeId = static_cast<U32>(std::strtoul(row[0], nullptr, 10));
                    w.geoId = static_cast<U32>(std::strtoul(row[1], nullptr, 10));
                    if (w.objectTypeId != 0 && w.geoId != 0)
                        rows->push_back(w);
                }

                api.freeResult(result);
                api.close(conn);

                {
                    std::lock_guard<std::mutex> lock(s.snapshotMutex);
                    s.snapshot = rows;
                }
                errorOut.clear();
                return true;
            }

            // ----------------------------------------------------------------- //
            void PollerMain()
            {
                State& s = S();
                MariaApi api;
                if (!LoadMariaApi(api))
                {
                    FileLog("poller: libmariadb.dll or one of its functions is missing; buff disabled");
                    return;
                }
                if (api.threadInit)
                    api.threadInit();

                FileLog("poller: started, refresh every %u s, %zu rule(s)",
                        s.refreshSeconds, s.rules.size());

                bool lastOk = true;
                bool firstRun = true;
                size_t lastCount = static_cast<size_t>(-1);
                for (;;)
                {
                    std::string err;
                    const bool ok = PollOnce(api, err);
                    if (!ok)
                    {
                        if (firstRun || lastOk)
                            FileLog("poller: refresh FAILED (%s); keeping previous data", err.c_str());
                    }
                    else
                    {
                        Snapshot snap;
                        {
                            std::lock_guard<std::mutex> lock(s.snapshotMutex);
                            snap = s.snapshot;
                        }
                        const size_t count = snap ? snap->size() : 0;
                        if (firstRun || !lastOk || count != lastCount)
                            FileLog("poller: %zu placed workshop(s) known", count);
                        lastCount = count;
                    }
                    lastOk = ok;
                    firstRun = false;
                    Sleep(s.refreshSeconds * 1000);
                }
            }

            // ----------------------------------------------------------------- //
            // POD-only so it may contain __try (MSVC C2712 forbids mixing with
            // objects that need unwinding).
            struct PlayerProbe
            {
                LPVOID player = nullptr;
                U32 mask = 0;
                U32 charIdStamp = 0;
                U32 rawAt2464 = 0;
                float x = 0, y = 0, z = 0;
                U32 vtableRva = 0;
                U32 geoFromPos = 0;
                U32 geoId = 0;
            };

            using WorldToGeoIdFn = U32* (__fastcall*)(U32* outGeoId, const float* worldPos);

            // status: 0 ok, 2 access violation, 3 access violation in
            // WorldToGeoId, 4 connection has no control object. POD-only (contains __try).
            int ProbePlayer(LPVOID conn, PlayerProbe* out)
            {
                __try
                {
                    const auto* c = static_cast<const std::uint8_t*>(conn);
                    void** holder = *reinterpret_cast<void***>(const_cast<std::uint8_t*>(c) + kConnControlObjectOffset);
                    if (holder == nullptr || *holder == nullptr)
                        return 4;
                    const auto* base = static_cast<const std::uint8_t*>(*holder);
                    out->player = *holder;
                    out->mask = *reinterpret_cast<const U32*>(base + kPlayerTypeMaskOffset);
                    out->charIdStamp = *reinterpret_cast<const U32*>(base + kPlayerCharIdOffset);
                    out->rawAt2464 = *reinterpret_cast<const U32*>(base + kPlayerGeoIdOffset);
                    out->x = *reinterpret_cast<const float*>(base + kPlayerPosXOffset);
                    out->y = *reinterpret_cast<const float*>(base + kPlayerPosYOffset);
                    out->z = *reinterpret_cast<const float*>(base + kPlayerPosZOffset);
                    const auto vt = *reinterpret_cast<const std::uintptr_t*>(base);
                    out->vtableRva = static_cast<U32>(
                        vt - reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)));
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                    return 2;
                }
                // (The Player carries different flag bits than its connection, so no flag check
                // here; identity is verified by the char-id stamp in the caller.)

                // Prefer the geoId the engine caches on the Player each movement step.
                const U32 rawBlock = out->rawAt2464 / kTilesPerBlock;
                if (out->rawAt2464 != 0 && rawBlock >= 1 && rawBlock <= 4000)
                    out->geoId = out->rawAt2464;

                // Also derive it from the world position with the engine's own converter
                // (used as a fallback and as a cross-check in debug logs).
                __try
                {
                    alignas(16) float pos[4] = { out->x, out->y, out->z, 0.0f };
                    alignas(16) U32 result[4] = { 0, 0, 0, 0 };
                    const auto fn = reinterpret_cast<WorldToGeoIdFn>(
                        reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)) + kWorldToGeoIdRva);
                    fn(result, pos);
                    out->geoFromPos = result[0];
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                    if (out->geoId == 0)
                        return 3;
                }
                if (out->geoId == 0)
                    out->geoId = out->geoFromPos;
                return 0;
            }
            // ----------------------------------------------------------------- //
            const WorkshopRule* FindRule(U32 objectTypeId)
            {
                for (const WorkshopRule& r : S().rules)
                    if (r.objectTypeId == objectTypeId)
                        return &r;
                return nullptr;
            }

            // ----------------------------------------------------------------- //
            void ApplyBuff(U32 charId, LPVOID player, int abilityId, U16* quality)
            {
                State& s = S();

                // Crafts of gates flagged noWorkshopBuff (the Ore Washer) are exempt.
                if (Hooks::Engine::HerbGateSuppressesWorkshopBuff())
                    return;

                PlayerProbe probe;
                const int readStatus = ProbePlayer(player, &probe);
                const U32 geoId = probe.geoId;
                if (s.debug)
                {
                    Con::Echo("[lifx-workshop] probe char=%u conn=%p player=%p vtableRVA=0x%X charStamp=%u pos=(%.1f, %.1f, %.1f) cachedGeo=%u geoFromPos=%u status=%d using=%u (block %u)",
                              charId, player, probe.player, probe.vtableRva, probe.charIdStamp,
                              probe.x, probe.y, probe.z, probe.rawAt2464, probe.geoFromPos,
                              readStatus, geoId, geoId / kTilesPerBlock);
                }
                if (readStatus != 0 || geoId == 0)
                    return;
                if (probe.charIdStamp != charId)
                {
                    if (s.debug)
                        Con::Echo("[lifx-workshop] char id stamp %u does not match crafter %u; skipping", probe.charIdStamp, charId);
                    return;
                }

                Snapshot snap;
                {
                    std::lock_guard<std::mutex> lock(s.snapshotMutex);
                    snap = s.snapshot;
                }
                if (!snap || snap->empty())
                {
                    if (s.debug)
                        Con::Echo("[lifx-workshop] craft char=%u ability=%d: no workshop data loaded yet", charId, abilityId);
                    return;
                }

                const U32 pBlock = geoId / kTilesPerBlock;
                const int pRow = static_cast<int>((geoId % kTilesPerBlock) / kTilesPerRow);
                const int pCol = static_cast<int>(geoId % kTilesPerRow);

                bool found = false;
                float nearestAny = -1.0f;
                float bestMultiplier = 1.0f;
                float bestDistance = 0.0f;
                U32 bestType = 0;

                for (const PlacedWorkshop& w : *snap)
                {
                    const WorkshopRule* rule = FindRule(w.objectTypeId);
                    if (!rule)
                        continue;
                    if (w.geoId / kTilesPerBlock != pBlock)
                        continue;

                    const int wRow = static_cast<int>((w.geoId % kTilesPerBlock) / kTilesPerRow);
                    const int wCol = static_cast<int>(w.geoId % kTilesPerRow);
                    const float dx = static_cast<float>(pCol - wCol);
                    const float dy = static_cast<float>(pRow - wRow);
                    const float distance = std::sqrt(dx * dx + dy * dy);
                    if (nearestAny < 0.0f || distance < nearestAny)
                        nearestAny = distance;

                    if (!rule->abilityIds.empty()
                        && std::find(rule->abilityIds.begin(), rule->abilityIds.end(), abilityId)
                            == rule->abilityIds.end())
                        continue;

                    if (distance <= rule->radiusTiles
                        && (!found || rule->multiplier > bestMultiplier))
                    {
                        found = true;
                        bestMultiplier = rule->multiplier;
                        bestDistance = distance;
                        bestType = w.objectTypeId;
                    }
                }

                if (s.debug)
                {
                    Con::Echo("[lifx-workshop] craft char=%u ability=%d quality=%u geo=%u (block %u) known=%zu nearest=%.1f tiles -> %s",
                              charId, abilityId, static_cast<unsigned>(*quality), geoId, pBlock,
                              snap->size(), nearestAny, found ? "IN RANGE" : "no buff");
                }

                if (!found)
                    return;

                const U16 before = *quality;
                const long scaled = std::lround(static_cast<float>(before) * bestMultiplier);
                const U16 after = static_cast<U16>(std::clamp<long>(scaled, 1, 100));
                if (after == before)
                    return;

                *quality = after;
                if (s.verbose)
                {
                    Con::Echo("[lifx-workshop] char=%u ability=%d quality %u -> %u (x%.2f, workshop type %u at %.1f tiles)",
                              charId, abilityId, static_cast<unsigned>(before),
                              static_cast<unsigned>(after), bestMultiplier, bestType, bestDistance);
                }
            }

            // ----------------------------------------------------------------- //
            std::vector<int> ParseIntList(const char* text)
            {
                std::vector<int> out;
                if (!text)
                    return out;
                const char* p = text;
                while (*p)
                {
                    while (*p && !((*p >= '0' && *p <= '9') || *p == '-'))
                        ++p;
                    if (!*p)
                        break;
                    char* end = nullptr;
                    const long v = std::strtol(p, &end, 10);
                    if (end == p)
                        break;
                    out.push_back(static_cast<int>(v));
                    p = end;
                }
                return out;
            }

            // ----------------------------------------------------------------- //
            bool MatchBytes(std::uintptr_t address, const std::uint8_t* expected, std::size_t size)
            {
                return std::memcmp(reinterpret_cast<const void*>(address), expected, size) == 0;
            }
        }

        // --------------------------------------------------------------------- //
        U8 __fastcall OnCheckExceptionalChance(
            LPVOID self, U32 charId, LPVOID player, U32 skillAmount,
            int abilityId, float chance, LPVOID resultType,
            U16* quality, U8* exceptionalFlag)
        {
            const U8 result = _Creation_CheckExceptionalChance(
                self, charId, player, skillAmount, abilityId, chance,
                resultType, quality, exceptionalFlag);

            if (result && quality != nullptr && S().enabled)
                ApplyBuff(charId, player, abilityId, quality);

            return result;
        }

        // --------------------------------------------------------------------- //
        bool ConfigureWorkshopBuff(const tinyxml2::XMLElement* root)
        {
            State& s = S();
            if (s.attached)
            {
                Lifx::ShowErrorMessage("Can't configure workshopBuff while the hook is attached.");
                return false;
            }

            s.enabled = false;
            s.rules.clear();

            if (root == nullptr)
                return true;

            const tinyxml2::XMLElement* section = root->FirstChildElement("workshopBuff");
            if (section == nullptr || !section->BoolAttribute("enabled", false))
                return true;

            s.verbose = section->BoolAttribute("verbose", true);
            s.debug = section->BoolAttribute("debug", false);
            unsigned refresh = section->UnsignedAttribute("refreshSeconds", 30);
            s.refreshSeconds = std::clamp(refresh, 5u, 3600u);

            const tinyxml2::XMLElement* db = section->FirstChildElement("db");
            if (db == nullptr)
            {
                Lifx::ShowErrorMessage("workshopBuff is enabled but has no <db> element.");
                return false;
            }
            if (const char* v = db->Attribute("host")) s.dbHost = v;
            if (const char* v = db->Attribute("user")) s.dbUser = v;
            if (const char* v = db->Attribute("password")) s.dbPassword = v;
            if (const char* v = db->Attribute("name")) s.dbName = v;
            s.dbPort = db->UnsignedAttribute("port", 3306);
            if (s.dbUser.empty())
            {
                Lifx::ShowErrorMessage("workshopBuff <db> needs a user attribute.");
                return false;
            }

            for (const tinyxml2::XMLElement* w = section->FirstChildElement("workshop");
                 w != nullptr; w = w->NextSiblingElement("workshop"))
            {
                unsigned typeId = 0;
                if (w->QueryUnsignedAttribute("objectTypeId", &typeId) != tinyxml2::XML_SUCCESS
                    || typeId == 0)
                {
                    Lifx::ShowErrorMessage("Invalid <workshopBuff><workshop>: objectTypeId must be a positive integer.");
                    s.rules.clear();
                    return false;
                }

                WorkshopRule rule;
                rule.objectTypeId = typeId;
                rule.radiusTiles = w->FloatAttribute("radiusTiles", 6.0f);
                rule.multiplier = w->FloatAttribute("multiplier", 1.2f);
                rule.abilityIds = ParseIntList(w->Attribute("abilityIds"));

                if (rule.radiusTiles < 0.5f || rule.radiusTiles > 100.0f
                    || rule.multiplier < 0.1f || rule.multiplier > 3.0f)
                {
                    Lifx::ShowErrorMessage(
                        "Invalid <workshop objectTypeId=%u>: radiusTiles must be 0.5..100 and multiplier 0.1..3.0.",
                        typeId);
                    s.rules.clear();
                    return false;
                }
                if (FindRule(typeId) != nullptr)
                {
                    Lifx::ShowErrorMessage("Duplicate <workshop objectTypeId=%u>.", typeId);
                    s.rules.clear();
                    return false;
                }
                s.rules.push_back(std::move(rule));
            }

            if (s.rules.empty())
            {
                Lifx::ShowErrorMessage("workshopBuff is enabled, but no <workshop> entries were provided.");
                return false;
            }

            s.enabled = true;
            return true;
        }

        // --------------------------------------------------------------------- //
        void AttachWorkshopBuffHook()
        {
            State& s = S();
            if (!s.enabled || s.attached)
                return;

            const std::uintptr_t base =
                reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
            if (base == 0)
            {
                Lifx::ShowErrorMessage("Can't attach workshop-buff hook: main module base is null.");
                return;
            }

            // mov rax,rsp; push rdi; push r14; push r15; sub rsp,70h; mov qword [rax-58h],-2
            static constexpr std::uint8_t prologue[] = {
                0x48, 0x8B, 0xC4, 0x57, 0x41, 0x56, 0x41, 0x57,
                0x48, 0x83, 0xEC, 0x70,
                0x48, 0xC7, 0x40, 0xA8, 0xFE, 0xFF, 0xFF, 0xFF
            };

            const std::uintptr_t target = base + kCheckExceptionalChanceRva;
            if (!MatchBytes(target, prologue, sizeof(prologue)))
            {
                Lifx::ShowErrorMessage(
                    "Can't attach workshop-buff hook: checkExceptionalChance prologue not found (different server build?).");
                return;
            }

            _Creation_CheckExceptionalChance =
                reinterpret_cast<_Creation_CheckExceptionalChance_Fn>(target);

            const LONG rc = DetourAttach(
                &(PVOID&)_Creation_CheckExceptionalChance, OnCheckExceptionalChance);
            if (rc != NO_ERROR)
            {
                Lifx::ShowErrorMessage("DetourAttach failed for workshop-buff hook. Error: %ld", rc);
                return;
            }

            s.attached = true;
            FileLog("hook attached at RVA 0x%X", static_cast<unsigned>(kCheckExceptionalChanceRva));
            std::thread(PollerMain).detach();
        }
    }
}
