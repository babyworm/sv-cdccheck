# CLAUDE.md — sv-cdccheck

## Build
make build   # Release (fetches slang v10.0 via FetchContent)
make test    # 256 test cases, 722 assertions
make debug   # Debug build

## Architecture
6-pass pipeline: ClockTree → FFClassifier → Connectivity → CrossingDetector → SyncVerifier → ReportGenerator
All passes in src/*.cpp with headers in include/sv-cdccheck/*.h
Shared AST utilities in ast_utils.h/cpp

## Key Design Decisions
- ClockSource* pointer equality = same clock domain
- unique_ptr ownership in ClockDatabase, raw pointers for references
- Hash indexes in SyncVerifier for O(1) lookups
- string::find over std::regex for clock/reset name matching

## slang API Quirks
- driver.parseAllSources() MUST be called before createCompilation()
- compilation->getRoot() is non-const
- compilation->getAllDiagnostics() forces lazy AST body evaluation
- ProceduralBlockSymbol.getBody() is lazy — force before using
- getPortConnections() returns span<const PortConnection* const> (pointers)
- Temp SV files must NOT be deleted while compilation is alive

## Testing
- Use tests/test_helpers.h for compileSV() helper
- Temp files use static int counter for unique names
- Temp files are intentionally not deleted (slang lazy refs)
- Integration tests use tests/basic/*.sv fixtures with SOURCE_DIR macro

## Dependencies
- slang v10.0 (C++20, FetchContent)
- fmt 12.1.0 (transitive via slang)
- Catch2 v3.7.1 (test only)
- mimalloc 2.2.4 (transitive via slang, optional)
