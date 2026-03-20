#include "slang-cdc/crossing_detector.h"

namespace slang_cdc {

CrossingDetector::CrossingDetector(const std::vector<FFEdge>& edges,
                                   const ClockDatabase& clock_db)
    : edges_(edges), clock_db_(clock_db) {}

void CrossingDetector::analyze() {
    for (auto& edge : edges_) {
        if (!edge.source || !edge.dest) continue;
        if (!edge.source->domain || !edge.dest->domain) continue;

        // Same domain → no crossing
        if (edge.source->domain->isSameDomain(*edge.dest->domain))
            continue;

        CrossingReport report;
        report.source_domain = edge.source->domain;
        report.dest_domain = edge.dest->domain;
        report.source_signal = edge.source->hier_path;
        report.dest_signal = edge.dest->hier_path;
        report.sync_type = SyncType::None; // Will be updated by SyncVerifier

        // Classify severity based on domain relationship
        bool is_async = clock_db_.isAsynchronous(
            edge.source->domain, edge.dest->domain);

        if (is_async) {
            report.severity = Severity::High;
            report.category = ViolationCategory::Violation;
            report.id = "VIOLATION-" + std::to_string(++violation_counter_);
            report.recommendation = "Insert 2-FF synchronizer at " +
                edge.dest->hier_path;
        } else {
            // Related domains (divided, gated) — lower severity
            report.severity = Severity::Medium;
            report.category = ViolationCategory::Caution;
            report.id = "CAUTION-" + std::to_string(++caution_counter_);
            report.recommendation = "Verify timing constraints for related-clock crossing";
        }

        crossings_.push_back(std::move(report));
    }
}

std::vector<CrossingReport> CrossingDetector::getCrossings() const {
    return crossings_;
}

} // namespace slang_cdc
