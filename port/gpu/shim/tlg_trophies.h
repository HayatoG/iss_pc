// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
void TlgTrophiesMenu();
void TlgTrophiesUpdateTextures();
// Trophy pop-up (2026-11-12): top right, icon + name, with a chime, for each trophy newly earned
// while playing (not for ones recovered from saves). ISS_TROPHY_POPUP=0 turns it off,
// ISS_TROPHY_SOUND=0 only the sound; <data folder>/sounds/trophy.wav replaces the chime.
bool TlgTrophiesToastActive();
void TlgTrophiesToast(float scale);
