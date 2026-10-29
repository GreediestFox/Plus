#pragma once

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
	Helpers for optional hooks that must not be able to take other hooks down with them.

	DetourTx
	    One Detours transaction per hook. A shared transaction is all-or-nothing: a single failed
	    DetourAttach (or a DetourDetach of a not-yet-committed attach) makes DetourTransactionCommit
	    fail for every hook queued in it. With DetourTx an optional hook that cannot attach is
	    aborted on its own and every other hook stays active. Hooks that use it must be attached
	    AFTER the main transaction in Lifx::Server::AttachHooks() has been committed, because
	    Detours transactions cannot nest.

	BytePatchSet
	    Records the original bytes of every in-memory patch so a hook that fails half way (a later
	    patch, or its Detours commit) can put the code back exactly as it found it.

	Typical use:

	    BytePatchSet patches;
	    DetourTx tx;
	    if (!patches.Write(addr, newBytes, n) || !tx.Attach(&(PVOID&)_Orig, Handler) || !tx.Commit())
	    {
	        patches.Rollback();
	        return;
	    }
	    patches.Release();
*/

#include <Windows.h>
#include <detours/detours.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace Hooks
{
	namespace Engine
	{
		class DetourTx
		{
		public:
			DetourTx()
			{
				ok_ = (DetourTransactionBegin() == NO_ERROR);
				open_ = ok_;
				if (ok_)
					ok_ = (DetourUpdateThread(GetCurrentThread()) == NO_ERROR);
			}

			~DetourTx()
			{
				if (open_)
					DetourTransactionAbort();
			}

			DetourTx(const DetourTx&) = delete;
			DetourTx& operator=(const DetourTx&) = delete;

			// Queues an attach. Returns false (and poisons the transaction) on failure.
			// `detour` may be any function pointer (the engine hooks are __fastcall free functions).
			template <typename Fn>
			bool Attach(PVOID* target, Fn detour)
			{
				if (!ok_)
					return false;
				lastError_ = DetourAttach(target, reinterpret_cast<PVOID>(detour));
				if (lastError_ != NO_ERROR)
					ok_ = false;
				return ok_;
			}

			// Queues a detach (used on unload).
			template <typename Fn>
			bool Detach(PVOID* target, Fn detour)
			{
				if (!ok_)
					return false;
				lastError_ = DetourDetach(target, reinterpret_cast<PVOID>(detour));
				if (lastError_ != NO_ERROR)
					ok_ = false;
				return ok_;
			}

			// Commits when every queued call succeeded, aborts otherwise. Returns true only when committed.
			bool Commit()
			{
				if (!open_)
					return false;
				open_ = false;
				if (!ok_)
				{
					DetourTransactionAbort();
					return false;
				}
				lastError_ = DetourTransactionCommit();
				ok_ = (lastError_ == NO_ERROR);
				return ok_;
			}

			LONG LastError() const { return lastError_; }

		private:
			bool ok_ = false;
			bool open_ = false;
			LONG lastError_ = NO_ERROR;
		};

		class BytePatchSet
		{
		public:
			~BytePatchSet() = default;

			// Writes `size` bytes at `addr` and remembers what was there. Returns false when the page cannot be written.
			// `protect` is the protection used while writing (PAGE_READWRITE for read-only data, code by default).
			bool Write(std::uintptr_t addr, const void* bytes, std::size_t size, DWORD protect = PAGE_EXECUTE_READWRITE)
			{
				Entry e;
				e.addr = addr;
				e.protect = protect;
				e.original.assign(reinterpret_cast<const std::uint8_t*>(addr), reinterpret_cast<const std::uint8_t*>(addr) + size);
				if (!Poke(addr, bytes, size, protect))
					return false;
				entries_.push_back(std::move(e));
				return true;
			}

			bool WriteByte(std::uintptr_t addr, std::uint8_t value) { return Write(addr, &value, 1); }

			bool WriteU32(std::uintptr_t addr, std::uint32_t value) { return Write(addr, &value, sizeof(value)); }

			// Restores every patch in reverse order.
			void Rollback()
			{
				for (auto it = entries_.rbegin(); it != entries_.rend(); ++it)
					Poke(it->addr, it->original.data(), it->original.size(), it->protect);
				entries_.clear();
			}

			// Keeps the patches (call once the hook is fully active).
			void Release() { entries_.clear(); }

			bool Empty() const { return entries_.empty(); }

		private:
			struct Entry
			{
				std::uintptr_t addr = 0;
				DWORD protect = PAGE_EXECUTE_READWRITE;
				std::vector<std::uint8_t> original;
			};

			static bool Poke(std::uintptr_t addr, const void* bytes, std::size_t size, DWORD protect)
			{
				DWORD old = 0;
				if (!VirtualProtect(reinterpret_cast<void*>(addr), size, protect, &old))
					return false;
				std::memcpy(reinterpret_cast<void*>(addr), bytes, size);
				DWORD dummy = 0;
				VirtualProtect(reinterpret_cast<void*>(addr), size, old, &dummy);
				FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(addr), size);
				return true;
			}

			std::vector<Entry> entries_;
		};
	}
}
