#pragma once

#include "slang-cdc/types.h"

namespace slang_cdc {

/// Pass 5: Synchronizer verification — pattern matching on crossing paths
///
/// For each crossing, examines the destination-domain FFs to detect
/// synchronizer patterns (2-FF, 3-FF, etc.).
/// Updates crossing reports with sync_type and adjusts category accordingly.
/// Also detects: reconvergence, combinational-before-sync, reset sync issues.
class SyncVerifier {
public:
    SyncVerifier(std::vector<CrossingReport>& crossings,
                 const std::vector<std::unique_ptr<FFNode>>& ff_nodes,
                 const std::vector<FFEdge>& edges);

    void analyze();

    /// Set minimum required synchronizer stages (default: 2).
    /// A crossing with fewer stages than required is not downgraded to INFO.
    void setRequiredStages(int n) { required_stages_ = n; }

private:
    std::vector<CrossingReport>& crossings_;
    const std::vector<std::unique_ptr<FFNode>>& ff_nodes_;
    const std::vector<FFEdge>& edges_;

    /// Check if dest FF is the start of a 2-FF or 3-FF sync chain
    SyncType detectSyncPattern(const FFNode* dest_ff) const;

    /// Find downstream FF connected to given FF in the same domain
    const FFNode* findNextFF(const FFNode* ff) const;

    /// Find the FFEdge that connects source to dest (for comb logic check)
    const FFEdge* findEdge(const std::string& source_signal,
                           const std::string& dest_signal) const;

    /// Post-processing: flag reconvergence when multiple signals from the
    /// same source domain cross to the same dest domain independently
    void detectReconvergence();

    /// Post-processing: flag combinational logic before first sync FF
    void detectCombBeforeSync();

    /// Post-processing: check async resets crossing domains without reset sync
    void detectResetSyncIssues();

    int info_counter_ = 0;
    int caution_counter_ = 0;
    int required_stages_ = 2;
};

} // namespace slang_cdc
