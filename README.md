# stm32-pulse-generator

> **Status: v2 in development.** The library is being rewritten from scratch:
> HAL-independent, testable on host, multi-instance. The previous
> HAL-dependent version is still available in the project history on
> `main`.

HAL-independent C library for generating pulses on a pin from a hardware
timer. Designed to be portable across MCUs through a platform abstraction
layer, and to be fully testable on a host machine without hardware.

Three generation modes:

- **Fixed count** — exactly N pulses at a fixed frequency, stopping on its
  own.
- **Continuous** — pulses at a frequency that can be updated on the fly,
  until explicitly stopped.
- **Scheduled** — an arbitrary sequence of pulses at caller-specified
  instants, streamed through a queue so the sequence need never be held in
  memory in full.

All three run on a single engine: one timer per output in PWM mode with
ARR/CCR preload, fed one entry per update event by a circular DMA stream
that the library refills half at a time. There is no interrupt per edge,
and the latency of every change is bounded by a configurable window (plus,
for a frequency change, the rest of a period in progress longer than that).
The CPU cost goes per buffer entry rather than per pulse, with a floor of
`entries / window` refilled entries per second even at low rates: a longer
window trades latency for CPU.

## Optional utilities

`include/pulse_generator_util.h` collects helpers for building the
intervals a scheduled queue takes. They are plain tick arithmetic: the core
does not depend on them, and they take the tick rate as a parameter instead
of an instance.

- **Unit conversions** — µs, ns and Hz to ticks, and ticks to µs, rounded
  to the nearest unit, reporting a result that does not fit instead of
  wrapping.
- **Spread** — an iterator that turns "n pulses over T ticks" into n
  intervals that add up to exactly T and differ by at most one tick, so
  chained segments never drift.
- **Timeline** — turns absolute pulse instants, in ns, into intervals,
  rounding each instant rather than each interval, so rounding errors never
  accumulate.

## Repository layout

- `include/` — public API of the library.
- `src/` — implementation.
- `tests/` — host unit tests (Unity + CTest), no hardware required.
- `examples/` — reference firmware projects that exercise the library on
  real hardware, one per mode, for the STM32F407 Discovery
  (`stm32f407_discovery_fixed_count`, `stm32f407_discovery_continuous` and
  `stm32f407_discovery_scheduled_dma`). Within each example, `Core/` is
  STM32CubeMX-owned (regenerated from the `.ioc`, hand edits only inside
  `USER CODE` blocks) and `App/` holds the hand-written integration code
  (e.g. the real `pulse_generator_ops_t`), organized in cohesive
  subfolders.
- `docs/` — reference material (frequency/resolution spreadsheet) and
  bench notes from hardware validation.

## Building and running the tests (host)

No cross toolchain is needed for the tests. With CMake 3.21+, Ninja and
`gcc`, use the `dev` preset, the same one CI runs:

```sh
cmake --preset dev
cmake --build build
ctest --test-dir build --output-on-failure
```

Without the preset (CMake 3.15+ and any C compiler), configure with
`cmake -S . -B build -DPULSE_GENERATOR_BUILD_TESTS=ON` instead.

## License

TBD.
