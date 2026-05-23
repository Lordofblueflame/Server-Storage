#ifndef HUGIN_OBSERVABILITY_METRICS_HPP
#define HUGIN_OBSERVABILITY_METRICS_HPP

#include <atomic>
#include <cstdint>
#include <string>

namespace hugin::observability {

struct IngestMetrics {
    std::atomic<std::uint64_t> frames_received {0};
    std::atomic<std::uint64_t> frames_committed {0};
    std::atomic<std::uint64_t> frames_duplicate {0};
    std::atomic<std::uint64_t> frames_gap {0};
    std::atomic<std::uint64_t> frames_validation_failed {0};
    std::atomic<std::uint64_t> frames_storage_failed {0};
    std::atomic<std::uint64_t> sink_failures {0};

    [[nodiscard]] std::string render_prometheus() const;
};

} // namespace hugin::observability

#endif // HUGIN_OBSERVABILITY_METRICS_HPP
