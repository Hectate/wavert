# Linux test results

Date: 2026-11-01
Machine: Linux Mint 22.3 (Zena); Linux 6.14.0-37-generic x86_64
Node: v22.18.0; Zig: 0.17.0 (not on `PATH`; run with `ZIG=~/Downloads/zig-x86_64-linux-0.17.0/zig`); Terminal for Part B: GNOME Terminal 3.52.0 (Mint's default)

`.wvt/wvc` built from the seed with `$ZIG cc -O2 -o .wvt/wvc wvc/wvc.c`: exit 0, no output.

| ID | Result | Notes |
|---|---|---|
| A1 | pass | `# tests 191 # pass 191 # fail 0 # cancelled 0 # skipped 0` (151 s) |
| A2 | pass | `WVT_GC_STRESS=50`: `# tests 172 # pass 172 # fail 0 # skipped 0` (262 s). `WVT_GC_STRESS=1` on `gc.test.js`: `# tests 2 # pass 2 # fail 0 # skipped 0` (25 s) |
| A3 | pass | `100`, then `gc: 11 collections, live 11536 bytes, peak heap 9437184 bytes` |
| A4 | pass | `input false output false size -1x-1` |
| A5 | pass | `term` in `script`: `input true output true size -1x-1` (the pseudo-terminal reports 0x0, so `$terminalSize` fails). *Crates*: no panic; `^[[90;49m#` walls, `^[[96;49m@` player, `^[[92;49m*` crate on target after the move, then `Solved in 1 move! Press a key for the next puzzle.`; `q` on puzzle 2 printed `Goodbye.`, exit 0. Output below |
| A6 | FAIL | Sleeps took 122–125 ms, above the expected 100 to about 115 ms. The wait was as expected. See below |
| A7 | pass | `exit 130`. But see the note below: as written, `walk` drew nothing before the signal |
| B1 | pass | |
| B2 | pass | |
| B3 | pass | |
| B4 | pass | |
| B5 | pass | |
| B6 | pass | |
| B7 | pass | |
| B8 | FAIL | `20 x 5 ms sleep: 140 ms`, `250 ms waitKey: 250 ms, null: true`. The wait is as expected; the sleeps are over, as in A6. See "The virtual machine and A6" |
| B9 | pass | Some frames flicker; see the note below |
| B10 | pass | |
| B11 | pass | |
| B12 | pass | Holding a key stops the shimmer instead of speeding it up: the tick advances only when `$waitKey` times out, which key repeat prevents |
| B13 | pass | |

## Failures

### A6: 20 x 5 ms sleep takes 122–125 ms

```
sleep 2 | /tmp/wvt-linux/wait
```

Run three times, each with exit 0:

```
20 x 5 ms sleep: 122 ms
250 ms waitKey: 250 ms, null: true
20 x 5 ms sleep: 125 ms
250 ms waitKey: 250 ms, null: true
20 x 5 ms sleep: 122 ms
250 ms waitKey: 250 ms, null: true
```

That is about 1.1–1.25 ms over each 5 ms sleep, consistently. `$waitKey` is accurate.

## Notes

### A5: *Crates* output (trimmed)

```
printf 'd q' | script -qc "samples/crates samples/crates-puzzles" /dev/null | cat -v | head -40
```

73 lines in all; blank rows (80 spaces then `^[[K^M`) are cut here. Because the pty reports 0x0, the frame is 80 columns wide.

```
d q^[[2J^[[Hcrates: puzzle 1 of 5: First push^[[K^M
...
                                      ^[[90;49m#####^[[39;49m                                     ^[[K^M
                                      ^[[90;49m#^[[96;49m@^[[93;49m$^[[33;49m.^[[90;49m#^[[39;49m                                     ^[[K^M
                                      ^[[90;49m#####^[[39;49m                                     ^[[K^M
...
moves 0    u undo  r restart  n/p puzzle  q quit^[[K^M
^[[K^M
^[[Hcrates: puzzle 1 of 5: First push^[[K^M
...
                                      ^[[90;49m#####^[[39;49m                                     ^[[K^M
                                      ^[[90;49m#^[[39;49m ^[[96;49m@^[[92;49m*^[[90;49m#^[[39;49m                                     ^[[K^M
                                      ^[[90;49m#####^[[39;49m                                     ^[[K^M
...
moves 1    u undo  r restart  n/p puzzle  q quit^[[K^M
Solved in 1 move! Press a key for the next puzzle.^[[K^M
^[[Hcrates: puzzle 2 of 5: Around the pillar^[[K^M
...
moves 0    u undo  r restart  n/p puzzle  q quit^[[K^M
^[[K^M
Goodbye.^M
```

The space in `d q` was taken as the "press a key" for the next puzzle, so `q` quit on puzzle 2.

### A7: the signal arrives before `walk` reads keys

```
.wvt/wvc build samples/walk.wvt
printf 'a\003' | script -qec "samples/walk" /dev/null > /tmp/wvt-linux/walk.out; echo "exit $?"
```

Output: `exit 130`. `/tmp/wvt-linux/walk.out` holds only 2 bytes, `^C`: `walk` drew no frame, and `a` was not echoed. Both bytes probably reached the pseudo-terminal before `walk` started, so the line discipline raised SIGINT itself. This run may not test the signal handler in key mode at all.

An extra run, not in the plan, with delays so that `walk` is waiting for a key:

```
(sleep 1; printf 'a'; sleep 0.5; printf '\003'; sleep 1) | script -qec "samples/walk" /dev/null > /tmp/wvt-linux/walk2.out; echo "exit $?"
```

Output: `exit 130`. `walk2.out` (158 bytes) shows the first frame (`gold: 0`), then a frame after moving left onto the gold (`gold: 7`), then `found 7 gold`. So Ctrl+C in key mode also ends it with 130. Part B (B3) still needs to check that the terminal is restored.

### Files left behind

`wvc build` wrote `samples/crates`, `samples/crates.c`, `samples/walk` and `samples/walk.c`. They are untracked and not in `.gitignore`; they were left in place. Part B needs the binaries.

### B9–B13: *Crates* scrolls one line per frame (a bug in the sample)

Sometimes, when a frame is drawn, text from the bottom of the previous frame flashes at the top of the window. It does not happen on every frame.

Cause: in a terminal, each frame is exactly as tall as the window (the board gets `size.height - 5` rows, plus five lines of text), and every line ends with a newline ([samples/crates.wvt](../samples/crates.wvt), `draw`). The newline after the bottom row scrolls the window up one line before the next frame redraws from the top (`ESC[H`), so the title line is written and then scrolled off. In the A5 capture, where the size falls back to 80x24, each frame is 24 lines, each ending in a newline.

Confirmed in GNOME Terminal: after a session, the scrollback above the game holds a long run of copies of the title line, `crates: puzzle 2 of 5: Around the pillar`, one per frame drawn.

This is in the sample, not the runtime. Windows has not been checked for the same behaviour, and it may scroll there too.

## The virtual machine and A6

This machine is a VirtualBox guest (`systemd-detect-virt` prints `oracle`; clock source `tsc`). On Linux, `$sleep` is a single `nanosleep` call ([runtime/wvt.c](../runtime/wvt.c), `sleep_for`), so a C program was used for comparison. It does the same twenty 5 ms sleeps with no Wavert code (`/tmp/wvt-linux/ns.c`, built with `zig cc -O2`):

```
20 x 5 ms nanosleep: 108.25 ms
20 x 5 ms nanosleep: 121.92 ms
20 x 5 ms nanosleep: 119.34 ms
20 x 5 ms nanosleep: 115.02 ms
20 x 5 ms nanosleep: 108.72 ms
1000 x 1 us nanosleep: 135.19 ms (wakeup latency ~0.135 ms each)
```

Plain `nanosleep` overshoots by about as much as `$sleep` (8–22 ms over 20 sleeps, against Wavert's 22–25 ms), so the A6 failure is most likely the virtual machine's timer and not the runtime. `$waitKey` is one `poll` call, so it pays the wake-up cost only once, and less than 1 ms is lost when `$elapsed` rounds to whole milliseconds. To settle it, run A6 on Linux that is not in a virtual machine.
