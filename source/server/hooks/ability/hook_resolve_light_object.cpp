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

#include "hook_resolve_light_object.h"

__CM_INSTATNTIATE(_LightOn_ResolveLightObject_Orig);

namespace
{
	// Temporary instrumentation -- disable once the correct calling
	// convention for Lifx::forceLightOn is confirmed and this probe is no
	// longer needed to cross-check live values.
	constexpr bool kProbeEnabled = true;
}

void* Hooks::LightOnResolve::Call(void* entity, void* outBuf2Qwords)
{
	void* result = _LightOn_ResolveLightObject_Orig(entity, outBuf2Qwords);
	if constexpr (kProbeEnabled)
	{
		void* lightObj = result ? *reinterpret_cast<void**>(result) : nullptr;
		Con::Echo("[lifx-light] resolveLightObject entity(castedInput)=%p -> lightObj=%p", entity, lightObj);
	}
	return result;
}
