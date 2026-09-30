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
	Cart places: per-cart-type capacity for "put movable in cart" (logs and other large items), which the engine hard-codes to 12 places.

	Found by decompilation of ddctd_cm_yo_server.exe:

	  AbilityImp::PutMovableInCart::_onDoPerform (RVA 0x37E1D0) adds the places already used in the target cart to the size of the item
	  being loaded (byte at type record +0x1C, a log is 2) and refuses when the sum is above 12: `cmp r14d,0Ch ; jbe` at RVA 0x37E33A
	  (bytes 41 83 FE 0C 76 17, imm8 at 0x37E33D). The "Im Wagen sind %1 von %2 Plaetze belegt" message (id 2678, sent by that ability
	  and by AbilityImp::PullMovableFromCart::_onDoPerform, RVA 0x37D880) prints the maximum from a global int at RVA 0x81D3E0 (value 12).
	  The check never looks at the cart's type, so it is the same for every cart.

	Both abilities are wrapped: the wrapper reads the object type of the cart the ability targets (entity at ability+0x38 -> object ->
	type record [obj+0x370] -> id [rec+0x60], same lookup as the greenhouse alias), then sets the compare byte and the displayed maximum in
	memory to that cart type's places (12 for every other cart) for the duration of the call.

	Configured from lifxpluss.xml:
	    <cartPlaces enabled="1" verbose="1">
	        <cart datablockId="707" places="30" />
	    </cartPlaces>
	(places 1..127; match a cart by objectTypeId, or, for harnessed carts (AttachedObject_Entity, whose type is not readable), by the AttachedShapeData datablockId from Transport.cs)
*/

#include "server/cm_server.h"

namespace tinyxml2 { class XMLElement; }

// AbilityImp::PutMovableInCart::_onDoPerform(ability) -> result code. RVA 0x37E1D0.
__CM_DECL_EXTERNAL(U32, __fastcall, _Cart_PutInCart, LPVOID ability);
// AbilityImp::PullMovableFromCart::_onDoPerform(ability) -> result code. RVA 0x37D880.
__CM_DECL_EXTERNAL(U32, __fastcall, _Cart_PullFromCart, LPVOID ability);

namespace Hooks
{
	namespace Engine
	{
		U32 __fastcall OnCartPutInCart(LPVOID ability);
		U32 __fastcall OnCartPullFromCart(LPVOID ability);

		bool ConfigureCartPlaces(const tinyxml2::XMLElement* root);
		void AttachCartPlacesHook();
	}
}