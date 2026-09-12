---
title: Encrypted assets (LFXE)
status: verified
domain: reverse-engineering
tags: [lfxe, encryption, chacha20, filestream, assets]
related: [lfxe_texture_re.md]
updated: 2026-09-12
---

# Encrypted assets (LFXE)

Transparent encryption of loose game assets — shapes (`.dts`), compiled
scripts (`.cs.dso`), and textures (`.dds`/`.png`) — so that **only the LiFx
client/server** can load them. The goal is to stop casual ripping of art
assets (model viewers, copying shapes to another server); it is deliberately
**not** a guarantee against a determined attacker who holds the client DLL.

> **Coverage (issue #116):** as of the universal hook, decryption happens at
> the engine **FileStream** layer, so *every* loose-file asset is covered —
> not just `.dts`. The earlier per-format hooks (`TSShape::read` for shapes,
> the `openFileStream` factory for `.cs.dso`) are **retired**, subsumed by the
> one FileStream hook. See [`lfxe_texture_re.md`](lfxe_texture_re.md) for the
> RE that motivated this.

## Threat model — read this first

The client must decrypt shapes to render them, so the key is, by necessity,
reachable by the client at runtime. Anyone with the client binary can
recover the key and decrypt every asset. Therefore:

- **Achievable (this feature, v1):** a stock DTS viewer or an unmodified
  client cannot open your shapes; they're not trivially copyable. The key is
  obfuscated in the DLL (not a plaintext blob). Stops ~everyone casual.
- **Not achievable:** making extraction impossible for someone reverse
  engineering the LiFx client. No client-side scheme can do that.

The design keeps the door open for **server-delivered per-session keys**
(v2), which raises the bar further (key never on disk) but still cannot beat
the fundamental limit above.

## How it works

Every loose asset the engine reads goes through a single `FileStream` class
(RE in [`lfxe_texture_re.md`](lfxe_texture_re.md)). The DLL detours that
class's methods (`source/client/hook_filestream.cpp`,
`source/server/hooks/engine/hook_filestream.cpp`; shared core
`source/core/crypto/lfxe_filestream.cpp`):

1. `FileStream::open` runs as normal, then we **peek the `LFXE` magic** on the
   freshly opened stream (read mode only).
2. **No magic** → leave the stream untouched. Vanilla and encrypted assets
   coexist; non-encrypted files cost only one magic check at open.
