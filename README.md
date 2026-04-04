> [!IMPORTANT]
> **이 프로젝트는 [svlens](https://github.com/babyworm/svlens) 프로젝트에 통합되었습니다. 이 저장소는 더 이상 유지보수되지 않으며 아카이브될 예정입니다. 앞으로는 svlens를 이용해 주세요.**
>
> **svlens**는 SystemVerilog RTL 코드에 대한 구조적 CDC(Clock Domain Crossing) 분석과 연결성 검사를 포함한 simplified structural lint 도구입니다.
>
> **This project has been merged into [svlens](https://github.com/babyworm/svlens). This repository is no longer maintained and will be archived. Please use svlens going forward.**
>
> **svlens** is a simplified structural lint tool for SystemVerilog RTL, covering structural CDC analysis as well as connectivity checks.

# sv-cdccheck

Open-source structural CDC (Clock Domain Crossing) analysis tool for SystemVerilog RTL designs.

Built on [slang](https://github.com/MikePopoloski/slang) — a fast, compliant SystemVerilog compiler library.

## Why

CDC bugs cause non-deterministic metastability failures that are nearly impossible to reproduce in simulation. Commercial CDC tools cost $100K+/year and are inaccessible to small teams, academic users, and open-source silicon projects.

**sv-cdccheck** provides structural CDC analysis using slang's elaborated design representation — detecting cross-domain paths, verifying synchronizer patterns, and generating actionable reports.

## Features

### Clock Analysis
- Clock source auto-detection from port naming patterns
- SDC constraint parsing (`create_clock`, `create_generated_clock`, `set_clock_groups`)
- YAML clock specification (`--clock-yaml`)
- PLL/MMCM/DCM module recognition
- Clock divider and clock gate (ICG) detection
- Clock propagation tracking through module hierarchy (same source, different names)
- Async reset domain tracking

### FF Classification & Connectivity
- `always_ff` and legacy `always @(posedge)` FF detection
- Library cell FF recognition (DFF, SDFF, FDRE patterns)
- `always_latch` flagged as warning
- Multi-clock edge FF flagged as error
- Generate block support
- Inter-module FF-to-FF connectivity through port connections
- Combinational path tracking (continuous `assign` resolution)
- Fan-in signal collection per FF

### Synchronizer Detection
- 2-FF / 3-FF synchronizer chain recognition
- Gray code pattern (multi-bit with per-bit sync)
- Handshake (REQ/ACK) bidirectional sync pattern
- Async FIFO (gray-coded pointer sync)
- MUX synchronizer (synced select signal)
- Pulse synchronizer (toggle + edge detect)
- Johnson counter recognition (valid non-power-of-2 technique)
- Configurable required sync stages (`--sync-stages`)

### Quality Checks
- Reconvergence detection (multiple independent crossings from same source domain)
- Combinational logic before sync FF (glitch risk)
- Fan-out before sync completion
- Reset synchronizer verification (async assert, sync deassert)
- Non-power-of-2 FIFO depth warning

### Reporting & Integration
- Markdown, JSON, SDC, DOT (Graphviz), and waiver template output
- Waiver mechanism (YAML) with exact match and glob patterns
- VIOLATION / CAUTION / CONVENTION / INFO / WAIVED categories
- Exit code = violation count (CI compatible, capped at 255)
- `--strict` mode (CAUTION counted as VIOLATION)
- GitHub Actions CI workflow included

## Build

### Prerequisites

- C++20 compiler (GCC 11+, Clang 17+, Apple Xcode 16+)
- CMake 3.20+
- Python 3 (required by slang code generation)

### Build from source

```bash
git clone https://github.com/babyworm/sv-cdccheck.git
cd sv-cdccheck
make build
```

`make build` runs CMake configure + build. On first run, it automatically downloads slang v10.0 and all dependencies via CMake FetchContent. Subsequent builds are incremental.

### Makefile targets

```
$ make help

sv-cdccheck build targets:
  make deps        Fetch slang + dependencies via CMake FetchContent
  make build       Release build (default)
  make debug       Debug build
  make test        Run test suite
  make install     Install to ~/.local/bin (override: INSTALL_PREFIX=...)
  make clean       Remove build directories
  make rust-check  Check Rust/slang-rs availability

Variables:
  JOBS=N           Parallel build jobs (default: auto-detect)
  INSTALL_PREFIX   Install path (default: ~/.local)
```

### Install

```bash
make install                          # installs to ~/.local/bin/sv-cdccheck
INSTALL_PREFIX=/usr/local make install  # custom prefix
```

### Test

```bash
make test     # 257 tests, ~720 assertions
```

## Usage

### Help

```
$ ./build/sv-cdccheck --help

sv-cdccheck v0.1.2 — Structural CDC Analysis Tool

Usage: sv-cdccheck [OPTIONS] <SV_FILES...>

Required:
  <SV_FILES...>           SystemVerilog source files
  --top <module>          Top-level module name

Output:
  -o, --output <dir>      Output directory (default: ./cdc_reports/)
  --format <fmt>          md|json|sdc|waiver|all (default: all)
  --dump-graph <file>     Export DOT graph to file

Options:
  --sdc <file>            SDC file with clock definitions
  --clock-yaml <file>     Clock specification YAML file
  --waiver <file>         Waiver YAML file
  --sync-stages <n>       Required synchronizer stages (default: 2)
  --strict                Treat CAUTION as VIOLATION in exit code
  --ignore-gated          Skip gated-clock crossings from report
  --auto-clocks           Auto-detect clocks (default)
  -v, --verbose           Detailed output
  -q, --quiet             Only violations and summary
  --version               Show version
  -h, --help              Show this help
```

Slang pass-through options (`-I`, `-D`, `--std`, `--top`, etc.) are forwarded directly to slang's compiler driver.

### Examples

```bash
# Basic: analyze a design, detect CDC violations
./build/sv-cdccheck --top soc_top rtl/soc_top.sv rtl/subsystem.sv

# With include path and defines
./build/sv-cdccheck --top soc_top -I rtl/include -D SYNTHESIS rtl/*.sv

# With SDC clock constraints
./build/sv-cdccheck --top soc_top rtl/*.sv --sdc syn/clocks.sdc

# With YAML clock specification
./build/sv-cdccheck --top soc_top rtl/*.sv --clock-yaml clock_domains.yaml

# Apply waivers for known-safe crossings
./build/sv-cdccheck --top soc_top rtl/*.sv --waiver cdc_waivers.yaml

# Require 3-stage synchronizers (high-frequency designs)
./build/sv-cdccheck --top soc_top rtl/*.sv --sync-stages 3

# CI mode: JSON only, strict (CAUTION = failure), exit code = violations
./build/sv-cdccheck --top soc_top rtl/*.sv --format json --strict -q

# Export connectivity graph for visualization
./build/sv-cdccheck --top soc_top rtl/*.sv --dump-graph cdc_graph.dot
dot -Tpng cdc_graph.dot -o cdc_graph.png   # requires Graphviz

# Verbose mode: show clock sources, FF counts, edge counts
./build/sv-cdccheck --top soc_top rtl/*.sv -v
```

### Output files

By default (`--format all`), sv-cdccheck writes to `./cdc_reports/`:

| File | Format | Content |
|------|--------|---------|
| `cdc_report.md` | Markdown | Human-readable summary, domain table, crossing details |
| `cdc_report.json` | JSON | Machine-readable with category, severity, sync_type per crossing |
| `cdc_report.sdc` | SDC | `set_false_path` / `set_max_delay` constraints for synced crossings |
| `cdc_waivers.yaml` | YAML | Waiver template for VIOLATION crossings (fill in reason/owner) |

### Example output

```
$ ./build/sv-cdccheck --top missing_sync tests/basic/02_missing_sync.sv

sv-cdccheck: Design elaborated successfully.
  FFs detected: 2

  === CDC Summary ===
  VIOLATION:  1
  CAUTION:    0
  CONVENTION: 0
  INFO:       0
  WAIVED:     0

  Reports written to: ./cdc_reports/
```

Exit code is 1 (= 1 violation). In CI, a non-zero exit code fails the build.

## Architecture

```
SV RTL files
    | slang C++ API
Elaborated Design (full hierarchy)
    | Pass 1
Clock Tree Analysis (sources, PLL/divider/gate, SDC/YAML import, propagation)
    | Pass 2
FF Classification (every FF -> clock domain, reset tracking, latch/error detection)
    | Pass 3
Connectivity Graph (FF-to-FF data paths through hierarchy, assign tracing)
    | Pass 4
Cross-Domain Detection (async/related/convention classification)
    | Pass 5
Synchronizer Verification (8 patterns, 5 quality checks)
    | Pass 6
Report Generation (Markdown + JSON + SDC + DOT + Waiver template)
```

## Violation Categories

| Category | Meaning | Exit code |
|----------|---------|-----------|
| `VIOLATION` | No synchronizer on async crossing | Counted |
| `CAUTION` | Sync exists but quality issue (reconvergence, glitch, fan-out, non-pow2 FIFO) | Counted with `--strict` |
| `CONVENTION` | Non-standard clock/reset naming | Not counted |
| `INFO` | Properly synchronized crossing | Not counted |
| `WAIVED` | User-waived crossing | Not counted |

## Known Limitations

- SDC period data is used for report output only, not for crossing classification
- Cross-instance assign chains are partially supported
- Incremental analysis is not yet implemented

## License

[MIT](LICENSE) — Hyun-Gyu Kim

See [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md) for dependency licenses.

## References

- Clifford Cummings, "Clock Domain Crossing (CDC) Design & Verification Techniques" (SNUG 2008)
- [slang documentation](https://sv-lang.com)
- [slang source](https://github.com/MikePopoloski/slang)
- [OpenTitan CDC methodology](https://opentitan.org/book/hw/methodology/clock_domain_crossing.html)
