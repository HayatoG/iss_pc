# SPDX-License-Identifier: GPL-2.0-or-later
"""The classic (Tk) launcher must build its window. A real display is needed; the test is skipped
without one (CI has none), so run it by hand after touching launcher/infamous_launcher.py.
Regression: show_game_language() ran during build() and used the play button, which is created later."""
import os
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'launcher'))


class TkLauncherBuilds(unittest.TestCase):
    def test_window_builds_and_updates(self):
        try:
            import tkinter as tk
            root = tk.Tk()
        except Exception as error:   # no tkinter or no display
            self.skipTest(f'no Tk display: {error}')
        try:
            import infamous_launcher
            app = infamous_launcher.Launcher(root)
            root.update_idletasks()
            root.update()
            self.assertTrue(hasattr(app, 'play_button'))
            app.show_game_language()          # also safe when called again after the build
        finally:
            root.destroy()


if __name__ == '__main__':
    unittest.main()
