// SPDX-License-Identifier: GPL-2.0-or-later
#include "tlg_actors.h"
#include "tlg_settings.h"
#include <cstdint>
#include "../../src/actor_hook.h"

alignas(64) TlgActorSlot tlg_actor_slots[2];

extern "C" uintptr_t tlg_actor_slots_address(void) {
    return reinterpret_cast<uintptr_t>(tlg_actor_slots);
}

extern "C" void tlg_actor_status(int supported) {
    TlgSettings::Get().actor_watch_supported = supported != 0;
}
