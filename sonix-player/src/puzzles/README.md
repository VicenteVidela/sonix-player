# Simon Tatham's Portable Puzzle Collection (vendored subset)

From https://git.tartarus.org/simon/puzzles.git, commit 616da16 (2026-09-22),
MIT licence (see LICENCE). The core plus Meloetta's games: Solo, Mines, Net,
Bridges, Light Up, Towers, Same Game, Flood, and (added 2026-11-07, all the
untested ones) Black Box, Cube, Dominosa, Filling, Flip, Galaxies, Guess,
Inertia, Magnets, Map, Mosaic, Netslide, Palisade, Pearl, Pegs, Range,
Rectangles, Signpost, Singles, Sixteen, Slant, Tents, Train Tracks, Twiddle,
Undead and Unruly. Pattern, Unequal, Keen, Untangle, Loopy and Fifteen were
removed at Vicho's request.

Local changes, kept to the minimum (each marked "Meloetta" in the source):
- puzzles.h defines COMBINED, so each game is named by its own struct.
- generated-games.h is written by hand (Tatham's build generates it).
- Solo and Towers: pencil selection stays after a note, givens can be
  selected, matching digits light up, and ink clears that note from its peers.
  Towers also defaults to the flat (2D) look, so notes get the whole square.
- Undead: pencil selection stays after a note.

The front end (drawing, input, timers) is src/gui/meloetta/puzzleplay.c.
