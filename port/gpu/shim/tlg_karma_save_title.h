// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <string>
// The game's save folder (savedata/1/<title>) for the karma switcher's backups, recovery and its
// own state folders. CUSA00223 for US 1.07 (the default, so US paths are unchanged); EU 1.07, whose
// code is the US executable byte for byte, saves under its own folder (CUSA00004). probe.c sets it
// once at game start, before the switcher runs (tlg_karma_set_save_title).
namespace KarmaSaveTitle {
inline std::string& Ref(){static std::string title="CUSA00223";return title;}
inline const std::string& Get(){return Ref();}
}
