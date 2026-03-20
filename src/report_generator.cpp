#include "slang-cdc/report_generator.h"
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <functional>

namespace slang_cdc {

int AnalysisResult::violation_count() const {
    int count = 0;
    for (auto& c : crossings)
        if (c.category == ViolationCategory::Violation) count++;
    return count;
}

int AnalysisResult::caution_count() const {
    int count = 0;
    for (auto& c : crossings)
        if (c.category == ViolationCategory::Caution) count++;
    return count;
}

int AnalysisResult::info_count() const {
    int count = 0;
    for (auto& c : crossings)
        if (c.category == ViolationCategory::Info) count++;
    return count;
}

int AnalysisResult::waived_count() const {
    int count = 0;
    for (auto& c : crossings)
        if (c.category == ViolationCategory::Waived) count++;
    return count;
}

int AnalysisResult::convention_count() const {
    int count = 0;
    for (auto& c : crossings)
        if (c.category == ViolationCategory::Convention) count++;
    return count;
}

ClockSource* ClockDatabase::addSource(std::unique_ptr<ClockSource> src) {
    auto* ptr = src.get();
    sources.push_back(std::move(src));
    return ptr;
}

ClockNet* ClockDatabase::addNet(std::unique_ptr<ClockNet> net) {
    auto* ptr = net.get();
    net_by_path[net->hier_path] = ptr;
    nets.push_back(std::move(net));
    return ptr;
}

ClockDomain* ClockDatabase::findOrCreateDomain(ClockSource* source, Edge edge) {
    for (auto& d : domains) {
        if (d->source == source && d->edge == edge)
            return d.get();
    }
    auto dom = std::make_unique<ClockDomain>();
    dom->canonical_name = source->name;
    dom->source = source;
    dom->edge = edge;
    auto* ptr = dom.get();
    domain_by_name[dom->canonical_name] = ptr;
    domains.push_back(std::move(dom));
    return ptr;
}

ClockDomain* ClockDatabase::domainForSignal(const std::string& hier_path) const {
    auto it = net_by_path.find(hier_path);
    if (it == net_by_path.end()) return nullptr;
    auto* net = it->second;
    // Find matching domain
    for (auto& d : domains) {
        if (d->source == net->source && d->edge == net->edge)
            return d.get();
    }
    return nullptr;
}

bool ClockDatabase::isAsynchronous(const ClockDomain* a, const ClockDomain* b) const {
    if (!a || !b) return true; // unknown -> conservative
    if (a->source == b->source) return false; // same source

    for (auto& rel : relationships) {
        if ((rel.a == a->source && rel.b == b->source) ||
            (rel.a == b->source && rel.b == a->source)) {
            return rel.relationship == DomainRelationship::Type::Asynchronous;
        }
    }
    return true; // no relationship found -> assume async
}

ReportGenerator::ReportGenerator(const AnalysisResult& result)
    : result_(result) {}

std::string ReportGenerator::jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char ch : s) {
        switch (ch) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (ch < 0x20) {
                    // Control character: \uXXXX
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", ch);
                    out += buf;
                } else {
                    out += static_cast<char>(ch);
                }
                break;
        }
    }
    return out;
}

void ReportGenerator::generateMarkdown(const std::filesystem::path& output_path) const {
    std::ofstream out(output_path);
    out << "# CDC Analysis Report\n\n";
    out << "## Summary\n\n";
    out << "| Category | Count |\n";
    out << "|----------|-------|\n";
    out << "| VIOLATION | " << result_.violation_count() << " |\n";
    out << "| CAUTION | " << result_.caution_count() << " |\n";
    out << "| CONVENTION | " << result_.convention_count() << " |\n";
    out << "| INFO | " << result_.info_count() << " |\n";
    out << "| WAIVED | " << result_.waived_count() << " |\n\n";

    out << "## Clock Domains\n\n";
    out << "| Domain | Source | Type | Edge |\n";
    out << "|--------|--------|------|------|\n";
    for (auto& d : result_.clock_db.domains) {
        out << "| " << d->canonical_name
            << " | " << d->source->name
            << " | ";
        switch (d->source->type) {
            case ClockSource::Type::Primary: out << "primary"; break;
            case ClockSource::Type::Generated: out << "generated"; break;
            case ClockSource::Type::Virtual: out << "virtual"; break;
            case ClockSource::Type::AutoDetected: out << "auto"; break;
        }
        out << " | " << (d->edge == Edge::Posedge ? "posedge" : "negedge")
            << " |\n";
    }

    out << "\n## Crossings\n\n";
    for (auto& c : result_.crossings) {
        out << "### " << c.id << ": "
            << (c.source_domain ? c.source_domain->canonical_name : "?")
            << " -> "
            << (c.dest_domain ? c.dest_domain->canonical_name : "?")
            << "\n";
        out << "- Source: " << c.source_signal << "\n";
        out << "- Dest: " << c.dest_signal << "\n";
        if (!c.path.empty()) {
            out << "- Path: ";
            for (size_t i = 0; i < c.path.size(); i++) {
                out << c.path[i];
                if (i + 1 < c.path.size()) out << " -> ";
            }
            out << "\n";
        }
        if (!c.recommendation.empty())
            out << "- Fix: " << c.recommendation << "\n";
        out << "\n";
    }
}

