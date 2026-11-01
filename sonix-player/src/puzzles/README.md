# Simon Tatham's Portable Puzzle Collection (vendored subset)

From https://git.tartarus.org/simon/puzzles.git, commit 616da16 (2026-09-22),
MIT licence (see LICENCE). Only the core and the seven puzzles on Meloetta's
Puzzles page are here: Solo, Mines, Pattern, Net, Bridges, Loopy, Light Up.

Local changes, kept to the minimum:
- puzzles.h defines COMBINED, so each game is named by its own struct.
- generated-games.h is written by hand (Tatham's build generates it).

The front end (drawing, input, timers) is src/gui/meloetta/puzzleplay.c.
