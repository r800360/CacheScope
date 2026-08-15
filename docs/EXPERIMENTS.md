# The experiments, and what they show

This document explains each CacheScope experiment: the question it asks, the
trick that makes the answer measurable, how to read the resulting curve, and
what can distort it. The last section has exercises that use CacheScope to
demonstrate standard operating systems and computer architecture material.

Throughout, keep the central distinction in mind. **Reported geometry** is what
the operating system or CPUID says exists. **Experimental observations** are
what this machine actually did. They usually agree. When they do not, the
disagreement is itself the interesting result, and the report never quietly
replaces one with the other.

---

## 1. Dependent-load latency

**Question.** How long does a single memory access take, as a function of how
much memory you are touching?

**Method.** Allocate a buffer, place one node per cache line, and link the nodes
into a single random Hamiltonian cycle. Then chase it: the address of each load
is the value returned by the previous load.

That dependency is the whole experiment. A modern CPU can keep a dozen or more
cache misses in flight at once, so if the addresses were independent, the
measured "latency" would really be throughput divided by parallelism. Making
each address depend on the previous load serializes them, and what you measure
is one full access at a time. The random ordering additionally defeats the
stride prefetchers, which would otherwise fetch the next node before it is
asked for.

**Reading the curve.** Latency is flat while the working set fits in a cache and
steps up as it outgrows each level. The reported L1/L2/L3 sizes are drawn on the
chart as dashed reference lines.

The report does not print "the L2 boundary is here", because on real hardware
the curve ramps rather than steps: prefetchers, non-inclusive caches and sliced
last-level caches smear the transitions, and any single "knee" you name moves
from run to run. Instead it reports **latency escalation**: the working set at
which latency reached 2x, 4x, 8x, 16x and 32x the machine's own fastest access.
That is well defined, reproducible, and comparable between machines with very
different cache sizes.

**Distortions.** Frequency scaling changes nanoseconds without changing cycles.
Transparent huge pages move the point where TLB pressure adds to cache pressure.
On a shared last-level cache, other processes evict your data.

---

## 2. Streaming read, write and memcpy

**Question.** What bandwidth does a single core actually achieve?

**Method.** Sequential loops over the same working-set sweep. These kernels are
deliberately the *opposite* of the latency probe: they are allowed to vectorize,
to prefetch, to keep many requests outstanding, and to combine writes. That is
what real streaming code does, so that is what is measured.

**Reading the curve.** Bandwidth is highest inside L1 and falls at each cache
boundary. The absolute numbers are single-threaded; full-chip bandwidth is
higher and needs several cores to reach.

Watch for asymmetries. Writes are usually slower than reads, because a write to
a line not already present must first fetch it (read-for-ownership), so a write
of N bytes can move 2N bytes. `memcpy` reads and writes, and its reported figure
is bytes copied per second, so the underlying traffic is roughly twice that.

**One trap worth knowing.** An earlier version of this project measured
impossible read bandwidths. The compiler had noticed that the read kernel
returned the same value every iteration and hoisted the whole loop out. Any
benchmark kernel must consume its result and must not be provably redundant
across repetitions, or you are timing an empty loop. The kernels here mutate one
seed-dependent sentinel per pass to prevent exactly that.

---

## 3. Serialized line-pair probe (cache line size)

**Question.** How large is a cache line, measured rather than looked up?

**Method.** Chase random regions. At each region, load a value, then load a
second byte at a fixed offset from it, but compute that second address *from
the value the first load returned*, so the processor cannot issue both requests
in parallel. Sweep the offset from 8 to 512 bytes.

If both accesses land in the same cache line, the second one is nearly free. If
the offset crosses a line boundary, the second access is a separate cache access
that cannot start until the first finishes. The step in the curve is the line
boundary.

**Reading it.** The estimate is found as a change point: every split of the
offsets into "before" and "after" is scored, and the best-separated split wins.
The result is snapped down to a power of two, because cache lines are powers of
two and the probe's offsets are not.

