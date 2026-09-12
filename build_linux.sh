#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# Linux cross-build for LiFx using clang-cl + lld-link + xwin (MSVC ABI).
#
# Prereqs (Arch / CachyOS):
#   sudo pacman -S clang lld llvm cargo
#   cargo install xwin
#   xwin --accept-license splat --output ~/.xwin --include-debug-libs
#
# Then run:
#   ./build_linux.sh                  # builds both DLLs (Release)
#   BUILD=Debug ./build_linux.sh      # debug build
#   ./build_linux.sh loader           # just the pdh.dll proxy
#   ./build_linux.sh lifx             # just the mod DLL (name: $LIFX_DLL_NAME)
# ---------------------------------------------------------------------------
set -euo pipefail

cd "$(dirname "$0")"

XWIN_ROOT="${XWIN_ROOT:-$HOME/.xwin}"
BUILD="${BUILD:-Release}"
OUT="win/build/${BUILD}"
mkdir -p "$OUT"

# Opaque random filename for the mod DLL — the proxy in
# source/loader/pdh_loader.cpp does `LoadLibraryW("<this>")`, so this name and
# the literal in that file MUST stay identical. <TargetName> in
# win/LiFx.vcxproj also tracks this for the Windows build path.
LIFX_DLL_NAME="${LIFX_DLL_NAME:-4ba5cb5e.dll}"

if [[ ! -d "$XWIN_ROOT/sdk" || ! -d "$XWIN_ROOT/crt" ]]; then
    echo "ERROR: xwin splat output not found at $XWIN_ROOT" >&2
    echo "       Run: xwin --accept-license splat --output $XWIN_ROOT --include-debug-libs" >&2
    exit 1
fi

# clang-cl flags shared by both projects, mirroring the vcxproj.
SDK_INC=(
    /imsvc "$XWIN_ROOT/crt/include"
    /imsvc "$XWIN_ROOT/sdk/include/ucrt"
    /imsvc "$XWIN_ROOT/sdk/include/um"
    /imsvc "$XWIN_ROOT/sdk/include/shared"
    /imsvc "$XWIN_ROOT/sdk/include/winrt"
)

SDK_LIBPATHS=(
    "/libpath:$XWIN_ROOT/crt/lib/x86_64"
    "/libpath:$XWIN_ROOT/sdk/lib/ucrt/x86_64"
    "/libpath:$XWIN_ROOT/sdk/lib/um/x86_64"
)

if [[ "$BUILD" == "Debug" ]]; then
    CONFIG_FLAGS=(/MDd /Od /Z7 -D_DEBUG)
    LINK_DEBUG=(/debug)
else
    CONFIG_FLAGS=(/MD /O2 -DNDEBUG)
    LINK_DEBUG=()
fi

COMMON_DEFS=(
    -DWIN32 -D_WINDOWS -D_WIN64
    -D_WIN32_WINNT=0x0A00
    -DWIN32_LEAN_AND_MEAN
    -D_CRT_SECURE_NO_WARNINGS -D_CRT_NONSTDC_NO_WARNINGS
)

COMMON_CFLAGS=(
    -fuse-ld=lld
    -Wno-msvc-not-found
)

target="${1:-all}"

build_loader() {
    echo ">>> Building pdh.dll (LiFx loader)"
    mkdir -p "$OUT/loader_ic"
    clang-cl \
        --target=x86_64-pc-windows-msvc \
        /std:c++20 /EHsc /W3 \
        "${COMMON_CFLAGS[@]}" \
        "${CONFIG_FLAGS[@]}" \
        "${COMMON_DEFS[@]}" \
        "${SDK_INC[@]}" \
        /LD \
        /Fo"$OUT/loader_ic/" /Fe"$OUT/pdh.dll" \
        source/loader/pdh_loader.cpp \
        /link \
        /subsystem:windows /machine:x64 \
        "${SDK_LIBPATHS[@]}" \
        "${LINK_DEBUG[@]}" \
        kernel32.lib
    echo "    -> $OUT/pdh.dll"
}

