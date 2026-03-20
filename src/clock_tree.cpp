#include "slang-cdc/clock_tree.h"
#include "slang/ast/symbols/CompilationUnitSymbols.h"
#include "slang/ast/symbols/BlockSymbols.h"
#include "slang/ast/TimingControl.h"
#include "slang/ast/statements/MiscStatements.h"
#include "slang/ast/expressions/MiscExpressions.h"
#include "slang/ast/SemanticFacts.h"
#include "slang/ast/Expression.h"
#include "slang/ast/Statement.h"

#include <algorithm>
#include <regex>

namespace slang_cdc {

ClockTreeAnalyzer::ClockTreeAnalyzer(slang::ast::Compilation& compilation,
                                     ClockDatabase& clock_db)
    : compilation_(compilation), clock_db_(clock_db) {}

void ClockTreeAnalyzer::loadSdc(const SdcConstraints& sdc) {
    sdc_ = sdc;
}

void ClockTreeAnalyzer::analyze() {
    // Phase 1a: Identify clock sources
    if (sdc_) {
        importSdcClocks();
    }
    autoDetectClockPorts();

    // Phase 1b: Propagate through hierarchy
    propagateFromRoot();

    // Phase 1c: Register relationships
    if (sdc_) {
        importSdcRelationships();
    }
    inferRelationships();
}

// ── Phase 1a: Source identification ──

void ClockTreeAnalyzer::importSdcClocks() {
    for (auto& clk : sdc_->clocks) {
        auto src = std::make_unique<ClockSource>();
        src->id = "sdc_" + clk.name;
        src->name = clk.name;
        src->type = ClockSource::Type::Primary;
        src->period_ns = clk.period;
        src->origin_signal = clk.target;
        clock_db_.addSource(std::move(src));
    }

    for (auto& gen : sdc_->generated_clocks) {
        auto src = std::make_unique<ClockSource>();
        src->id = "sdc_gen_" + gen.name;
        src->name = gen.name;
        src->type = ClockSource::Type::Generated;
        src->origin_signal = gen.target;
        src->divide_by = gen.divide_by;
        src->multiply_by = gen.multiply_by;
        src->invert = gen.invert;

        // Link to master source
        for (auto& existing : clock_db_.sources) {
            if (existing->origin_signal == gen.source_clock ||
                existing->name == gen.source_clock) {
                src->master = existing.get();
                break;
            }
        }
        clock_db_.addSource(std::move(src));
    }
}

void ClockTreeAnalyzer::autoDetectClockPorts() {
    auto& root = compilation_.getRoot();
    for (auto& member : root.members()) {
        if (member.kind != slang::ast::SymbolKind::Instance)
            continue;

        auto& inst = member.as<slang::ast::InstanceSymbol>();
        for (auto& port_member : inst.body.members()) {
            if (port_member.kind != slang::ast::SymbolKind::Port)
                continue;

            auto& port = port_member.as<slang::ast::PortSymbol>();
            std::string port_name(port.name);

            if (isClockName(port_name)) {
                // Check if SDC already defined this clock
                bool already_defined = false;
                for (auto& src : clock_db_.sources) {
                    if (src->origin_signal == port_name) {
                        already_defined = true;
                        break;
                    }
                }
                if (!already_defined) {
                    auto src = std::make_unique<ClockSource>();
                    src->id = "auto_" + port_name;
                    src->name = port_name;
                    src->type = ClockSource::Type::AutoDetected;
                    src->origin_signal = port_name;
                    clock_db_.addSource(std::move(src));
                }
            }
        }
    }
}

// ── Phase 1b: Hierarchical propagation ──

// Extract signal name from an expression (NamedValueExpression)
static std::string extractSignalNameFromExpr(const slang::ast::Expression& expr) {
    if (expr.kind == slang::ast::ExpressionKind::NamedValue) {
        auto& named = expr.as<slang::ast::NamedValueExpression>();
        return std::string(named.symbol.name);
    }
    return "";
}

void ClockTreeAnalyzer::propagateFromRoot() {
    // Build initial net map from known sources at top level
    std::unordered_map<std::string, ClockNet*> top_nets;
    for (auto& src : clock_db_.sources) {
        auto net = std::make_unique<ClockNet>();
        net->hier_path = src->origin_signal;
        net->source = src.get();
        auto* net_ptr = clock_db_.addNet(std::move(net));
        top_nets[src->origin_signal] = net_ptr;
    }

    auto& root = compilation_.getRoot();
    for (auto& member : root.members()) {
        if (member.kind == slang::ast::SymbolKind::Instance) {
            propagateInstance(member.as<slang::ast::InstanceSymbol>(), top_nets);
        }
    }
}

void ClockTreeAnalyzer::propagateInstance(
    const slang::ast::InstanceSymbol& inst,
    const std::unordered_map<std::string, ClockNet*>& parent_nets)
{
    std::unordered_map<std::string, ClockNet*> local_nets;

    auto port_connections = inst.getPortConnections();

    if (port_connections.empty()) {
        // Root-level instance: no port connections from parent.
        // Seed local_nets directly from parent_nets — the port names at the
        // module boundary correspond to top-level clock source names.
        for (auto& [name, net] : parent_nets) {
            local_nets[name] = net;
        }
    } else {
        // Map port connections: resolve the actual expression to find parent clock net
        for (auto* conn : port_connections) {
            if (!conn) continue;

            auto& port = conn->port;
            std::string port_name(port.name);

            ClockNet* parent_clock_net = nullptr;

            // Resolve the actual signal name from the connection expression
            auto* expr = conn->getExpression();
            if (expr) {
                std::string actual_signal = extractSignalNameFromExpr(*expr);
                if (!actual_signal.empty()) {
                    auto it = parent_nets.find(actual_signal);
                    if (it != parent_nets.end()) {
                        parent_clock_net = it->second;
                    }
                }
            }

            if (parent_clock_net) {
                auto net = std::make_unique<ClockNet>();
                net->hier_path = std::string(inst.name) + "." + port_name;
                net->source = parent_clock_net->source; // Same source!
                net->edge = parent_clock_net->edge;
                auto* net_ptr = clock_db_.addNet(std::move(net));
                local_nets[port_name] = net_ptr;
            }
        }
    }

    // Collect clocks from always_ff sensitivity lists in this instance
    collectSensitivityClocks(inst, local_nets);

    // Recurse into child instances
    for (auto& child : inst.body.membersOfType<slang::ast::InstanceSymbol>()) {
        propagateInstance(child, local_nets);
    }
}

void ClockTreeAnalyzer::collectSensitivityClocks(
    const slang::ast::InstanceSymbol& inst,
    std::unordered_map<std::string, ClockNet*>& local_nets)
{
    for (auto& member : inst.body.members()) {
        if (member.kind != slang::ast::SymbolKind::ProceduralBlock)
            continue;

        auto& block = member.as<slang::ast::ProceduralBlockSymbol>();
        if (block.procedureKind != slang::ast::ProceduralBlockKind::AlwaysFF &&
            block.procedureKind != slang::ast::ProceduralBlockKind::Always)
            continue;

        auto& body = block.getBody();
        if (body.kind != slang::ast::StatementKind::Timed)
            continue;

        auto& timed = body.as<slang::ast::TimedStatement>();
        auto& timing = timed.timing;

        // Extract events from sensitivity list
        std::vector<std::pair<std::string, Edge>> events;

        auto processEvent = [&](const slang::ast::TimingControl& tc) {
            if (tc.kind != slang::ast::TimingControlKind::SignalEvent)
                return;
            auto& sec = tc.as<slang::ast::SignalEventControl>();
            std::string sig_name = extractSignalNameFromExpr(sec.expr);
            if (sig_name.empty()) return;
            Edge edge = (sec.edge == slang::ast::EdgeKind::PosEdge) ?
                Edge::Posedge : Edge::Negedge;
            events.push_back({sig_name, edge});
        };

        if (timing.kind == slang::ast::TimingControlKind::SignalEvent) {
            processEvent(timing);
        } else if (timing.kind == slang::ast::TimingControlKind::EventList) {
            auto& list = timing.as<slang::ast::EventListControl>();
            for (auto* ev : list.events) {
                if (ev) processEvent(*ev);
            }
        }

        // Classify: find the clock signal (not a reset)
        for (auto& [sig_name, edge] : events) {
            bool looks_clock = isClockName(sig_name);
            bool looks_reset = isResetName(sig_name);

            // Skip resets
            if (looks_reset && !looks_clock)
                continue;

            // If this signal is already a known local net, skip
            if (local_nets.count(sig_name))
                continue;

            // Check if there's already a source for this clock
            ClockSource* found_source = nullptr;
            for (auto& src : clock_db_.sources) {
                if (src->origin_signal == sig_name || src->name == sig_name) {
                    found_source = src.get();
                    break;
                }
            }

            // If no source found and it looks like a clock, create auto-detected
            if (!found_source && looks_clock) {
                auto src = std::make_unique<ClockSource>();
                src->id = "auto_sens_" + sig_name;
                src->name = sig_name;
                src->type = ClockSource::Type::AutoDetected;
                src->origin_signal = sig_name;
                found_source = clock_db_.addSource(std::move(src));
            }

            if (found_source) {
                auto net = std::make_unique<ClockNet>();
                net->hier_path = std::string(inst.name) + "." + sig_name;
                net->source = found_source;
                net->edge = edge;
                auto* net_ptr = clock_db_.addNet(std::move(net));
                local_nets[sig_name] = net_ptr;
            }
        }
    }
}

// ── Phase 1c: Relationship registration ──

void ClockTreeAnalyzer::importSdcRelationships() {
    for (auto& group : sdc_->clock_groups) {
        DomainRelationship::Type rel_type;
        switch (group.type) {
            case SdcClockGroup::Type::Asynchronous:
                rel_type = DomainRelationship::Type::Asynchronous; break;
            case SdcClockGroup::Type::Exclusive:
                rel_type = DomainRelationship::Type::PhysicallyExclusive; break;
            case SdcClockGroup::Type::LogicallyExclusive:
                rel_type = DomainRelationship::Type::PhysicallyExclusive; break;
        }

        // Register pairwise relationships between groups
        for (size_t i = 0; i < group.groups.size(); i++) {
            for (size_t j = i + 1; j < group.groups.size(); j++) {
                for (auto& name_a : group.groups[i]) {
                    for (auto& name_b : group.groups[j]) {
                        ClockSource* src_a = nullptr;
                        ClockSource* src_b = nullptr;
                        for (auto& s : clock_db_.sources) {
                            if (s->name == name_a) src_a = s.get();
                            if (s->name == name_b) src_b = s.get();
                        }
                        if (src_a && src_b) {
                            clock_db_.relationships.push_back(
                                {src_a, src_b, rel_type});
                        }
                    }
                }
            }
        }
    }
}

void ClockTreeAnalyzer::inferRelationships() {
    // For sources sharing a master: mark as Divided
    // For unrelated auto-detected sources: conservatively mark as Asynchronous
    for (size_t i = 0; i < clock_db_.sources.size(); i++) {
        for (size_t j = i + 1; j < clock_db_.sources.size(); j++) {
            auto* a = clock_db_.sources[i].get();
            auto* b = clock_db_.sources[j].get();

            // Skip if relationship already defined (e.g., from SDC)
            bool already_defined = false;
            for (auto& rel : clock_db_.relationships) {
                if ((rel.a == a && rel.b == b) || (rel.a == b && rel.b == a)) {
                    already_defined = true;
                    break;
                }
            }
            if (already_defined) continue;

            // Same master → related/divided
            if (a->master && a->master == b->master) {
                clock_db_.relationships.push_back(
                    {a, b, DomainRelationship::Type::Divided});
            } else if (a->master == b || b->master == a) {
                clock_db_.relationships.push_back(
                    {a, b, DomainRelationship::Type::Divided});
            } else {
                // Different sources, no known relationship → assume async
                clock_db_.relationships.push_back(
                    {a, b, DomainRelationship::Type::Asynchronous});
            }
        }
    }
}

// ── Helpers ──

bool ClockTreeAnalyzer::isClockName(const std::string& name) {
    static std::regex clock_re(
        R"((^|_)(clk|clock|ck)($|_))", std::regex::icase);
    return std::regex_search(name, clock_re);
}

bool ClockTreeAnalyzer::isResetName(const std::string& name) {
    static std::regex reset_re(
        R"((^|_)(rst|reset|rstn|rst_n)($|_))", std::regex::icase);
    return std::regex_search(name, reset_re);
}

} // namespace slang_cdc
