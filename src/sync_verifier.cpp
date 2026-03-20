#include "slang-cdc/sync_verifier.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace slang_cdc {

SyncVerifier::SyncVerifier(std::vector<CrossingReport>& crossings,
                           const std::vector<std::unique_ptr<FFNode>>& ff_nodes,
                           const std::vector<FFEdge>& edges)
    : crossings_(crossings), ff_nodes_(ff_nodes), edges_(edges) {}

const FFNode* SyncVerifier::findNextFF(const FFNode* ff) const {
    // Find an FF in the same domain that is directly fed by this FF
    // with no combinational logic in between (sync chain characteristic).
    for (auto& edge : edges_) {
        if (edge.source == ff && edge.dest &&
            edge.dest->domain == ff->domain &&
            !edge.has_comb_logic) {
            // Verify single fan-in: the dest FF's fanin should contain
            // only the source FF's leaf name (sync chain characteristic).
            const auto& fanin = edge.dest->fanin_signals;
            std::string source_leaf = ff->hier_path;
            auto dot_pos = source_leaf.rfind('.');
            if (dot_pos != std::string::npos)
                source_leaf = source_leaf.substr(dot_pos + 1);

            if (fanin.empty()) {
                // No fanin info available, accept the edge
                return edge.dest;
            }

            bool single_fanin = (fanin.size() == 1 && fanin[0] == source_leaf);
            if (single_fanin)
                return edge.dest;
        }
    }
    return nullptr;
}

SyncType SyncVerifier::detectSyncPattern(const FFNode* dest_ff) const {
    if (!dest_ff) return SyncType::None;

    // Check for 2-FF synchronizer:
    // dest_ff (first sync stage) -> next_ff (second sync stage)
    // Both must be in the same domain with direct FF-to-FF connection
    const FFNode* second = findNextFF(dest_ff);
    if (!second) return SyncType::None;

    // 2-FF detected! Check for 3-FF
    const FFNode* third = findNextFF(second);
    if (third) return SyncType::ThreeFF;

    return SyncType::TwoFF;
}

const FFEdge* SyncVerifier::findEdge(const std::string& source_signal,
                                      const std::string& dest_signal) const {
    for (auto& edge : edges_) {
        if (edge.source && edge.dest &&
            edge.source->hier_path == source_signal &&
            edge.dest->hier_path == dest_signal) {
            return &edge;
        }
    }
    return nullptr;
}

void SyncVerifier::detectReconvergence() {
    // Group crossings by (source_domain, dest_domain) pair
    // Key: source_domain_name + "|" + dest_domain_name
    std::unordered_map<std::string, std::vector<size_t>> domain_pair_crossings;

    for (size_t i = 0; i < crossings_.size(); ++i) {
        auto& c = crossings_[i];
        if (!c.source_domain || !c.dest_domain) continue;

        std::string key = c.source_domain->canonical_name + "|" +
                          c.dest_domain->canonical_name;
        domain_pair_crossings[key].push_back(i);
    }

    // For any pair with 2+ crossings, flag reconvergence
    for (auto& [key, indices] : domain_pair_crossings) {
        if (indices.size() < 2) continue;

        for (auto idx : indices) {
            auto& c = crossings_[idx];
            // Only downgrade synced crossings to CAUTION, don't touch VIOLATIONs
            if (c.sync_type != SyncType::None) {
                c.category = ViolationCategory::Caution;
                c.severity = Severity::Medium;
                c.id = "CAUTION-" + std::to_string(++caution_counter_);
                c.recommendation = "Reconvergence risk: multiple signals from same "
                    "source domain cross independently. Consider gray code or handshake.";
            }
        }
    }
}

void SyncVerifier::detectCombBeforeSync() {
    for (auto& crossing : crossings_) {
        // Find the edge for this crossing
        const FFEdge* edge = findEdge(crossing.source_signal, crossing.dest_signal);
        if (!edge) continue;

        // If the edge has combinational logic between source and dest FF
        if (edge->has_comb_logic) {
            crossing.category = ViolationCategory::Caution;
            crossing.severity = Severity::Medium;
            if (crossing.id.find("CAUTION") == std::string::npos)
                crossing.id = "CAUTION-" + std::to_string(++caution_counter_);
            crossing.recommendation = "Combinational logic before sync FF introduces "
                "glitch risk. Drive synchronizer input directly from a source-domain FF.";
        }
    }
}

