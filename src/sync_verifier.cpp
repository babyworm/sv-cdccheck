#include "slang-cdc/sync_verifier.h"

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
}

// ─── Future synchronizer pattern detection criteria ───
//
// GrayCode:
//   Detect multi-bit buses where all bits cross the same domain pair.
//   The source side must encode using binary-to-gray conversion (XOR chain),
//   and the destination side must decode with gray-to-binary. Both the
//   encoder output and decoder input should pass through 2-FF synchronizers.
//   Key structural pattern: N-bit register → N XOR gates → N 2-FF syncs.
//
// Handshake:
//   Detect paired crossings: a request signal from domain A→B and an
//   acknowledge signal from domain B→A. Both must be single-bit and have
//   2-FF synchronizers. The request/ack pair forms a closed-loop protocol.
//   Key structural pattern: req FF in domain A, ack FF in domain B,
//   each crossing the other domain with a 2-FF sync chain.
//
// AsyncFIFO:
//   Detect gray-coded write and read pointers crossing between domains.
//   The FIFO must have: (1) write pointer gray-encoded crossing to read
//   domain, (2) read pointer gray-encoded crossing to write domain,
//   (3) full/empty comparators in the respective domains.
//   Key structural pattern: dual-port memory + gray pointer pairs.
//
// MuxSync:
//   Detect a multiplexer whose select signal is synchronized, allowing
//   multi-bit data to cross safely. The data path goes through a MUX
//   controlled by a synced select; the data is stable when select toggles.
//   Key structural pattern: MUX with synced select + hold timing constraint.
//
// PulseSync:
//   Detect a toggle-domain-crossing pulse synchronizer. The source domain
//   toggles a FF on each pulse, the toggle signal is 2-FF synced, and the
//   destination domain XORs consecutive sync outputs to regenerate the pulse.
//   Key structural pattern: toggle FF → 2-FF sync → XOR edge detector.

} // namespace slang_cdc