The probe runs against a working set sized to about half the last-level cache.
Against main memory the experiment fails: the neighbouring line is already in
the open DRAM row or has been fetched by a prefetcher, and the curve is flat.

**Why it may report twice the line size, or nothing.** Intel CPUs prefetch the
128-byte buddy of every 64-byte line, so the first *visible* step can be at 128
bytes even though the coherency line is 64. And because the last-level cache is
shared, a busy machine can make the curve too noisy to split, in which case the
report says "not decisive" rather than inventing a number. Both outcomes are
honest answers about the machine.

---

## 4. Stride probe (spatial locality and prefetch reach)

**Question.** How much of a fetched line do you get to use, and how far does
hardware prefetching stretch?

**Method.** One fixed buffer, touching one byte every `stride` bytes, with the
stride sweeping from 4 B to 2 KiB.

**Reading the curve.** While the stride is smaller than the unit the machine
effectively transfers, doubling the stride halves the number of touched elements
without changing the number of transfers, so the cost *per touched element*
doubles. Once every element needs its own transfer, the per-element cost
flattens. The report calls the transition the **spatial-locality saturation
stride** and states plainly that it is an upper bound on the line size: on a
machine that prefetches aggressively it lands well beyond it.

This is the practical lesson behind "arrays of structs versus structs of
arrays". If you touch one 4-byte field of a 256-byte record, you pay for the
whole line every time.

---

## 5. Conflict-miss probe (associativity)

**Question.** How many blocks that map to the same cache set can be held at
once?

**Method.** A cache of size S with W ways has S/W bytes between addresses that
land in the same set. So allocate blocks spaced by a power-of-two stride and
chase N of them. While N is at most W they all stay resident, and every access
hits. The moment N exceeds W they evict each other on every pass, and every
access misses.

CacheScope sweeps N from 1 to 24 at strides of 4 KiB, 32 KiB, 256 KiB and 2 MiB.
The latency jump gives the way count of whichever cache has size/ways equal to
that stride, and the report says which reported level that is, or says that no
reported level maps to it, in which case the knee mixes several levels and means
less.

**Reading it.** A stride that matches a real level gives a sharp, repeatable
knee: on the machine this was developed on, the 4 KiB stride reproduces the
reported 12-way L1 exactly. Strides matching outer levels are much noisier,
because outer caches are physically indexed: contiguous *virtual* addresses at a
large stride do not necessarily map to the same physical set. That is a direct
observation of the difference between virtual and physical indexing.

**Why this matters beyond microbenchmarks.** Conflict misses are why a power-of-
two array stride can be catastrophically slower than a slightly larger one, and
why performance-sensitive code pads array dimensions to non-powers of two. It is
also the mechanism behind page colouring in operating systems that care about
cache placement.

---

## 6. TLB reach probe

**Question.** How many pages can a process touch before address *translation*,
rather than data, becomes the bottleneck?

**Method.** Chase a dependent pointer cycle with exactly one cache line touched
per page. The number of distinct pages grows while the amount of resident data
stays tiny; a thousand pages is only 64 KiB of actual data. So any latency
increase is translation cost, not capacity misses.

The stride is one page *plus one line*. With a stride of exactly one page, every
touched line would map to the same cache set and the experiment would silently
turn into the conflict probe from section 5.

**Reading it.** The knee is where the first-level data TLB runs out; typical
values are 64 to 100 entries, with a second, gentler rise at the second-level
TLB. The report also shows how much address space that reach corresponds to.

**Why this is an operating systems experiment.** TLB reach is the clearest place
where a hardware structure the OS manages shows up in user-visible performance.
The page size is an OS decision. Huge pages exist precisely to extend this reach:
a 2 MiB page covers 512 times the address space of a 4 KiB page with the same
one TLB entry. On Linux the report prints the transparent-huge-page setting, so
you can change it and watch this curve move.

---

## 7. Coherence and false sharing

