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

#include "lifx_geo.h"
#include "server/cm_server.h"

#include <Windows.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace
{
	constexpr std::uintptr_t kSnapToCellRva = 0x58B2F0;   // float* (float* out3, float* in3)
	constexpr std::uintptr_t kWorldToGeoIdRva = 0x57AAE0; // uint32* Geo::WorldToGeoId(uint32* out, float* posXY)
	// _fnTestRaiseTerrainimpl passes the quality of the raised layer as the literal 100: `mov byte ptr [rsp+28h], 64h` (C6 44 24 28 64)
	constexpr std::uintptr_t kRaiseQualityInsnRva = 0x3AFE10;

	using SnapToCellFn = float* (__fastcall*)(float* out, float* in);
	using WorldToGeoIdFn = std::uint32_t* (__fastcall*)(std::uint32_t* out, float* in);

	std::uintptr_t g_base = 0;
	bool g_ok = false;
	bool g_qualityOk = false;

	// Lifx::setTerrainQuality(q 0..100): sets the quality TestRaiseTerrain gives the layer it raises (engine default 100). Returns q, or -1.
	S32 SetTerrainQuality(LPVOID /*obj*/, S32 argc, const char* argv[])
	{
		if (!g_qualityOk || argc < 2 || argv[1] == nullptr)
			return -1;
		const int q = std::atoi(argv[1]);
		if (q < 0 || q > 100)
			return -1;
		std::uint8_t* insn = reinterpret_cast<std::uint8_t*>(g_base + kRaiseQualityInsnRva);
		DWORD old = 0;
		if (!VirtualProtect(insn, 5, PAGE_EXECUTE_READWRITE, &old))
			return -1;
		insn[4] = static_cast<std::uint8_t>(q);
		DWORD tmp = 0;
		VirtualProtect(insn, 5, old, &tmp);
		FlushInstructionCache(GetCurrentProcess(), insn, 5);
		return q;
	}

	S32 WorldToGeoId(LPVOID /*obj*/, S32 argc, const char* argv[])
	{
		if (!g_ok || argc < 3 || argv[1] == nullptr || argv[2] == nullptr)
			return 0;
		float in[3] = { static_cast<float>(std::atof(argv[1])), static_cast<float>(std::atof(argv[2])), 0.0f };
		float cell[3] = { 0.0f, 0.0f, 0.0f };
		std::uint32_t out[2] = { 0, 0 };
		__try
		{
			reinterpret_cast<SnapToCellFn>(g_base + kSnapToCellRva)(cell, in);
			reinterpret_cast<WorldToGeoIdFn>(g_base + kWorldToGeoIdRva)(out, cell);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return 0;
		}
		return static_cast<S32>(out[0]);
	}
}

void Lifx::Api::Geo::Register()
{
	g_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
	static constexpr std::uint8_t snapPrologue[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57 };
	static constexpr std::uint8_t geoPrologue[] = { 0x40, 0x53, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x40 };
	static constexpr std::uint8_t qualityInsn[] = { 0xC6, 0x44, 0x24, 0x28 };   // mov byte ptr [rsp+28h], imm8 (the imm8 is patched)
	if (g_base != 0 && std::memcmp(reinterpret_cast<const void*>(g_base + kRaiseQualityInsnRva), qualityInsn, sizeof(qualityInsn)) == 0)
	{
		g_qualityOk = true;
		Con::AddCommand("Lifx", "setTerrainQuality", &SetTerrainQuality,
		                "(int quality 0..100) - quality of the layer TestRaiseTerrain raises (engine default 100). Returns the value set, or -1",
		                2, 2);
	}
	else
	{
		Con::Warning("Lifx::setTerrainQuality not registered: expected engine code not found (different server build?)");
	}
	if (g_base == 0
	    || std::memcmp(reinterpret_cast<const void*>(g_base + kSnapToCellRva), snapPrologue, sizeof(snapPrologue)) != 0
	    || std::memcmp(reinterpret_cast<const void*>(g_base + kWorldToGeoIdRva), geoPrologue, sizeof(geoPrologue)) != 0)
	{
		Con::Warning("Lifx::worldToGeoId not registered: expected engine code not found (different server build?)");
		return;
	}
	g_ok = true;
	Con::AddCommand("Lifx", "worldToGeoId", &WorldToGeoId,
	                "(float x, float y) - returns the raw GeoID of the map cell at that world position (0 if it can't be resolved)",
	                3, 3);
}