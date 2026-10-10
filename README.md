# perf tutorial with `perflab`

`perflab.cpp` is one program with seven workloads. Each is chosen so that one
`perf` command shows something obvious about it.

| Mode | What it does | What perf should reveal |
|---|---|---|
| `hot` | arithmetic loop on registers | no misses of any kind |
| `branch_sorted` | `if (x >= 128)` over sorted data | almost no branch misses |
| `branch_random` | same loop, shuffled data | huge branch misses, ~15x slower |
| `cache_seq` | sums a 256 MB array in order | few cache misses |
| `cache_rand` | same array, random order | cache and TLB misses on nearly every read |
| `alloc` | 30M small `new`/`delete` | time is in libc, not your code |
| `syscall` | 10M one-byte `write()` calls | time is in the kernel |
| `all` | all of the above | a profile with several hot spots |

The example numbers below were measured on an Intel Core Ultra (24 cores),
perf 7.0. Yours will differ; the ratios should not.

## 0. Build

```sh
cmake -B build
cmake --build build
./perflab            # lists the modes
```

The binary is written next to the source, so every command below uses
`./perflab`. Without CMake, this one line builds the same thing:

```sh
g++ -O2 -g -fno-omit-frame-pointer perflab.cpp -o perflab
```

- `-g` lets perf map samples back to source lines.
- `-fno-omit-frame-pointer` lets perf walk the stack for call graphs (step 6).

Two habits used throughout:

- `taskset -c 2` pins the program to one core, which cuts noise.
- If perf complains about permissions: `sudo sysctl kernel.perf_event_paranoid=1`.

On hybrid Intel CPUs (P-cores + E-cores) every counter is printed twice, as
`cpu_core/...` and `cpu_atom/...`. Core 2 is a P-core here, so read the
`cpu_core` lines and ignore `cpu_atom ... <not counted>`.

## 1. `perf stat` — the summary

```sh
taskset -c 2 perf stat ./perflab hot
```

Things to read:

- **instructions / cycles** (`insn_per_cycle`, "IPC"). `hot` gives about 1.3.
  Run `alloc` and you get about 5.7. Higher means the CPU is getting more
  done per clock tick.
- **user vs sys seconds** at the bottom: time in your code vs in the kernel.
- **branch-misses**: near zero here.

## 2. Branch prediction — same code, different data

```sh
taskset -c 2 perf stat -e cycles,instructions,branches,branch-misses ./perflab branch_sorted
taskset -c 2 perf stat -e cycles,instructions,branches,branch-misses ./perflab branch_random
```

| | sorted | random |
|---|---|---|
| instructions | 2.57 B | 2.57 B |
| branches | 789 M | 789 M |
| branch-misses | 0.28 M | 181 M |
| cycles | 0.35 B | 5.45 B |

The two runs execute the same instructions. The only thing that changed is
how often the CPU guessed the `if` wrong, and that alone costs 15x.

`-e` chooses the events. `perf list` shows every event your machine has.

## 3. Cache and TLB misses — same code, different order

```sh
taskset -c 2 perf stat -e cycles,instructions,cache-misses,L1-dcache-load-misses,dTLB-load-misses,page-faults ./perflab cache_seq
taskset -c 2 perf stat -e cycles,instructions,cache-misses,L1-dcache-load-misses,dTLB-load-misses,page-faults ./perflab cache_rand
```

| | seq | rand |
|---|---|---|
| L1-dcache-load-misses | 3.3 M | 110 M |
| cache-misses (last level) | 11 M | 75 M |
| dTLB-load-misses | 0.04 M | 55 M |
| page-faults | 98 k | 98 k |
| cycles | 0.9 B | 3.1 B |

- Reading in order lets the hardware prefetcher fetch memory ahead of you.
  Random order defeats it.
- `page-faults` is identical: both modes allocate the same memory. It is a
  software event counted by the kernel, not a hardware counter.
- `cache_rand` also runs more instructions, because it shuffles the index
  array first. Step 5 shows how to separate setup from the loop you care about.

## 4. Noise — repeat the run

```sh
taskset -c 2 perf stat -r 5 -e cycles,branch-misses ./perflab branch_sorted
```

`-r 5` runs it five times and prints the mean with `+- %`. If that
percentage is larger than the difference you are trying to measure, you have
not measured anything yet.

## 5. `perf record` + `perf report` — where is the time going?

`perf stat` counts for the whole program. `perf record` samples it so you can
see which function is responsible.

```sh
taskset -c 2 perf record ./perflab all
perf report                       # interactive; q to quit
perf report --stdio --sort symbol # plain text
perf report --stdio --sort dso    # grouped by binary / library / kernel
```

Expected top of the report:

```
28%  [.] hot_loop
26%  [.] sum_big_values
 6%  [.] shuffle_order
 5%  [.] sum_by_index
 4%  [.] alloc_churn
 2%  [.] _int_free
 2%  [.] __GI___libc_write
```

