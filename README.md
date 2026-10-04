# stm32-pulse-generator

> **Status: v2 in development.** The library is being rewritten from scratch:
> HAL-independent, testable on host, multi-instance. The previous
> HAL-dependent version is still available in the project history on
> `main`.

HAL-independent C library for generating pulses on a pin using a timer in
Output Compare / Toggle mode. Designed to be portable across MCUs through a
platform abstraction layer, and to be fully testable on a host machine
without hardware.

Three generation modes share that one mechanism and differ only in how the
instants of the pin toggles are decided:

- **Fixed count** — exactly N pulses at a fixed frequency, stopping on its
  own.
- **Continuous** — pulses at a frequency that can be updated on the fly,
  until explicitly stopped.
- **Scheduled** — an arbitrary sequence of pulses at caller-specified
  instants, streamed through a queue so the sequence need never be held in
  memory in full. *(In design; see the roadmap.)*

## Repository layout

- `include/` — public API of the library.
- `src/` — implementation.
- `tests/` — host unit tests (Unity + CTest), no hardware required.
- `examples/` — reference firmware projects that exercise the library on
  real hardware. Within each example, `Core/` is STM32CubeMX-owned
  (regenerated from the `.ioc`, hand edits only inside `USER CODE`
  blocks) and `App/` holds the hand-written integration code (e.g. the
  real `pulse_generator_ops_t`), organized in cohesive subfolders.
- `docs/` — reference material (frequency/resolution spreadsheet).

## Building and running the tests (host)

Requires CMake 3.15+ and a C compiler (no cross toolchain needed for tests).

```sh
cmake -S . -B build -DPULSEGEN_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

## License

TBD.
