# QuickApp Examples

Example apps and integration demos for [QuickApp Kit](https://github.com/quickapp-kit).

## Contents

| Directory | Description |
|-----------|-------------|
| `composition/` | C++ composition examples — package loading, JS execution, LVGL rendering |
| `binding-001/` | Data binding example (JS ↔ Native) |
| `baseline-cases/` | Narrow technical RPK baselines for contract validation |
| `showcases/` | User-facing product scenario RPKs |
| `alliance-hap-case001/` | Alliance HAP reference sample (UX source, read-only baseline) |
| `quickapp-code-test2/` | Another sample quick-app project |

## Build (C++ examples)

Requires sibling repos: `quickapp-runtime-core`, `quickapp-runtime-js`, `quickapp-runtime-lvgl`.

```bash
cmake -S . -B build -G Ninja
cmake --build build
```

### Run the LVGL simulator

```bash
./build/quickapp_lvgl_simulator
```

## Quick-app sample projects

```bash
cd alliance-hap-case001
npm install
# Use quickapp-toolkit CLI to build/run
```

## Related

- [quickapp-runtime-core](https://github.com/quickapp-kit/quickapp-runtime-core)
- [quickapp-runtime-js](https://github.com/quickapp-kit/quickapp-runtime-js)
- [quickapp-runtime-lvgl](https://github.com/quickapp-kit/quickapp-runtime-lvgl)
- [quickapp-toolkit](https://github.com/quickapp-kit/quickapp-toolkit)

## License

[MIT](LICENSE)