- `[.]` is user code, `[k]` is kernel code.
- By `dso`: about 72% `perflab`, 17% kernel, 9% `libc`. That is `alloc` and
  `syscall` showing up as time you did not write.
- `shuffle_order` is the setup for `cache_rand`, now visibly separate from
  `sum_by_index`.

## 6. Call graphs — who called it?

```sh
taskset -c 2 perf record -g ./perflab alloc
perf report --stdio --children --sort symbol | head -40
```

Two columns now:

- **Self**: time in that function's own instructions.
- **Children**: Self plus everything it called.

`alloc_churn` is about 31% self but 89% with children. The gap is
`malloc`, `_int_free` and `operator new`, called on its behalf.

If stacks look broken, the code was built without frame pointers. Either
rebuild with the flag from step 0 or record with `perf record --call-graph dwarf`.

## 7. `perf annotate` — which instruction?

```sh
taskset -c 2 perf record ./perflab cache_rand
perf annotate --stdio -M intel sum_by_index
```

```
 0.75 :  mov  ecx, dword ptr [rax]          ; load the next index
92.72 :  add  rdx, qword ptr [rdi + 8*rcx]  ; load data[index]  <- the cache miss
```

One instruction holds over 90% of the samples. In the interactive
`perf report`, press `a` on a function to get the same view.

Samples often land one instruction *after* the slow one ("skid"), so read
the neighbours too. Try `perf annotate --stdio -M intel sum_big_values` after
recording `branch_random` to see it.

## 8. Top-down — why is the CPU not doing useful work?

```sh
taskset -c 2 perf stat -M TopdownL1 ./perflab cache_rand
```

This splits the pipeline's capacity into four buckets (Intel; needs a real
CPU, usually not a VM):

| Bucket | Meaning |
|---|---|
| retiring | useful work |
| bad speculation | work thrown away after a wrong guess |
| frontend bound | could not fetch/decode instructions fast enough |
| backend bound | waiting on data or on a busy execution unit |

Measured here:

| Mode | retiring | bad spec | frontend | backend |
|---|---|---|---|---|
| `hot` | 12% | 0% | 0% | 87% |
| `branch_random` | 4% | 22% | 62% | 12% |
| `cache_rand` | 11% | 1% | 3% | 86% |

Two results worth thinking about rather than memorising:

- `hot` is backend bound even though it never misses a cache. Each iteration
  needs the previous `x`, so the CPU waits on the multiply's result.
- `branch_random` shows mostly as frontend bound on this CPU, not bad
  speculation. After each wrong guess the frontend has to restart fetching
  from the right place, and that restart time lands in the frontend bucket.

(`perf stat --topdown` is the older spelling. On this hybrid machine it prints
misaligned columns, so `-M TopdownL1` is easier to read.)

## 9. System calls

```sh
taskset -c 2 perf stat -e task-clock,context-switches,syscalls:sys_enter_write ./perflab syscall
perf trace -s ./perflab syscall 2>&1 | head -20
```

- First command: 10,000,001 writes, and about 1.06 s sys against 0.22 s user.
- `perf trace -s` prints a per-syscall table (calls, total ms, average).
  Tracing every call is expensive, so the program runs several times slower
  under it. Use it for counts, not for timing.

Tracepoints and `perf trace` usually need root or `perf_event_paranoid <= 0`.

## 10. Flame graphs — the whole profile as one picture

perf collects the stacks; Brendan Gregg's FlameGraph scripts draw them. The
pipeline is always the same three stages: **sample** stacks, **fold**
identical stacks together, **draw** boxes whose width is the sample count.

The graphs produced by the commands below are checked in under `result/`.

### 10.1 One-time setup

You need `perf`, `perl`, and the FlameGraph scripts (plain Perl, nothing to
build):

```sh
git clone --depth 1 https://github.com/brendangregg/FlameGraph ~/FlameGraph
```

### 10.2 Build with frame pointers and debug info

The CMake build from step 0 already uses `RelWithDebInfo` and
`-fno-omit-frame-pointer`. Rebuild only if you changed the source:

```sh
cmake -B build
cmake --build build
```

### 10.3 Record a profile with call stacks

```sh
taskset -c 2 perf record -g ./perflab all
```

- `-g` saves the whole call stack with every sample, not just the function
  that was running. Without it there is nothing to stack.
- `taskset -c 2` pins the run to one core, as in the earlier steps.
- Output goes to `perf.data` (an existing one is renamed `perf.data.old`).
  Expect about 5 seconds and roughly 16,000 samples.

### 10.4 Dump the samples as text

```sh
perf script > out.perf
```

`perf script` reads `perf.data` and prints every sample with its stack, one
frame per line. Look at it once with `head -30 out.perf`.

### 10.5 Fold the stacks

```sh
~/FlameGraph/stackcollapse-perf.pl out.perf > out.folded
```

