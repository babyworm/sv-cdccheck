# slang-cdc

Open-source structural CDC (Clock Domain Crossing) analysis tool for SystemVerilog RTL designs.

Built on [slang](https://github.com/MikePopoloski/slang) — a fast, compliant SystemVerilog compiler library.

## Why

CDC bugs cause non-deterministic metastability failures that are nearly impossible to reproduce in simulation. Commercial CDC tools cost $100K+/year and are inaccessible to small teams, academic users, and open-source silicon projects.

**slang-cdc** provides structural CDC analysis using slang's elaborated design representation — detecting cross-domain paths, verifying synchronizer patterns, and generating actionable reports.

## Features

### Clock Analysis
- Clock source auto-detection from port naming patterns
- SDC constraint parsing (`create_clock`, `create_generated_clock`, `set_clock_groups`)
- YAML clock specification (`--clock-yaml`)
- Clock propagation tracking through module hierarchy (same source, different names)
- Async reset domain tracking

### FF Classification & Connectivity
- `always_ff` and legacy `always @(posedge)` FF detection
- `always_latch` flagged as warning
- Inter-module FF-to-FF connectivity through port connections
- Combinational path tracking (continuous `assign` resolution)
- Fan-in signal collection per FF

### Synchronizer Detection
- 2-FF / 3-FF synchronizer chain recognition
- Gray code pattern (multi-bit with per-bit sync)
- Handshake (REQ/ACK) bidirectional sync pattern
- Pulse synchronizer (toggle + edge detect)
- Configurable required sync stages (`--sync-stages`)

### Quality Checks
- Reconvergence detection (multiple independent crossings from same source domain)
- Combinational logic before sync FF (glitch risk)
- Fan-out before sync completion
- Reset synchronizer verification (async assert, sync deassert)

### Reporting & Integration
- Markdown, JSON, SDC, and DOT (Graphviz) output formats
- Waiver mechanism (YAML) with exact match and glob patterns
- VIOLATION / CAUTION / CONVENTION / INFO / WAIVED categories
- Exit code = violation count (CI compatible, capped at 255)
- `--strict` mode (CAUTION counted as VIOLATION)

## Quick Start

### Prerequisites

- C++20 compiler (GCC 11+, Clang 17+, Apple Xcode 16+)
- CMake 3.20+
- Python 3 (required by slang code generation)

### Build

```bash
git clone https://github.com/babyworm/slang-cdc.git
cd slang-cdc
make build    # fetches slang v10.0 automatically via CMake FetchContent
```

### Run

```bash
# Basic usage
./build/slang-cdc --top my_soc rtl/*.sv

# With SDC constraints
./build/slang-cdc --top my_soc rtl/*.sv --sdc constraints/clocks.sdc

# With YAML clock spec
./build/slang-cdc --top my_soc rtl/*.sv --clock-yaml clock_domains.yaml

# With waivers
./build/slang-cdc --top my_soc rtl/*.sv --waiver cdc_waivers.yaml

# CI mode (strict, JSON only)
./build/slang-cdc --top my_soc rtl/*.sv --format json --strict

# DOT graph export
./build/slang-cdc --top my_soc rtl/*.sv --dump-graph cdc_graph.dot

# Help
./build/slang-cdc --help
```

### Test

```bash
make test     # 126 tests, 400 assertions
```

### Install

```bash
make install              # installs to ~/.local/bin
INSTALL_PREFIX=/usr/local make install  # custom prefix
```

## CLI Options

```
slang-cdc [OPTIONS] <SV_FILES...>

Required:
  <SV_FILES...>           SystemVerilog source files
  --top <module>          Top-level module name

Output:
  -o, --output <dir>      Output directory (default: ./cdc_reports/)
  --format <fmt>          md|json|sdc|all (default: all)
  --dump-graph <file>     Export DOT graph to file

Clock specification:
  --sdc <file>            SDC file with clock definitions
  --clock-yaml <file>     YAML file with clock domain relationships
  --auto-clocks           Auto-detect clocks from port names (default)

Analysis control:
  --waiver <file>         Waiver file (YAML) for known crossings
  --sync-stages <n>       Required synchronizer stages (default: 2)
  --ignore-gated          Don't report gated-clock crossings
  --strict                Treat CAUTION as VIOLATION

Slang options (pass-through):
  -I <dir>                Include directory
  -D <macro>=<val>        Define preprocessor macro
  --std <ver>             SystemVerilog standard version
```

**Note:** Custom options like `--verbose` may conflict with slang's internal flags. Use `--quiet` (`-q`) for minimal output.

## Architecture

```
SV RTL files
    | slang C++ API
Elaborated Design (full hierarchy)
    | Pass 1
Clock Tree Analysis (sources, propagation, SDC/YAML import)
    | Pass 2
FF Classification (every FF -> clock domain, reset tracking)
    | Pass 3
Connectivity Graph (FF-to-FF data paths through hierarchy)
    | Pass 4
Cross-Domain Detection (async/related classification)
    | Pass 5
Synchronizer Verification (2-FF, gray, handshake, pulse, quality checks)
    | Pass 6
Report Generation (Markdown + JSON + SDC + DOT)
```

## Violation Categories

| Category | Meaning |
|----------|---------|
| `VIOLATION` | No synchronizer on async crossing |
| `CAUTION` | Synchronizer exists but has quality issue (reconvergence, glitch path, fan-out) |
| `CONVENTION` | Non-standard clock/reset naming |
| `INFO` | Properly synchronized crossing |
| `WAIVED` | User-waived crossing |

## Makefile Targets

| Target | Description |
|--------|-------------|
| `make deps` | Fetch slang + dependencies via CMake FetchContent |
| `make build` | Release build |
| `make debug` | Debug build |
| `make test` | Run test suite (126 tests) |
| `make install` | Install binary |
| `make clean` | Remove build directories |

## License

[MIT](LICENSE) — Hyun-Gyu Kim

See [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md) for dependency licenses.

## References

- Clifford Cummings, "Clock Domain Crossing (CDC) Design & Verification Techniques" (SNUG 2008)
- [slang documentation](https://sv-lang.com)
- [slang source](https://github.com/MikePopoloski/slang)
- [OpenTitan CDC methodology](https://opentitan.org/book/hw/methodology/clock_domain_crossing.html)