# The baked LFXE key header (source/core/crypto/lfxe_key_data.h) is generated
# and gitignored. If missing, derive it from config/dts_key.bin, generating a
# fresh key the first time. Both DLLs and the packer (scripts/dts_encrypt.py)
# read the same key, so they always agree. Shared by both build targets.
ensure_baked_key() {
    if [ ! -f source/core/crypto/lfxe_key_data.h ]; then
        if [ -f config/dts_key.bin ]; then
            python3 scripts/gen_baked_key.py --key-in config/dts_key.bin
        else
            python3 scripts/gen_baked_key.py --new --key-out config/dts_key.bin
        fi
    fi
}

# win/LiFx.vcxproj must list the same sources as build_lifx below, or the
# Visual Studio build breaks for Windows users. Warn (don't fail) on drift.
check_vcxproj_sync() {
    python3 scripts/sync_vcxproj.py --check || \
        echo "WARNING: Windows project out of date -- commit the output of the command above" >&2
}

build_lifx() {
    echo ">>> Building $LIFX_DLL_NAME (LiFx)"
    mkdir -p "$OUT/ic"
    ensure_baked_key
    check_vcxproj_sync
    local srcs=(
        source/core/cm_aux.cpp
        source/core/cm_globals.cpp
        source/core/cm_memory_mgr.cpp
        source/core/tinyxml2.cpp
        source/core/crypto/chacha20.cpp
        source/core/crypto/lfxe_decrypt.cpp
        source/core/crypto/lfxe_filestream.cpp
        source/core/crypto/baked_key_provider.cpp
        source/dllmain.cpp
        source/server/api/t3d_console.cpp
        source/server/api/lifx_character.cpp
        source/server/api/lifx_outpost.cpp
        source/server/api/lifx_effects.cpp
        source/server/api/lifx_timers.cpp
        source/server/api/lifx_dispatcher.cpp
        source/server/api/lifx_hostile.cpp
        source/server/api/lifx_battlezone.cpp
        source/server/cm_server.cpp
        source/server/hooks/engine/hook_console.cpp
        source/server/hooks/engine/hook_filestream.cpp
        source/server/hooks/engine/hook_recipe_starting_tools.cpp
        source/server/hooks/engine/hook_gem_drop.cpp
        source/server/hooks/engine/hook_tunnel_drop.cpp
        source/server/hooks/engine/hook_tree_drop.cpp
        source/server/hooks/furnace/hook_proc_desc.cpp
        source/server/hooks/furnace/hook_working_furnace_tick.cpp
        source/server/hooks/furnace/hook_brewing_tank_tick.cpp
        source/server/hooks/furnace/hook_brewing_tank_desc.cpp
        source/server/hooks/furnace/hook_working_fire_tick.cpp
        source/server/hooks/furnace/hook_working_greenhouse_tick.cpp
        source/server/hooks/furnace/hook_working_trap_tick.cpp
        source/server/hooks/furnace/hook_working_windmill_tick.cpp
        source/server/hooks/character/hook_vital_process_tick.cpp
        source/server/hooks/character/hook_calc_hit_damage.cpp
        source/server/hooks/character/hook_wounds_deal_damage.cpp
        source/server/hooks/character/hook_send_changes.cpp
        source/server/hooks/character/hook_apply_damage.cpp
        source/server/hooks/character/hook_onepunchman.cpp
        source/server/hooks/character/hook_set_control_object.cpp
        source/server/hooks/character/hook_npcdec_pack.cpp
        source/server/hooks/character/hook_setanimation.cpp
        source/server/hooks/character/hook_animal_death.cpp
        source/server/hooks/character/hook_animal_create.cpp
        source/server/hooks/character/hook_container_init.cpp
        source/server/hooks/outpost/hook_outpost_default_radius.cpp
        source/server/hooks/outpost/hook_outpost_proximity.cpp
        source/server/hooks/battlezone/hook_battlezone_containment.cpp
        source/server/hooks/effect/hook_effect_parse.cpp
        source/server/hooks/effect/hook_assign_effect.cpp
        source/server/hooks/effect/hook_broadcast_effects.cpp
        source/server/hooks/netevent/hook_netclassrep_dumper.cpp
        source/server/hooks/netevent/sector_handoff_event.cpp
        source/server/hooks/sector/sector_edge_trigger.cpp
        source/server/hooks/sector/world_grid.cpp
        source/server/hooks/sector/client_redirect.cpp
        source/server/hooks/dispatcher/dispatcher_client.cpp
        source/server/hooks/ai/hook_behavior_node.cpp
        source/server/hooks_engine.cpp
    )
    clang-cl \
        --target=x86_64-pc-windows-msvc \
        /std:c++20 /EHsc /W3 \
        "${COMMON_CFLAGS[@]}" \
        "${CONFIG_FLAGS[@]}" \
        "${COMMON_DEFS[@]}" -D_YOUR_OWN_AURORA \
        /I extra/include /I source \
        "${SDK_INC[@]}" \
        /LD \
        /Fo"$OUT/ic/" /Fe"$OUT/$LIFX_DLL_NAME" \
        "${srcs[@]}" \
        /link \
        /subsystem:windows /machine:x64 \
        /libpath:extra/lib \
        "${SDK_LIBPATHS[@]}" \
        "${LINK_DEBUG[@]}" \
        detours.lib kernel32.lib user32.lib psapi.lib ws2_32.lib
    echo "    -> $OUT/$LIFX_DLL_NAME"
}

