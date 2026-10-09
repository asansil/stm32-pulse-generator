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

Two engines drive the pin. The platform selects one by the group of hooks
it implements:

- **DMA engine** — one timer per output in PWM mode with ARR/CCR preload,
  fed one entry per update event by a circular DMA stream that the library
  refills half at a time. No interrupt per edge, and the latency of every
  change is bounded by a configurable window. Runs fixed count today;
  continuous and scheduled are being migrated to it.
- **Compare engine** — a free-running counter in output-compare toggle
  mode, one compare interrupt per edge. Supports all three modes, and will
  be removed once continuous and scheduled run on the DMA engine.

## Repository layout

- `include/` — public API of the library.
- `src/` — implementation.
- `tests/` — host unit tests (Unity + CTest), no hardware required.
- `examples/` — reference firmware projects that exercise the library on
  real hardware, one per mode, for the STM32F407 Discovery
  (`stm32f407_discovery_fixed_count` on the DMA engine,
  `stm32f407_discovery_continuous` and `stm32f407_discovery_scheduled_isr`
  on the compare engine). Within each example, `Core/` is STM32CubeMX-owned
  (regenerated from the `.ioc`, hand edits only inside `USER CODE`
  blocks) and `App/` holds the hand-written integration code (e.g. the
  real `pulse_generator_ops_t`), organized in cohesive subfolders.
- `docs/` — reference material (frequency/resolution spreadsheet) and
  bench notes from hardware validation.

## Building and running the tests (host)

Requires CMake 3.15+ and a C compiler (no cross toolchain needed for tests).

```sh
cmake -S . -B build -DPULSEGEN_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

## License

TBD.
