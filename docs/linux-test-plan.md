# Linux test plan

Wavert's native runtime ([runtime/wvt.c](../runtime/wvt.c)) has only ever run on Windows. Linux builds compile cleanly with `zig cc -Wall`, but have never run. The parts most likely to differ on Linux are:

- the garbage collector, which scans the C stack and registers and so depends on stack layout, `setjmp` and `posix_memalign`. The first Windows run exposed a frame-pointer bug in it;
- key input (`$readKey`, `$waitKey`): terminal modes through `termios`, signal handlers for Ctrl+C and Ctrl+Z, and `poll` for timeouts;
- `$terminal` and `$terminalSize` (`isatty`, `ioctl(TIOCGWINSZ)`), and `$sleep` (`nanosleep`).

This plan checks them. **Part A** is for a Claude Code session on the Linux machine: it needs no human at the keyboard. **Part B** needs a person pressing keys in a real terminal.

## Rules for the session running Part A

- **Record, do not fix.** Do not change `runtime/`, `wvc/`, `lib/` or the tests. If something fails, capture the exact command, its full output and exit code, and move on to the next check. Fixes are planned afterwards, from the results.
- Write the results to `docs/linux-test-results.md`, in the form shown at the end of this plan. Do not commit; the person running the plan decides.
- Scratch programs go in `/tmp/wvt-linux/`, not in the repository.

## Setup

1. **Node.js 22.3 or later**: `node --version`.
2. **Zig**: `zig version`. If `zig` is not on `PATH`, set `ZIG` to the zig executable for every command below: `export ZIG=/path/to/zig`.
3. Build the compiler from its seed, from the repository's root:
   ```
   mkdir -p .wvt
   ${ZIG:-zig} cc -O2 -o .wvt/wvc wvc/wvc.c
   ```
4. Record in the results: the distribution and version (`cat /etc/os-release`), the kernel (`uname -srm`), the Node and Zig versions, and the terminal used for Part B.

## Part A: automated checks (the Claude session)

### A1. The test suite

```
cd wvc
npm test
```

This builds `wvc` from its seed, checks the bootstrap fixed point (the Linux `wvc` must compile itself to exactly `wvc/wvc.c`), and builds and runs every example, sample, library and language test, about 190 in all. Expected: every test passes, none skipped. A skip means Zig was not found.

Record the totals line (`# tests ... # pass ... # fail ...`) and, for each failing test, its name and the `not ok` block of output.

### A2. The collector under stress

`WVT_GC_STRESS=n` makes every compiled program collect every *n* allocations and fill freed memory with a marker, so that a missing reference shows up as a crash or wrong output. Run, in `wvc/`:

```
WVT_GC_STRESS=50 node --test test/examples.test.js test/stdlib.test.js test/syntax.test.js test/errors.test.js test/spec.test.js test/gc.test.js test/lib.test.js
```

Expected: everything passes. Then the harshest setting, collecting on every allocation, on the collector's own tests:

```
WVT_GC_STRESS=1 node --test test/gc.test.js
```

Record both totals and any failures in full.

### A3. Collector statistics

Build and run a program that allocates heavily, with statistics on:

```
mkdir -p /tmp/wvt-linux
cat > /tmp/wvt-linux/churn.wvt <<'EOF'
;var keep is []
;var i is 0
;while (i < 1000000) {
    ;const s is "item {i}"
    ;if (i % 10000 == 0) { ;(keep, s)($push) }
    ;set i += 1
}
;keep($length)($print)
;exit
EOF
.wvt/wvc build /tmp/wvt-linux/churn.wvt
WVT_GC_STATS=1 /tmp/wvt-linux/churn
```

Expected: it prints `100`, then a line on stderr, `gc: N collections, live L bytes, peak heap P bytes`, with at least one collection and a live size far below the roughly 50 MB the program allocates in all. Record both lines.

### A4. Terminal detection with redirected streams

```
cat > /tmp/wvt-linux/term.wvt <<'EOF'
;const t is ($terminal)
;const s is ($terminalSize)
    onfail { { width: -1, height: -1 } }
;"input {t.input} output {t.output} size {s.width}x{s.height}"($print)
;exit
EOF
.wvt/wvc build /tmp/wvt-linux/term.wvt
echo | /tmp/wvt-linux/term | cat
```

Expected: `input false output false size -1x-1`. Both streams are pipes, and `$terminalSize` fails when stdout is not a terminal.

### A5. Terminal detection, key mode and colour in a pseudo-terminal

`script` runs a program in a pseudo-terminal, so `isatty` is true, while taking its keys from a pipe. Check that `script` exists (`script --version`, from util-linux); if it does not, record A5 as skipped.

