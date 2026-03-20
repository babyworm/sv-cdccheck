#include "slang-cdc/ff_classifier.h"
#include "slang-cdc/clock_tree.h"

#include "slang/ast/symbols/CompilationUnitSymbols.h"
#include "slang/ast/symbols/InstanceSymbols.h"
#include "slang/ast/symbols/BlockSymbols.h"
#include "slang/ast/symbols/VariableSymbols.h"
#include "slang/ast/TimingControl.h"
#include "slang/ast/statements/MiscStatements.h"
#include "slang/ast/expressions/MiscExpressions.h"
#include "slang/ast/expressions/AssignmentExpressions.h"
#include "slang/ast/Expression.h"
#include "slang/ast/Statement.h"
#include "slang/ast/statements/ConditionalStatements.h"
#include "slang/ast/SemanticFacts.h"

namespace slang_cdc {

FFClassifier::FFClassifier(slang::ast::Compilation& compilation,
                           ClockDatabase& clock_db)
    : compilation_(compilation), clock_db_(clock_db) {}

// Extract the signal name from an Expression (typically NamedValueExpression)
static std::string extractSignalName(const slang::ast::Expression& expr) {
    if (expr.kind == slang::ast::ExpressionKind::NamedValue) {
        auto& named = expr.as<slang::ast::NamedValueExpression>();
        return std::string(named.symbol.name);
    }
    return "";
}

// Information extracted from one SignalEventControl
struct EventInfo {
    std::string signal_name;
    bool is_posedge = false;
    bool is_negedge = false;
};

// Parse a single SignalEventControl into EventInfo
static EventInfo parseSignalEvent(const slang::ast::SignalEventControl& sec) {
    EventInfo info;
    info.signal_name = extractSignalName(sec.expr);
    info.is_posedge = (sec.edge == slang::ast::EdgeKind::PosEdge);
    info.is_negedge = (sec.edge == slang::ast::EdgeKind::NegEdge);
    return info;
}

// Extract all signal events from a TimingControl (handles both single event and event list)
static std::vector<EventInfo> extractEvents(const slang::ast::TimingControl& timing) {
    std::vector<EventInfo> events;

    if (timing.kind == slang::ast::TimingControlKind::SignalEvent) {
        events.push_back(parseSignalEvent(timing.as<slang::ast::SignalEventControl>()));
    }
    else if (timing.kind == slang::ast::TimingControlKind::EventList) {
        auto& list = timing.as<slang::ast::EventListControl>();
        for (auto* ev : list.events) {
            if (ev && ev->kind == slang::ast::TimingControlKind::SignalEvent) {
                events.push_back(parseSignalEvent(ev->as<slang::ast::SignalEventControl>()));
            }
        }
    }
    return events;
}

// Classify events into clock and reset(s)
struct SensitivityInfo {
    std::string clock_name;
    Edge clock_edge = Edge::Posedge;
    std::string reset_name;
    bool reset_is_async = false;
    ResetSignal::Polarity reset_polarity = ResetSignal::Polarity::ActiveLow;
};

static SensitivityInfo classifyEvents(const std::vector<EventInfo>& events) {
    SensitivityInfo info;

    // Heuristic: in always_ff @(posedge clk or negedge rst_n),
    // the clock is the posedge signal with a clock-like name,
    // the reset is the other signal.
    for (auto& ev : events) {
        bool looks_like_clock = ClockTreeAnalyzer::isClockName(ev.signal_name);
        bool looks_like_reset = ClockTreeAnalyzer::isResetName(ev.signal_name);

        if (looks_like_clock && !looks_like_reset) {
            info.clock_name = ev.signal_name;
            info.clock_edge = ev.is_posedge ? Edge::Posedge : Edge::Negedge;
        }
        else if (looks_like_reset && !looks_like_clock) {
            info.reset_name = ev.signal_name;
            info.reset_is_async = true; // in sensitivity list = async reset
            info.reset_polarity = ev.is_negedge ?
                ResetSignal::Polarity::ActiveLow : ResetSignal::Polarity::ActiveHigh;
        }
    }

    // Fallback: if no clock found by name, use the first posedge signal
    if (info.clock_name.empty()) {
        for (auto& ev : events) {
            if (ev.is_posedge && !ClockTreeAnalyzer::isResetName(ev.signal_name)) {
                info.clock_name = ev.signal_name;
                info.clock_edge = Edge::Posedge;
                break;
            }
        }
    }

    // Fallback: if still no clock, use first event
    if (info.clock_name.empty() && !events.empty()) {
        info.clock_name = events[0].signal_name;
        info.clock_edge = events[0].is_posedge ? Edge::Posedge : Edge::Negedge;
    }

    return info;
}

// Collect variable names assigned in a statement (the FF registers)
static void collectAssignedVars(const slang::ast::Statement& stmt,
                                std::vector<std::string>& vars) {
    switch (stmt.kind) {
        case slang::ast::StatementKind::ExpressionStatement: {
            auto& exprStmt = stmt.as<slang::ast::ExpressionStatement>();
            auto& expr = exprStmt.expr;
            if (expr.kind == slang::ast::ExpressionKind::Assignment) {
                auto& assign = expr.as<slang::ast::AssignmentExpression>();
                std::string name = extractSignalName(assign.left());
                if (!name.empty()) {
                    // Avoid duplicates
                    if (std::find(vars.begin(), vars.end(), name) == vars.end())
                        vars.push_back(name);
                }
            }
            break;
        }
        case slang::ast::StatementKind::Timed: {
            auto& timed = stmt.as<slang::ast::TimedStatement>();
            collectAssignedVars(timed.stmt, vars);
            break;
        }
        case slang::ast::StatementKind::Block: {
            auto& block = stmt.as<slang::ast::BlockStatement>();
            collectAssignedVars(block.body, vars);
            break;
        }
        case slang::ast::StatementKind::List: {
            auto& list = stmt.as<slang::ast::StatementList>();
            for (auto* child : list.list)
                if (child) collectAssignedVars(*child, vars);
            break;
        }
        case slang::ast::StatementKind::Conditional: {
            auto& cond = stmt.as<slang::ast::ConditionalStatement>();
            collectAssignedVars(cond.ifTrue, vars);
            if (cond.ifFalse)
                collectAssignedVars(*cond.ifFalse, vars);
            break;
        }
        default: break;
    }
}

// Walk an instance and extract FFs from always_ff blocks
static void processInstance(const slang::ast::InstanceSymbol& inst,
                            const std::string& prefix,
                            ClockDatabase& clock_db,
                            std::vector<std::unique_ptr<FFNode>>& ff_nodes,
                            std::vector<LatchWarning>& latch_warnings) {
    std::string inst_path = prefix.empty() ?
        std::string(inst.name) : prefix + "." + std::string(inst.name);

    for (auto& member : inst.body.members()) {
        if (member.kind == slang::ast::SymbolKind::ProceduralBlock) {
            auto& block = member.as<slang::ast::ProceduralBlockSymbol>();

            // Flag latches as warnings (spec 4.2.3)
            if (block.procedureKind == slang::ast::ProceduralBlockKind::AlwaysLatch) {
                latch_warnings.push_back({inst_path,
                    "always_latch detected — not a proper FF for CDC analysis"});
                continue;
            }

            if (block.procedureKind != slang::ast::ProceduralBlockKind::AlwaysFF &&
                block.procedureKind != slang::ast::ProceduralBlockKind::Always)
                continue;

            auto& body = block.getBody();

            // The body of always_ff is typically a TimedStatement
            const slang::ast::TimingControl* timing = nullptr;
            const slang::ast::Statement* inner_stmt = nullptr;

            if (body.kind == slang::ast::StatementKind::Timed) {
                auto& timed = body.as<slang::ast::TimedStatement>();
                timing = &timed.timing;
                inner_stmt = &timed.stmt;
            }

            if (!timing) continue;

            // Extract clock and reset from sensitivity list
            auto events = extractEvents(*timing);
            auto sens = classifyEvents(events);

            if (sens.clock_name.empty()) continue;

            // Find or create the domain for this clock
            ClockDomain* domain = nullptr;
            for (auto& src : clock_db.sources) {
                if (src->origin_signal == sens.clock_name ||
                    src->name == sens.clock_name) {
                    domain = clock_db.findOrCreateDomain(src.get(), sens.clock_edge);
                    break;
                }
            }

            // If clock not found in db, create an auto-detected source
            if (!domain) {
                auto src = std::make_unique<ClockSource>();
                src->id = "auto_ff_" + sens.clock_name;
                src->name = sens.clock_name;
                src->type = ClockSource::Type::AutoDetected;
                src->origin_signal = sens.clock_name;
                auto* src_ptr = clock_db.addSource(std::move(src));
                domain = clock_db.findOrCreateDomain(src_ptr, sens.clock_edge);
            }

            // Create reset signal if present
            ResetSignal* reset_ptr = nullptr;
            if (!sens.reset_name.empty()) {
                auto reset = std::make_unique<ResetSignal>();
                reset->hier_path = inst_path + "." + sens.reset_name;
                reset->is_async = sens.reset_is_async;
                reset->polarity = sens.reset_polarity;
                reset_ptr = reset.get();
                clock_db.resets.push_back(std::move(reset));
            }

            // Collect variables assigned in this always_ff → these are FFs
            // For now, scan the instance body for variables used in this block
            // Simple approach: every variable in the instance body that is
            // driven by this always_ff is an FF
            // We get assigned vars from the statements
            std::vector<std::string> assigned_vars;
            if (inner_stmt)
                collectAssignedVars(*inner_stmt, assigned_vars);

            if (assigned_vars.empty()) {
                // Fallback: create a single FF node for the entire block
                auto ff = std::make_unique<FFNode>();
                ff->hier_path = inst_path + ".__always_ff_" +
                    std::to_string(ff_nodes.size());
                ff->domain = domain;
                ff->reset = reset_ptr;
                ff_nodes.push_back(std::move(ff));
            } else {
                for (auto& var_name : assigned_vars) {
                    auto ff = std::make_unique<FFNode>();
                    ff->hier_path = inst_path + "." + var_name;
                    ff->domain = domain;
                    ff->reset = reset_ptr;
                    ff_nodes.push_back(std::move(ff));
                }
            }
        }

        // Recurse into child instances
        if (member.kind == slang::ast::SymbolKind::Instance) {
            processInstance(member.as<slang::ast::InstanceSymbol>(),
                          inst_path, clock_db, ff_nodes, latch_warnings);
        }
    }
}

void FFClassifier::analyze() {
    auto& root = compilation_.getRoot();

    for (auto& member : root.members()) {
        if (member.kind == slang::ast::SymbolKind::Instance) {
            processInstance(member.as<slang::ast::InstanceSymbol>(),
                          "", clock_db_, ff_nodes_, latch_warnings_);
        }
    }
}

const std::vector<std::unique_ptr<FFNode>>& FFClassifier::getFFNodes() const {
    return ff_nodes_;
}

const std::vector<LatchWarning>& FFClassifier::getLatchWarnings() const {
    return latch_warnings_;
}

} // namespace slang_cdc