void SyncVerifier::detectResetSyncIssues() {
    // For each FF, check if its async reset originates from a different clock domain.
    // If so, check whether that reset signal is properly synchronized (has a 2-FF
    // sync chain in the crossing list).

    // Build a set of source signals that have synced crossings, keyed by
    // "source_signal|dest_domain_name" to account for which destination
    // domain the sync is for.
    std::unordered_set<std::string> synced_signals;
    for (auto& c : crossings_) {
        if (c.sync_type != SyncType::None && c.dest_domain) {
            synced_signals.insert(c.source_signal + "|" +
                                  c.dest_domain->canonical_name);
        }
    }

    for (auto& ff : ff_nodes_) {
        if (!ff->reset || !ff->reset->is_async || !ff->domain) continue;

        // Find the FF that generates the reset signal
        const FFNode* reset_source_ff = nullptr;
        for (auto& other_ff : ff_nodes_) {
            if (other_ff->hier_path == ff->reset->hier_path ||
                ff->reset->hier_path.ends_with("." + other_ff->hier_path.substr(
                    other_ff->hier_path.rfind('.') + 1))) {
                // Check if it's in a different domain
                if (other_ff->domain && !other_ff->domain->isSameDomain(*ff->domain)) {
                    reset_source_ff = other_ff.get();
                    break;
                }
            }
        }

        if (!reset_source_ff) continue;

        // Check if there's already a synced crossing for this reset signal
        // to this specific destination domain
        std::string sync_key = reset_source_ff->hier_path + "|" +
            (ff->domain ? ff->domain->canonical_name : "");
        bool is_synced = synced_signals.count(sync_key) > 0;

        if (!is_synced) {
            // Create a CAUTION crossing for the unsynchronized reset
            // But first check if we already have a crossing for this pair
            bool already_reported = false;
            for (auto& c : crossings_) {
                if (c.source_signal == reset_source_ff->hier_path &&
                    c.dest_signal == ff->hier_path) {
                    // Update existing crossing
                    c.category = ViolationCategory::Caution;
                    c.severity = Severity::High;
                    c.id = "CAUTION-" + std::to_string(++caution_counter_);
                    c.recommendation = "Async reset from different clock domain without "
                        "reset synchronizer. Use async-assert, sync-deassert pattern.";
                    already_reported = true;
                    break;
                }
            }

            if (!already_reported) {
                // Add a new crossing report for the reset issue
                CrossingReport report;
                report.source_domain = reset_source_ff->domain;
                report.dest_domain = ff->domain;
                report.source_signal = reset_source_ff->hier_path;
                report.dest_signal = ff->hier_path;
                report.sync_type = SyncType::None;
                report.category = ViolationCategory::Caution;
                report.severity = Severity::High;
                report.id = "CAUTION-" + std::to_string(++caution_counter_);
                report.recommendation = "Async reset from different clock domain without "
                    "reset synchronizer. Use async-assert, sync-deassert pattern.";
                crossings_.push_back(std::move(report));
            }
        }
    }
}

void SyncVerifier::analyze() {
    // Phase 1: Detect sync patterns (existing)
    for (auto& crossing : crossings_) {
        // Find the dest FF node for this crossing
        const FFNode* dest_ff = nullptr;
        for (auto& ff : ff_nodes_) {
            if (ff->hier_path == crossing.dest_signal) {
                dest_ff = ff.get();
                break;
            }
        }

        crossing.sync_type = detectSyncPattern(dest_ff);

        // Update category based on sync detection
        if (crossing.sync_type != SyncType::None) {
            // Determine if the sync chain meets the required stages
            int stages = 0;
            if (crossing.sync_type == SyncType::TwoFF) stages = 2;
            else if (crossing.sync_type == SyncType::ThreeFF) stages = 3;
            else stages = required_stages_; // other types always qualify

            if (stages >= required_stages_) {
                crossing.category = ViolationCategory::Info;
                crossing.severity = Severity::Info;
                crossing.recommendation.clear();
                crossing.id = "INFO-" + std::to_string(++info_counter_);
            } else {
                crossing.category = ViolationCategory::Caution;
                crossing.severity = Severity::Medium;
                crossing.id = "CAUTION-" + std::to_string(++caution_counter_);
                crossing.recommendation = "Synchronizer has " +
                    std::to_string(stages) + " stages but " +
                    std::to_string(required_stages_) + " required.";
            }
        }
    }

    // Phase 2: Detect combinational logic before sync FF
    detectCombBeforeSync();

    // Phase 3: Detect reconvergence
    detectReconvergence();

    // Phase 4: Detect reset synchronizer issues
    detectResetSyncIssues();

    // Phase 5: Detect advanced synchronizer patterns (upgrade TwoFF/ThreeFF)
    detectGrayCodePattern();
    detectHandshakePattern();
    detectPulseSyncPattern();

    // Phase 6: Detect fan-out before sync completion
    detectFanoutBeforeSync();
}

