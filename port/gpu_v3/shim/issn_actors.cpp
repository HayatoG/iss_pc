// SPDX-License-Identifier: GPL-2.0-or-later
#include "issn_actors.h"
#include "issn_settings.h"
#include <cstdint>
#include "../../src/actor_hook.h"

alignas(64) IssnActorSlot issn_actor_slots[2];

extern "C" uintptr_t issn_actor_slots_address(void) {
    return reinterpret_cast<uintptr_t>(issn_actor_slots);
}

extern "C" void issn_actor_status(int supported) {
    IssnSettings::Get().actor_watch_supported = supported != 0;
}
