// SPDX-License-Identifier: GPL-2.0-or-later
// tlg_settings Save(): the in-game menu rewrites tlg.ini, and must keep the keys the launcher keeps
// there (renderer, native_passes, adv_*), each once, while its own keys take the menu's values.
#include "tlg_settings.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

static int Count(const std::string& text, const std::string& line) {
    int n = 0;
    std::istringstream in(text);
    for (std::string l; std::getline(in, l);) n += l == line;
    return n;
}

int main() {
    const char* path = "test_settings_save.ini";
#ifdef _WIN32
    _putenv_s("TLG_CONFIG", path);
#else
    setenv("TLG_CONFIG", path, 1);
#endif
    {
        std::ofstream out(path);
        out << "# written by the launcher\nrenderer=2\nnative_passes=1\nadv_barrier_merge=1\n"
               "vsync=0\nframe_gen=1\n";
    }
    TlgSettings::Load();
    TlgSettings::Get().vsync = true; // a menu change
    TlgSettings::Save();
    TlgSettings::Save(); // twice: nothing duplicated

    std::ifstream in(path);
    std::stringstream text;
    text << in.rdbuf();
    const std::string s = text.str();
    assert(Count(s, "renderer=2") == 1);
    assert(Count(s, "native_passes=1") == 1);
    assert(Count(s, "adv_barrier_merge=1") == 1);
    assert(Count(s, "vsync=1") == 1 && Count(s, "vsync=0") == 0);
    assert(Count(s, "frame_gen=1") == 1);
    std::remove(path);
    std::puts("settings-save-test: ok");
    return 0;
}