Each distinct stack becomes one line, root first, frames joined by `;`,
followed by how often it was seen:

```
perflab;_start;...;main;alloc_churn;__libc_free;_int_free 385541579
```

This file is plain text, so `grep` and `sort` work on it. The ten hottest
stacks:

```sh
awk '{print $NF, $0}' out.folded | sort -nr | head
```

### 10.6 Render the SVG

```sh
mkdir -p result
~/FlameGraph/flamegraph.pl --title "perflab all" out.folded > result/flame-fp.svg
```

### 10.7 Open it

Open `result/flame-fp.svg` in a browser. Click a box to zoom, hover for
percentages, Ctrl+F to search. On a remote machine, copy it to your laptop
first:

```sh
scp user@host:code_repo/perf-basic-tutorial/result/flame-fp.svg .
```

### 10.8 The same thing as one pipeline

Steps 10.4 to 10.6 without the intermediate files:

```sh
perf script | ~/FlameGraph/stackcollapse-perf.pl | ~/FlameGraph/flamegraph.pl > result/flame-fp.svg
```

If you did keep the intermediate files, remove them when done:

```sh
rm -f out.perf out.folded
```

### 10.9 How to read it

- **Width** is the share of samples in that function plus everything it
  called. Wide means expensive.
- **Height** is call depth: `main` at the bottom, the running function on top.
- **Left-to-right order** is alphabetical, not time.
- **Colour** is random.

For `all`, expect roughly this split (numbers from one run; yours will differ
a little):

| Tower | Share |
|---|---|
| `hot_loop` | ~28% |
| `sum_big_values` | ~26% |
| `write()` and the kernel stack above it | ~14% |
| `alloc_churn` with `malloc` / `_int_free` on top | ~10% |
| cache modes (`shuffle_order`, `sum_by_index`, page faults) | most of the rest |

### 10.10 Broken stacks, and the DWARF fix

`-g` walks the stack by following frame pointers. A function that does not
set up its own frame makes the unwinder skip that function's caller. In
`result/flame-fp.svg` this shows up in three places:

- `hot_loop` sits directly on `__libc_start_call_main`; `main` is missing.
- `__libc_write` sits directly on `main`; `syscall_storm` is missing.
- `sum_big_values` sits directly on `main`; `run_branch` is missing.

The widths are still right. Only the parent is wrong.

To get correct stacks, unwind with the DWARF debug info instead. Only the
record command changes:

```sh
taskset -c 2 perf record --call-graph dwarf ./perflab all
perf script | ~/FlameGraph/stackcollapse-perf.pl \
  | ~/FlameGraph/flamegraph.pl --title "perflab all (dwarf)" > result/flame-dwarf.svg
```

In `result/flame-dwarf.svg` all three callers are back: `main;hot_loop`,
`main;syscall_storm;__libc_write`, `main;run_branch;sum_big_values`.

The cost is size and overhead: perf copies a chunk of the stack with every
sample, so `perf.data` was about 128 MB instead of 1.8 MB for this run. Use
frame pointers by default and DWARF when the stacks look wrong.

This is the same data as step 6. It needs the same working call stacks, so
the `-fno-omit-frame-pointer` build flag matters here too.

## 11. Things to try next

1. Rebuild with `-O0` and repeat step 1 on `hot`. What happens to IPC and
   instruction count?
2. In `sum_big_values`, delete the `keep(sum);` line, rebuild, and repeat
   step 2. The gap between sorted and random should vanish. Use step 7 to see
   what the compiler did instead of a branch.
3. Change `n` in `run_cache` from `1 << 25` to `1 << 12` (32 KB) and repeat
   step 3. Does random order still hurt when everything fits in L1?
4. `perf stat -e cycles:u,cycles:k ./perflab syscall` — `:u` and `:k` split
   any event into user and kernel.
5. `taskset -c 2 perf record -e branch-misses ./perflab all` then
   `perf report` — sample on an event other than time, to see which function
   owns the misses (99% `sum_big_values`). Keep the `taskset`: unpinned on
   this hybrid CPU the report wrongly blamed `strcmp`.

## Cheat sheet

| Question | Command |
|---|---|
| How fast, overall? | `perf stat ./prog` |
| How many of event X? | `perf stat -e X,Y ./prog` |
| What events exist? | `perf list` |
| Is the result stable? | `perf stat -r 5 ./prog` |
| Which function is hot? | `perf record ./prog` then `perf report` |
| Who calls it? | `perf record -g ./prog` then `perf report --children` |
| Whole profile as a picture? | `perf script \| stackcollapse-perf.pl \| flamegraph.pl` (step 10) |
| Which instruction? | `perf annotate --stdio -M intel func` |
| Why is the CPU stalled? | `perf stat -M TopdownL1 ./prog` |
| Which syscalls? | `perf trace -s ./prog` |
| What is running right now? | `sudo perf top` |
