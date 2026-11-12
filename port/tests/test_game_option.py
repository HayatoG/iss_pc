# SPDX-License-Identifier: GPL-2.0-or-later
import sys, unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "launcher"))
import infamous_run as run


class GameOption(unittest.TestCase):
    def test_no_option_leaves_arguments_alone(self):
        self.assertEqual(run.take_game_option(["60"]), (None, ["60"]))

    def test_separate_value(self):
        self.assertEqual(run.take_game_option(["--game", "/games/iss"]), ("/games/iss", []))

    def test_equals_value(self):
        self.assertEqual(run.take_game_option(["--game=/games/iss", "60"]), ("/games/iss", ["60"]))

    def test_missing_value_is_ignored(self):
        self.assertEqual(run.take_game_option(["--game"]), (None, []))


if __name__ == "__main__":
    unittest.main()