// ─── Advanced synchronizer pattern detection ───

/// Extract common prefix from a signal name, stripping numeric suffix/index.
/// e.g. "top.gray_ptr[2]" → "top.gray_ptr"
///      "top.gray_ptr_2"  → "top.gray_ptr_"
static std::string extractPrefix(const std::string& sig) {
    // Strip trailing [N]
    auto bracket = sig.rfind('[');
    if (bracket != std::string::npos && sig.back() == ']')
        return sig.substr(0, bracket);

    // Strip trailing digits
    auto pos = sig.size();
    while (pos > 0 && std::isdigit(static_cast<unsigned char>(sig[pos - 1])))
        --pos;
    if (pos < sig.size() && pos > 0)
        return sig.substr(0, pos);

    return sig;
}

void SyncVerifier::detectGrayCodePattern() {
    // Group synced crossings by (source_domain, dest_domain) pair
    // Key: source_domain_name + "|" + dest_domain_name
    std::unordered_map<std::string, std::vector<size_t>> domain_pair_crossings;

    for (size_t i = 0; i < crossings_.size(); ++i) {
        auto& c = crossings_[i];
        if (!c.source_domain || !c.dest_domain) continue;
        if (c.sync_type != SyncType::TwoFF && c.sync_type != SyncType::ThreeFF) continue;

        std::string key = c.source_domain->canonical_name + "|" +
                          c.dest_domain->canonical_name;
        domain_pair_crossings[key].push_back(i);
    }

    for (auto& [key, indices] : domain_pair_crossings) {
        if (indices.size() < 3) continue;

        // Check if source signals share a common prefix with numeric suffix
        std::unordered_map<std::string, std::vector<size_t>> prefix_groups;
        for (auto idx : indices) {
            std::string prefix = extractPrefix(crossings_[idx].source_signal);
            prefix_groups[prefix].push_back(idx);
        }

        for (auto& [prefix, group_indices] : prefix_groups) {
            if (group_indices.size() < 3) continue;

            // All bits share prefix and all have 2-FF/3-FF sync → GrayCode
            for (auto idx : group_indices) {
                crossings_[idx].sync_type = SyncType::GrayCode;
                crossings_[idx].recommendation = "Gray code synchronizer detected.";
            }
        }
    }
}

void SyncVerifier::detectHandshakePattern() {
    // Build map: "A|B" → list of synced crossing indices
    std::unordered_map<std::string, std::vector<size_t>> pair_map;
    for (size_t i = 0; i < crossings_.size(); ++i) {
        auto& c = crossings_[i];
        if (!c.source_domain || !c.dest_domain) continue;
        if (c.sync_type != SyncType::TwoFF && c.sync_type != SyncType::ThreeFF) continue;

        std::string key = c.source_domain->canonical_name + "|" +
                          c.dest_domain->canonical_name;
        pair_map[key].push_back(i);
    }

    // For each pair A→B, check if B→A also has a synced crossing
    std::unordered_set<size_t> handshake_indices;
    for (auto& [key, indices] : pair_map) {
        auto sep = key.find('|');
        std::string dom_a = key.substr(0, sep);
        std::string dom_b = key.substr(sep + 1);
        if (dom_a == dom_b) continue;

        std::string reverse_key = dom_b + "|" + dom_a;
        auto it = pair_map.find(reverse_key);
        if (it == pair_map.end()) continue;

        // Both directions have synced crossings. Check for req/ack naming.
        bool has_req = false, has_ack = false;
        for (auto idx : indices) {
            auto& src = crossings_[idx].source_signal;
            if (src.find("req") != std::string::npos) has_req = true;
        }
        for (auto idx : it->second) {
            auto& src = crossings_[idx].source_signal;
            if (src.find("ack") != std::string::npos) has_ack = true;
        }

        if (has_req && has_ack) {
            // Strong match: req/ack naming
            for (auto idx : indices) handshake_indices.insert(idx);
            for (auto idx : it->second) handshake_indices.insert(idx);
        } else {
            // General bidirectional sync: also classify as handshake
            for (auto idx : indices) handshake_indices.insert(idx);
            for (auto idx : it->second) handshake_indices.insert(idx);
        }
    }

    for (auto idx : handshake_indices) {
        crossings_[idx].sync_type = SyncType::Handshake;
        crossings_[idx].recommendation = "Handshake synchronizer detected.";
    }
}

