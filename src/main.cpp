#include <iostream>
#include <filesystem>
#include <string>
#include <vector>

#include "slang/driver/Driver.h"
#include "slang/ast/Compilation.h"
#include "slang/ast/symbols/CompilationUnitSymbols.h"
#include "slang/ast/symbols/InstanceSymbols.h"

#include "slang-cdc/types.h"
#include "slang-cdc/clock_tree.h"
#include "slang-cdc/sdc_parser.h"
#include "slang-cdc/ff_classifier.h"
#include "slang-cdc/connectivity.h"
#include "slang-cdc/crossing_detector.h"
#include "slang-cdc/sync_verifier.h"
#include "slang-cdc/report_generator.h"

namespace fs = std::filesystem;

static void printUsage() {
    std::cout << "slang-cdc v0.1.0 — Structural CDC Analysis Tool\n\n"
              << "Usage: slang-cdc [OPTIONS] <SV_FILES...>\n\n"
              << "Required:\n"
              << "  <SV_FILES...>           SystemVerilog source files\n"
              << "  --top <module>          Top-level module name\n\n"
              << "Output:\n"
              << "  -o, --output <dir>      Output directory (default: ./cdc_reports/)\n"
              << "  --format <fmt>          md|json|all (default: all)\n\n"
              << "Options:\n"
              << "  --sdc <file>            SDC file with clock definitions\n"
              << "  -v, --verbose           Detailed output\n"
              << "  -q, --quiet             Only violations and summary\n"
              << "  --version               Show version\n"
              << "  -h, --help              Show this help\n";
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printUsage();
        return 1;
    }

    // Pre-scan for our custom args before passing to slang
    std::string output_dir = "./cdc_reports";
    std::string format = "all";
    std::string sdc_file;
    bool quiet = false;
    bool verbose = false;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--version") {
            std::cout << "slang-cdc 0.1.0\n";
            return 0;
        }
        if (arg == "-h" || arg == "--help") {
            printUsage();
            return 0;
        }
        if ((arg == "-o" || arg == "--output") && i + 1 < argc)
            output_dir = argv[++i];
        else if (arg == "--format" && i + 1 < argc)
            format = argv[++i];
        else if (arg == "--sdc" && i + 1 < argc)
            sdc_file = argv[++i];
        else if (arg == "-q" || arg == "--quiet")
            quiet = true;
        else if (arg == "-v" || arg == "--verbose")
            verbose = true;
    }

    // Use slang's Driver for SV argument parsing and compilation
    slang::driver::Driver driver;
    driver.addStandardArgs();

    if (!driver.parseCommandLine(argc, argv))
        return 1;

    if (!driver.processOptions())
        return 1;

    if (!driver.parseAllSources())
        return 1;

    auto compilation = driver.createCompilation();
    compilation->getRoot();
    compilation->getAllDiagnostics();

    if (!quiet)
        std::cout << "slang-cdc: Design elaborated successfully.\n";

    // ─── Pass 1: Clock Tree Analysis ───
    slang_cdc::ClockDatabase clock_db;
    slang_cdc::ClockTreeAnalyzer clock_analyzer(*compilation, clock_db);

    if (!sdc_file.empty()) {
        if (verbose)
            std::cout << "  Loading SDC: " << sdc_file << "\n";
        auto sdc = slang_cdc::SdcParser::parse(sdc_file);
        clock_analyzer.loadSdc(sdc);
    }

    clock_analyzer.analyze();

    if (verbose) {
        std::cout << "  Clock sources: " << clock_db.sources.size() << "\n";
        for (auto& src : clock_db.sources)
            std::cout << "    " << src->name << " (" << src->origin_signal << ")\n";
    }

    // ─── Pass 2: FF Classification ───
    slang_cdc::FFClassifier classifier(*compilation, clock_db);
    classifier.analyze();

    if (!quiet)
        std::cout << "  FFs detected: " << classifier.getFFNodes().size() << "\n";

    // ─── Pass 3: Connectivity Graph ───
    slang_cdc::ConnectivityBuilder connectivity(*compilation, classifier.getFFNodes());
    connectivity.analyze();

    if (verbose)
        std::cout << "  FF-to-FF edges: " << connectivity.getEdges().size() << "\n";

    // ─── Pass 4: Cross-Domain Detection ───
    slang_cdc::CrossingDetector detector(connectivity.getEdges(), clock_db);
    detector.analyze();
    auto crossings = detector.getCrossings();

    // ─── Pass 5: Synchronizer Verification ───
    slang_cdc::SyncVerifier verifier(crossings, classifier.getFFNodes(),
                                     connectivity.getEdges());
    verifier.analyze();

    // ─── Pass 6: Report Generation ───
    slang_cdc::AnalysisResult result;
    result.clock_db = std::move(clock_db);
    result.crossings = std::move(crossings);

    fs::create_directories(output_dir);

    slang_cdc::ReportGenerator report(result);

    if (format == "md" || format == "all")
        report.generateMarkdown(fs::path(output_dir) / "cdc_report.md");

    if (format == "json" || format == "all")
        report.generateJSON(fs::path(output_dir) / "cdc_report.json");

    // ─── Summary ───
    if (!quiet) {
        std::cout << "\n  === CDC Summary ===\n";
        std::cout << "  VIOLATION: " << result.violation_count() << "\n";
        std::cout << "  CAUTION:   " << result.caution_count() << "\n";
        std::cout << "  INFO:      " << result.info_count() << "\n";
        std::cout << "\n  Reports written to: " << output_dir << "/\n";
    }

    // Exit code = violation count (for CI)
    return result.violation_count();
}