**Question.** What does it cost when two cores touch the same cache line?

**Method.** Two threads, pinned to two logical CPUs, each perform the same number
of atomic increments. In one configuration their counters sit in the *same*
cache line; in the other, in separate lines. Same instruction count, same work,
only the memory layout differs. A single-threaded run gives the uncontended
baseline, and a store/observe ping-pong measures the round trip between the two
CPUs.

CacheScope measures two pairs when it can: an adjacent CPU, which on an SMT
system is usually a sibling thread on the same physical core, and a distant one.

**Reading it.** The ratio between the same-line and separate-line columns is the
false-sharing penalty; several times slower is normal. The line must migrate
between cores on every access: each write invalidates the other core's copy, so
the two threads ping-pong ownership through the coherence protocol. Nothing in
the source code says the counters are shared. They are not shared. They merely
happen to occupy the same 64 bytes.

The distant-CPU pair is usually worse than the adjacent one, because the line has
further to travel. Comparing the two is a direct measurement of the machine's
topology.

**Why this matters.** False sharing is one of the most common causes of parallel
code that gets slower as you add threads, and it is invisible in the source. The
fix is padding or per-thread state, which is exactly why `alignas(64)` appears
throughout concurrent runtimes and kernel data structures.

---

## Exercises

These use only the shipped CLI. Each produces a report you can point at.

1. **Find the caches without being told.** Run with `--only-sweeps`. From the
   latency curve alone, estimate where each cache ends. Then compare with the
   reported geometry table. Where do your estimates disagree, and why might the
   curve bend before the reported boundary?

2. **Latency versus bandwidth.** At a working set beyond the last-level cache,
   compare dependent latency with sequential read bandwidth. Compute how many
   accesses must be in flight for the observed bandwidth to be possible at the
   observed latency. That number is the machine's memory-level parallelism.

3. **Why writes cost more.** Compare the read and write curves inside L1 and in
   main memory. Explain the gap using read-for-ownership. Predict what a
   non-temporal store would change.

4. **Page size and TLB reach.** On Linux, record the TLB curve, then change
   `/sys/kernel/mm/transparent_hugepage/enabled` and record it again. Explain
   the shift in terms of TLB entries and coverage.

5. **Conflict misses.** Compare the conflict knee at a stride that matches a
   reported cache with one that does not. Explain why the second is noisier,
   using virtual versus physical indexing.

6. **False sharing.** From the coherency table, compute the penalty for both CPU
   pairs. Explain why the distant pair differs from the adjacent one, and what
   that says about the chip's topology.

7. **Is the machine trustworthy?** Run `--repeat 3` on a laptop on battery, then
   again on AC power while idle. Compare the run-to-run comparison report and the
   coefficient-of-variation columns. Decide which of the two sets of numbers you
   would be willing to publish.

8. **Cross-machine comparison.** Collect reports from an old and a new machine
   and overlay them with `cachescope_compare`. Identify one measure where the
   old machine is *not* far behind, and explain why memory latency has improved
   so much less than bandwidth or clock speed over the years.

---

## Common pitfalls when writing your own microbenchmarks

CacheScope is built to avoid these, and every one of them has produced a
published-looking wrong number at some point:

- **The optimizer deletes the work.** If a kernel's result is unused or
  recomputable, the loop disappears. Consume every result and make each
  repetition observably different.
- **Measuring throughput and calling it latency.** Without a dependency chain,
  the CPU overlaps misses and you measure parallelism.
- **Timing something shorter than the clock.** The report prints the observed
  clock resolution. Each sample must be far longer than it, which is why the
  harness calibrates a repeat count instead of timing single operations.
- **Forgetting warm-up.** The first pass pays for page faults and cold caches.
- **One sample.** Report a median across repeats, and a spread alongside it.
- **Ignoring the machine's state.** Frequency scaling, thermal limits, other
  processes and virtualization all move the numbers, so record them with the
  measurement. The report carries a warnings section about this.