void ReportGenerator::generateJSON(const std::filesystem::path& output_path) const {
    std::ofstream out(output_path);
    out << "{\n";
    out << "  \"summary\": {\n";
    out << "    \"violations\": " << result_.violation_count() << ",\n";
    out << "    \"cautions\": " << result_.caution_count() << ",\n";
    out << "    \"conventions\": " << result_.convention_count() << ",\n";
    out << "    \"info\": " << result_.info_count() << ",\n";
    out << "    \"waived\": " << result_.waived_count() << "\n";
    out << "  },\n";

    out << "  \"domains\": [\n";
    for (size_t i = 0; i < result_.clock_db.domains.size(); i++) {
        auto& d = result_.clock_db.domains[i];
        out << "    {\"name\": \"" << jsonEscape(d->canonical_name)
            << "\", \"source\": \"" << jsonEscape(d->source->name) << "\"}";
        if (i + 1 < result_.clock_db.domains.size()) out << ",";
        out << "\n";
    }
    out << "  ],\n";

    out << "  \"crossings\": [\n";
    for (size_t i = 0; i < result_.crossings.size(); i++) {
        auto& c = result_.crossings[i];
        out << "    {\"id\": \"" << jsonEscape(c.id)
            << "\", \"source\": \"" << jsonEscape(c.source_signal)
            << "\", \"dest\": \"" << jsonEscape(c.dest_signal)
            << "\", \"source_domain\": \""
            << jsonEscape(c.source_domain ? c.source_domain->canonical_name : "")
            << "\", \"dest_domain\": \""
            << jsonEscape(c.dest_domain ? c.dest_domain->canonical_name : "")
            << "\"";

        // Path field
        out << ", \"path\": [";
        for (size_t j = 0; j < c.path.size(); j++) {
            out << "\"" << jsonEscape(c.path[j]) << "\"";
            if (j + 1 < c.path.size()) out << ", ";
        }
        out << "]";

        // Recommendation
        out << ", \"recommendation\": \"" << jsonEscape(c.recommendation) << "\"";

        out << "}";
        if (i + 1 < result_.crossings.size()) out << ",";
        out << "\n";
    }
    out << "  ]\n";
    out << "}\n";
}

void ReportGenerator::generateDOT(const std::filesystem::path& output_path) const {
    std::ofstream out(output_path);
    out << "digraph CDC {\n";
    out << "  rankdir=LR;\n";
    out << "  node [shape=box, style=filled];\n\n";

    // Assign colors to domains
    static const char* palette[] = {
        "\"#A3CEF1\"", "\"#E8D5B7\"", "\"#B5EAD7\"", "\"#FFD6E0\"",
        "\"#C3B1E1\"", "\"#FFEAA7\"", "\"#DFE6E9\"", "\"#FAB1A0\""
    };
    constexpr int palette_size = 8;

    std::unordered_map<std::string, int> domain_color_idx;
    int color_counter = 0;
    for (auto& d : result_.clock_db.domains) {
        domain_color_idx[d->canonical_name] = color_counter % palette_size;
        color_counter++;
    }

    // Sanitize node name for DOT (replace dots with underscores)
    auto sanitize = [](const std::string& s) -> std::string {
        std::string out;
        for (char c : s) {
            out += (c == '.' || c == '[' || c == ']') ? '_' : c;
        }
        return out;
    };

    // Emit FF nodes
    for (auto& ff : result_.ff_nodes) {
        std::string node_id = sanitize(ff->hier_path);
        std::string color = "\"#DFE6E9\""; // default grey
        if (ff->domain) {
            auto it = domain_color_idx.find(ff->domain->canonical_name);
            if (it != domain_color_idx.end()) {
                color = palette[it->second];
            }
        }
        out << "  " << node_id << " [label=\"" << ff->hier_path << "\"";
        out << ", fillcolor=" << color;
        if (ff->domain)
            out << ", tooltip=\"domain: " << ff->domain->canonical_name << "\"";
        out << "];\n";
    }

    out << "\n";

    // Emit edges
    for (auto& edge : result_.edges) {
        if (!edge.source || !edge.dest) continue;
        std::string src_id = sanitize(edge.source->hier_path);
        std::string dst_id = sanitize(edge.dest->hier_path);

        bool is_crossing = false;
        if (edge.source->domain && edge.dest->domain) {
            is_crossing = !edge.source->domain->isSameDomain(*edge.dest->domain);
        }

        out << "  " << src_id << " -> " << dst_id;
        if (is_crossing) {
            out << " [color=red, penwidth=2.0, label=\"CDC\"]";
        }
        out << ";\n";
    }

    out << "}\n";
}

} // namespace slang_cdc