build_lifx_client() {
    # Output filename is intentionally distinct from the server's so a
    # parallel `all` build doesn't overwrite. The deployer renames it
    # to 4ba5cb5e.dll (same as server) when dropping into the client
    # install dir, because the pdh.dll proxy always looks for that
    # exact name in its own directory.
    local OUTNAME="${LIFX_DLL_NAME%.dll}_client.dll"
    echo ">>> Building $OUTNAME (LiFx client)"
    mkdir -p "$OUT/ic_client"

    ensure_baked_key

    local srcs=(
        source/client/dllmain.cpp
        source/client/client_runtime.cpp
        source/client/hook_console.cpp
        source/client/hook_console_init.cpp
        source/client/hook_filestream.cpp
        source/client/hook_naked_render.cpp
        source/client/hook_equip_unpack.cpp
        source/core/crypto/chacha20.cpp
        source/core/crypto/lfxe_decrypt.cpp
        source/core/crypto/lfxe_filestream.cpp
        source/core/crypto/baked_key_provider.cpp
    )
    clang-cl \
        --target=x86_64-pc-windows-msvc \
        /std:c++20 /EHsc /W3 \
        "${COMMON_CFLAGS[@]}" \
        "${CONFIG_FLAGS[@]}" \
        "${COMMON_DEFS[@]}" -D_LIFX_CLIENT \
        /I extra/include /I source \
        "${SDK_INC[@]}" \
        /LD \
        /Fo"$OUT/ic_client/" /Fe"$OUT/$OUTNAME" \
        "${srcs[@]}" \
        /link \
        /subsystem:windows /machine:x64 \
        /libpath:extra/lib \
        "${SDK_LIBPATHS[@]}" \
        "${LINK_DEBUG[@]}" \
        detours.lib kernel32.lib user32.lib
    echo "    -> $OUT/$OUTNAME"
}

case "$target" in
    loader)        build_loader ;;
    lifx|cm_server) build_lifx ;;
    lifx-client|client) build_lifx_client ;;
    all|"")        build_loader; build_lifx; build_lifx_client ;;
    *) echo "Unknown target: $target (loader | lifx | lifx-client | all)" >&2; exit 2 ;;
esac