void SyncVerifier::detectPulseSyncPattern() {
    // For each crossing with 2-FF sync, check if the sync chain output
    // feeds into a XOR/XNOR with a delayed version (detected via fanin_signals).
    for (auto& crossing : crossings_) {
        if (crossing.sync_type != SyncType::TwoFF &&
            crossing.sync_type != SyncType::ThreeFF)
            continue;

        // Find the dest FF (first sync stage)
        const FFNode* dest_ff = nullptr;
        for (auto& ff : ff_nodes_) {
            if (ff->hier_path == crossing.dest_signal) {
                dest_ff = ff.get();
                break;
            }
        }
        if (!dest_ff) continue;

        // Walk the sync chain to find the last stage
        const FFNode* second = findNextFF(dest_ff);
        if (!second) continue;

        const FFNode* last_sync = second;
        const FFNode* third = findNextFF(second);
        if (third) last_sync = third;

        // Check if any FF downstream of the last sync stage has fanin
        // containing both the last sync stage output AND a delayed version
        // (which indicates XOR edge detection)
        std::string last_leaf = last_sync->hier_path;
        auto dot_pos = last_leaf.rfind('.');
        if (dot_pos != std::string::npos)
            last_leaf = last_leaf.substr(dot_pos + 1);

        for (auto& edge : edges_) {
            if (edge.source != last_sync || !edge.dest) continue;
            if (edge.dest->domain != last_sync->domain) continue;

            const auto& fanin = edge.dest->fanin_signals;
            if (fanin.size() < 2) continue;

            // Check if fanin contains the last sync stage output
            // and another signal (the delayed version for XOR)
            bool has_sync_out = false;
            for (auto& f : fanin) {
                if (f == last_leaf) { has_sync_out = true; break; }
            }

            if (has_sync_out && fanin.size() >= 2) {
                crossing.sync_type = SyncType::PulseSync;
                crossing.recommendation = "Pulse synchronizer detected.";
                break;
            }
        }
    }
}

void SyncVerifier::detectFanoutBeforeSync() {
    for (auto& crossing : crossings_) {
        if (crossing.sync_type != SyncType::TwoFF &&
            crossing.sync_type != SyncType::ThreeFF)
            continue;

        // Find the first sync FF
        const FFNode* first_sync = nullptr;
        for (auto& ff : ff_nodes_) {
            if (ff->hier_path == crossing.dest_signal) {
                first_sync = ff.get();
                break;
            }
        }
        if (!first_sync) continue;

        // Check if the first sync FF's output feeds any FF other than
        // the second sync stage
        int fanout_count = 0;
        for (auto& edge : edges_) {
            if (edge.source == first_sync && edge.dest) {
                fanout_count++;
            }
        }

        // A proper sync chain has exactly 1 fanout (to second stage)
        if (fanout_count > 1) {
            crossing.category = ViolationCategory::Caution;
            crossing.severity = Severity::Medium;
            if (crossing.id.find("CAUTION") == std::string::npos)
                crossing.id = "CAUTION-" + std::to_string(++caution_counter_);
            crossing.recommendation = "Data used before completing sync chain. "
                "First sync FF has multiple fanouts.";
        }
    }
}

} // namespace slang_cdc
