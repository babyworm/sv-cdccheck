#include <algorithm>
#include <iostream>
#include <filesystem>
#include <set>
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
#include "slang-cdc/waiver.h"
#include "slang-cdc/clock_yaml_parser.h"

namespace fs = std::filesystem;

static void printUsage() {
    std::cout << "slang-cdc v0.1.0 — Structural CDC Analysis Tool\n\n"
              << "Usage: slang-cdc [OPTIONS] <SV_FILES...>\n\n"
              << "Required:\n"
              << "  <SV_FILES...>           SystemVerilog source files\n"
              << "  --top <module>          Top-level module name\n\n"
              << "Output:\n"
              << "  -o, --output <dir>      Output directory (default: ./cdc_reports/)\n"
              << "  --format <fmt>          md|json|sdc|all (default: all)\n"
              << "  --dump-graph <file>     Export DOT graph to file\n\n"
              << "Options:\n"
              << "  --sdc <file>            SDC file with clock definitions\n"
              << "  --clock-yaml <file>     Clock specification YAML file\n"
              << "  --waiver <file>         Waiver YAML file\n"
              << "  --sync-stages <n>       Required synchronizer stages (default: 2)\n"
              << "  --strict                Treat CAUTION as VIOLATION in exit code\n"
              << "  --ignore-gated          Skip gated-clock crossings (Severity::Low) from report\n"
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
    std::string clock_yaml_file;
    std::string waiver_file;
    std::string dump_graph_file;
    int sync_stages = 2;
    bool strict = false;
    bool quiet = false;
    bool verbose = false;
    bool ignore_gated = false;

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
        else if (arg == "--clock-yaml" && i + 1 < argc)
            clock_yaml_file = argv[++i];
        else if (arg == "--waiver" && i + 1 < argc)
            waiver_file = argv[++i];
        else if (arg == "--dump-graph" && i + 1 < argc)
            dump_graph_file = argv[++i];
        else if (arg == "--sync-stages" && i + 1 < argc)
            sync_stages = std::stoi(argv[++i]);
        else if (arg == "--strict")
            strict = true;
        else if (arg == "--ignore-gated")
            ignore_gated = true;
        else if (arg == "-q" || arg == "--quiet")
            quiet = true;
        else if (arg == "-v" || arg == "--verbose")
            verbose = true;
    }

    // Use slang's Driver for SV argument parsing and compilation
    slang::driver::Driver driver;
    driver.addStandardArgs();

    // Build filtered argv for slang (exclude our custom options)
    std::vector<const char*> slang_argv = {argv[0]};
    std::set<std::string> our_flags = {"-o", "--output", "--format", "--sdc", "--waiver",
        "--dump-graph", "-q", "--quiet", "-v", "--verbose", "--clock-yaml",
        "--sync-stages", "--strict", "--ignore-gated"};
    std::set<std::string> our_flags_with_arg = {"-o", "--output", "--format", "--sdc",
        "--waiver", "--dump-graph", "--clock-yaml", "--sync-stages"};

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--version" || arg == "-h" || arg == "--help")
            continue; // already handled above
        if (our_flags.count(arg)) {
            if (our_flags_with_arg.count(arg) && i + 1 < argc) i++; // skip value
            continue;
        }
        slang_argv.push_back(argv[i]);
    }

    if (!driver.parseCommandLine(static_cast<int>(slang_argv.size()),
                                  const_cast<char**>(slang_argv.data())))
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

    // Load clock YAML specification before analysis
    slang_cdc::ClockYamlParser clock_yaml_parser;
    if (!clock_yaml_file.empty()) {
        if (verbose)
            std::cout << "  Loading clock YAML: " << clock_yaml_file << "\n";
        if (!clock_yaml_parser.loadFile(clock_yaml_file)) {
            std::cerr << "slang-cdc: warning: could not load clock YAML file: "
                      << clock_yaml_file << "\n";
        } else {
            clock_yaml_parser.applyTo(clock_db);
            if (verbose)
                std::cout << "  Clock sources from YAML: "
                          << clock_yaml_parser.getConfig().clock_sources.size() << "\n";
        }
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
    verifier.setRequiredStages(sync_stages);
    verifier.analyze();

    // ─── Apply Waivers ───
    slang_cdc::WaiverManager waiver_mgr;
    if (!waiver_file.empty()) {
        if (verbose)
            std::cout << "  Loading waivers: " << waiver_file << "\n";
        if (!waiver_mgr.loadFile(waiver_file)) {
            std::cerr << "slang-cdc: warning: could not load waiver file: "
                      << waiver_file << "\n";
        } else if (verbose) {
            std::cout << "  Waivers loaded: " << waiver_mgr.getWaivers().size() << "\n";
        }
    }

    for (auto& c : crossings) {
        if (waiver_mgr.isWaived(c.source_signal, c.dest_signal)) {
            c.category = slang_cdc::ViolationCategory::Waived;
        }
    }

    // ─── Filter: remove gated-clock crossings if --ignore-gated ───
    if (ignore_gated) {
        crossings.erase(
            std::remove_if(crossings.begin(), crossings.end(),
                [](const slang_cdc::CrossingReport& c) {
                    return c.severity == slang_cdc::Severity::Low;
                }),
            crossings.end());
    }

    // ─── Pass 6: Report Generation ───
    slang_cdc::AnalysisResult result;
    result.clock_db = std::move(clock_db);
    result.crossings = std::move(crossings);
    result.ff_nodes = classifier.releaseFFNodes();
    result.edges = connectivity.getEdges();

    fs::create_directories(output_dir);

    slang_cdc::ReportGenerator report(result);

    if (format == "md" || format == "all")
        report.generateMarkdown(fs::path(output_dir) / "cdc_report.md");

    if (format == "json" || format == "all")
        report.generateJSON(fs::path(output_dir) / "cdc_report.json");

    if (format == "sdc" || format == "all")
        report.generateSDC(fs::path(output_dir) / "cdc_constraints.sdc");

    if (!dump_graph_file.empty())
        report.generateDOT(dump_graph_file);

    // ─── Summary ───
    if (!quiet) {
        std::cout << "\n  === CDC Summary ===\n";
        std::cout << "  VIOLATION:  " << result.violation_count() << "\n";
        std::cout << "  CAUTION:    " << result.caution_count() << "\n";
        std::cout << "  CONVENTION: " << result.convention_count() << "\n";
        std::cout << "  INFO:       " << result.info_count() << "\n";
        std::cout << "  WAIVED:     " << result.waived_count() << "\n";
        std::cout << "\n  Reports written to: " << output_dir << "/\n";
    }

    // Exit code = violation count (for CI), capped at 255 for POSIX
    int exit_count = result.violation_count();
    if (strict)
        exit_count += result.caution_count();
    return std::min(exit_count, 255);
}