```
script -qc "/tmp/wvt-linux/term" /dev/null < /dev/null | cat -v
```

Expected: `input true output true`, and a size. A pseudo-terminal may report `0x0`, in which case `$terminalSize` fails and prints `-1x-1`; record which.

Then play *Crates* through its first puzzle in a pseudo-terminal, which takes the colour path:

```
.wvt/wvc build samples/crates.wvt
printf 'd q' | script -qc "samples/crates samples/crates-puzzles" /dev/null | cat -v | head -40
```

Expected: no panic; escape sequences visible as `^[[...m`, including `^[[90;49m` (grey walls), `^[[96;49m@` (the player), and `^[[92;49m*` (a crate on its target) after the move; then `Solved in 1 move!`. Record the output, trimmed if long.

### A6. Timing

```
cat > /tmp/wvt-linux/wait.wvt <<'EOF'
;var t is ($elapsed)
;var i is 0
;while (i < 20) {
    ;5($sleep)
    ;set i += 1
}
;"20 x 5 ms sleep: {($elapsed) - t} ms"($print)
;set t to ($elapsed)
;const k is 250($waitKey)
;"250 ms waitKey: {($elapsed) - t} ms, null: {k == null}"($print)
;exit
EOF
.wvt/wvc build /tmp/wvt-linux/wait.wvt
sleep 2 | /tmp/wvt-linux/wait
```

`sleep 2 |` keeps stdin open with nothing in it, so `$waitKey` times out. Expected: the sleeps take 100 to about 115 ms, and the wait 250 to about 260 ms with `null: true`. Record both lines.

### A7. Signals while in key mode

In a pseudo-terminal, send Ctrl+C (byte 3) while `samples/walk` waits for a key:

```
.wvt/wvc build samples/walk.wvt
printf 'a\003' | script -qec "samples/walk" /dev/null > /tmp/wvt-linux/walk.out; echo "exit $?"
```

Expected: the exit status reports SIGINT, normally 130. Record it. Whether the terminal is restored can only be seen in Part B.

## Part B: by hand, in a real terminal

These need a person. Use the terminal you normally use, and note it (GNOME Terminal, Konsole, xterm, the VS Code terminal, ...). Build the programs first, from the repository's root, as in Part A.

| ID | Do | Expect |
|---|---|---|
| B1 | `samples/walk`; move with the arrow keys, then w/a/s/d | The `@` moves; gold is found; nothing is echoed |
| B2 | In `walk`, press Escape alone | It quits at once, with `Goodbye, with ... gold.` |
| B3 | Start `walk`, press Ctrl+C | It ends at once; afterwards, typing at the shell prompt is echoed normally (if not, `stty sane` fixes it; note that it was needed) |
| B4 | Start `walk`, press Ctrl+Z, then `fg` | It suspends with the shell prompt working normally, then resumes; the arrow keys still work |
| B5 | `/tmp/wvt-linux/term` (from A4) | `input true output true size WxH`, matching the window (compare `tput cols; tput lines`) |
| B6 | Resize the window, run `term` again | The new size |
| B7 | `/tmp/wvt-linux/term > /tmp/wvt-linux/t.txt; cat /tmp/wvt-linux/t.txt` | `output false`, size `-1x-1` |
| B8 | `/tmp/wvt-linux/wait`, pressing nothing | About 100 ms and about 250 ms, `null: true` |
| B9 | `samples/crates samples/crates-puzzles` | Grey walls, yellow targets that shimmer, yellow `$` crates that turn green `*` on a target, a cyan `@`, the puzzle centred |
| B10 | In `crates`, solve puzzle 1 (one move right) and wait a few seconds | `Solved in 1 move!` stays on screen while the targets keep shimmering |
| B11 | In `crates`, resize the window while it runs | The puzzle re-centres on the next frame (within a quarter of a second) |
| B12 | In `crates`, try u, r, n, p, h/j/k/l, and an arrow key held down | Undo, restart and moving between puzzles work; holding a key does not speed up the shimmer |
| B13 | In `crates`, press Escape alone, then run it again and press q | Both quit, and the shell prompt works normally afterwards |

## Results

Write `docs/linux-test-results.md` like this:

```
# Linux test results

Date:
Machine: (os-release name and version; uname -srm)
Node: ; Zig: ; Terminal for Part B:

| ID | Result | Notes |
|---|---|---|
| A1 | pass / FAIL / skipped | totals line; for a failure, see below |
| ... | | |
| B1 | | |

## Failures

### A1: <test name>
<command>
<full output, and exit code>
```

Use **pass** only when the output matched what this plan expects; otherwise **FAIL**, with what happened, even if it looks minor.
