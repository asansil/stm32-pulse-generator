# stm32-pulse-generator

> **Status: v2 in development.** The library is being rewritten from scratch:
> HAL-independent, testable on host, multi-instance. The previous
> HAL-dependent version is still available in the project history on
> `main`.

HAL-independent C library for generating pulses on a pin using a timer in
Output Compare / Toggle mode, plus bit-banging fallback modes. Designed to
be portable across MCUs through a platform abstraction layer, and to be
fully testable on a host machine without hardware.

## Repository layout

- `include/` — public API of the library.
- `src/` — implementation.
- `tests/` — host unit tests (Unity + CTest), no hardware required.
- `examples/` — reference firmware projects that exercise the library on
  real hardware.
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
