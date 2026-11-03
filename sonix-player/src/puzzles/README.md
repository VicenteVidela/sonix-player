# Simon Tatham's Portable Puzzle Collection (vendored subset)

From https://git.tartarus.org/simon/puzzles.git, commit 616da16 (2026-09-22),
MIT licence (see LICENCE). Only the core and Meloetta's nine puzzles are here:
Solo, Mines, Net, Bridges, Light Up, Towers, Fifteen, Same Game and Flood.
Pattern, Unequal, Keen, Untangle and Loopy were removed at Vicho's request;
the three tap-friendly replacements come from the same upstream commit.

Local changes, kept to the minimum:
- puzzles.h defines COMBINED, so each game is named by its own struct.
- generated-games.h is written by hand (Tatham's build generates it).

The front end (drawing, input, timers) is src/gui/meloetta/puzzleplay.c.