3. **Magic** → decrypt the whole payload via the `KeyProvider` into a heap
   buffer keyed on the `FileStream*`, and serve all subsequent
   `_read`/`getStreamSize`/`getPosition`/`setPosition` from that plaintext
   (the engine's own file buffering is bypassed for this stream). The
   plaintext never touches disk. A bad header or missing key leaves the raw
   bytes so the engine rejects them safely (fail-safe) instead of crashing.
   The destructor detour (and a non-LFXE re-open) drop the per-stream state.

Because the decrypt happens below the parsers, a decrypted `.dts` arrives
plaintext at `TSShape::read`, a `.cs.dso` at `exec()`, and a `.dds`/`.png` at
its bitmap loader — all without per-format hooks.

Encryption uses **ChaCha20** (RFC 8439). Because it's a stream cipher, the
decrypt is byte-exact for every seek/read at any (unaligned) offset, and the
container needs no padding.

### Why the FileStream layer (and not Win32 / per-format)

Hooking `TSShape::read` (shapes) and the `openFileStream` factory (`.cs.dso`)
only covered assets routed through those specific seams. Textures load by
calling `FileStream::open` **directly** (62 call sites client-side), bypassing
the factory — which is why an earlier factory-only hook missed `.dds`/`.png`.
The mapped-texture path was also suspected to use `MapViewOfFile`; RE
disproved that (only middleware memory-maps; engine assets do not). Hooking
the one `FileStream` class therefore covers everything with no Win32 hooking
and no per-format special-casing.

### LFXE container format

24-byte header, little-endian, then ciphertext (`source/core/crypto/lfxe_format.h`,
mirrored in `scripts/dts_lib.py`):

```
off size field
0   4    magic   = 'L','F','X','E'
4   1    format  = 1
5   1    cipher  = 1 (ChaCha20)
6   2    keyId   = which key decrypts this (0 = baked key)
8   12   nonce   = 96-bit ChaCha20 nonce (unique per file)
20  4    reserved= 0
24  ...  ciphertext (ChaCha20 of the original file, block counter starts at 1)
```

`keyId` is the migration door: a future `ServerKeyProvider` answers non-zero
ids with per-session keys over the existing net channel, with **no change**
to this format or to the hook.

## Keys

The project key lives in `config/dts_key.bin` (32 bytes, **gitignored** —
never commit it). Two consumers share it:

- **Client:** `scripts/gen_baked_key.py` bakes it into
  `source/core/crypto/lfxe_key_data.h` (also gitignored), XOR-masked by
  a fixed LCG keystream so it isn't a plaintext run in the DLL. Both
  `build_linux.sh` and the Visual Studio project (`GenerateLfxeKeyHeader`
  pre-build target, needs Python 3) regenerate this header automatically if missing.
- **Packer:** `scripts/dts_encrypt.py` reads the same `config/dts_key.bin`.

First build with no key generates a fresh one:
`python3 scripts/gen_baked_key.py --new --key-out config/dts_key.bin`.
**Re-baking the key requires rebuilding the client DLL** (and re-encrypting
shapes), or old encrypted shapes won't decrypt.

## Build

`./build_linux.sh client` (and `lifx` for the server) compiles the decrypt
path into the DLL (`hook_filestream.cpp` + `core/crypto/*.cpp`) and generates
the baked-key header from `config/dts_key.bin` if absent. Deploy as usual (see
[`loader_and_injection.md`](loader_and_injection.md)). The client decrypts
shapes/textures/scripts; the server decrypts the assets it reads (notably
`.cs.dso`), so deploy the rebuilt DLL wherever encrypted assets load.

## Packing assets

Run **before** zipping the modpack, against the staging art/scripts tree.
`--ext` selects which extensions to wrap (default `dts`):

```bash
# shapes only (default)
python3 scripts/dts_encrypt.py yolauncher/modpack/art --key config/dts_key.bin
# shapes + textures + compiled scripts in one pass
python3 scripts/dts_encrypt.py <dir> --ext dts,dds,png,dso --key config/dts_key.bin
```

Idempotent (skips files already carrying `LFXE`), atomic per file. `--ext dso`
matches `*.cs.dso`; `--dry-run` to preview.

**Caveat:** an encrypted modpack requires players to run the LiFx client. A
stock Yo Launcher client cannot load encrypted assets — skip encryption for
vanilla-client distributions. This step is wired into the `lif-server-install`
skill as an optional pre-zip stage.

## Offline self-test

Two offline tests prove the shipped path is byte-correct (no Wine needed):

- `bash scripts/test_lfxe_roundtrip.sh` — the **crypto**: C == Python == RFC
  8439 §2.3.2 keystream, baked-key recovery, pack→decrypt round-trip across
  sizes 0–250017. Exercises `chacha20`, `lfxe_decrypt`, `baked_key_provider`.
- `bash scripts/test_lfxe_filestream.sh` — the **serve logic**: drives
  `lfxe_filestream.cpp` through a mock `FileStream`, proving an LFXE container
  is served back as byte-exact plaintext for full reads, arbitrary unaligned
  seeks, and EOF edges (incl. >8 KB payloads, the engine's buffer-refill
  range), and that vanilla files and unregistered streams pass straight
  through.

## Live verification (M5 — DONE)

> **Confirmed in the live client (2026-06-15):** encrypted PNG icons, a 5.5 MB
> `.dds` material texture, and a `.dts` shape all decrypt in-memory and render;
> an encrypted server `main.cs` decrypts and executes on the dedicated server.
> One client-only fix was needed first: the registry lock was changed from
> `std::mutex` to a Win32 SRWLOCK — `std::mutex::lock()` faults through the
> client's `MSVCP140.dll` under Wine (see `lfxe_texture_re.md` "Client
> boot-crash root cause"). The repro recipe below still applies.

### Repro recipe

The offline tests cover crypto + serve; the runtime detours (attach +
FileStream ABI) must be confirmed in the running game:

1. Build + deploy the rebuilt DLL (client and/or server) and `pdh.dll` into
   the install dir.
2. Encrypt assets the target loads — a shape, a UI `.png`, a material `.dds`,
   a `.cs.dso`:
   `python3 scripts/dts_encrypt.py <dir> --ext dts,dds,png,dso --key config/dts_key.bin`
3. Launch under Wine (`WINEDLLOVERRIDES="pdh=n,b"`).
4. **Expect:** shapes render, textures show, icons appear, scripts run; the
   console/log shows `[LiFx] LFXE decryption active (first encrypted asset
   served)` (server) / `[LiFx-client] …` (client) with no
   `attempt to load a version N dts-shape` / `Found an old DSO` errors.
5. **Negative controls:** the same encrypted assets in a *stock* client/server
   (no LiFx DLL) fail their version gates; a stock DTS viewer can't open them.

The previously-missing buckets from the #116 investigation — Atlas `.dds`
material textures and UI `.png` icons — should now decrypt, since their
loaders call `FileStream::open` directly (the seam this hook covers).

## Encrypted compiled scripts (`*.cs.dso`)

Compiled TorqueScript (`*.cs.dso`) are **server-side** scripts loaded by the
engine's script VM (`exec()` / `CodeBlock`) — in the dedicated server **and**
the client when it host-mode hosts a local game. (The client's own scripts
ship as loose `.cs` and compile at runtime, so there is nothing pre-compiled
to protect there.) They are covered by the same universal FileStream hook: a
`.cs.dso` decrypted at `open` arrives plaintext at `exec()`, passing its
version+size validation transparently.

⚠️ As with shapes, encrypting an *essential* script that can't be decrypted
(wrong/missing key, or no LiFx DLL) hard-fails the engine — a stock server
rejects the bytes with `exec: Found an old DSO (… ver 76 …)`.

## RE notes

- The universal hook targets the engine's single `FileStream` class. Method
  RVAs (client / server), verified by disassembly and recovered with
  `scripts/client_re_texture.py` + `scripts/client_re_callgraph.py`: vtable
  `0xff9ba0` / `0x871ae8`; `open` `0x61e9c0` / `0x44a4f0` (slot 12); `_read`
  `0x61de40` / `0x449b40` (2); `getPosition` `0x61e910` / `0x44a470` (6);
  `setPosition` `0x61ebc0` / `0x44a6f0` (7); `getStreamSize` `0x61e930` /
  `0x44a490` (8); `~FileStream` `0x61dd50` / `0x449a50` (0). Status field at
  base `Stream+0x08` (0=Ok, 2=EOS). Full writeup + the design rationale
  (FileStream vs Win32) in [`lfxe_texture_re.md`](lfxe_texture_re.md).
- Retired seams (kept as reference in the offset headers): `TSShape::read`
  (client `0xb572b0`, the version-gate string xref) and the `openFileStream`
  factory (client `0x61e620`, server `0x44a2c0`, one of 5 FileStream
  constructors). The DSO loader `exec()` (client `0x597a10`, server
  `0x40b2b0`) still validates `getStreamSize()>=0xC` and `version==0x2750` on
  the stream — now satisfied because the FileStream serves plaintext.
