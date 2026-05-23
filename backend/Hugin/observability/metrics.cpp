#include "metrics.hpp"

#include <sstream>

namespace hugin::observability {

std::string IngestMetrics::render_prometheus() const {
    std::ostringstream out;
    out << "# TYPE hugin_frames_received_total counter\n";
    out << "hugin_frames_received_total " << frames_received.load() << '\n';
    out << "# TYPE hugin_frames_committed_total counter\n";
    out << "hugin_frames_committed_total " << frames_committed.load() << '\n';
    out << "# TYPE hugin_frames_duplicate_total counter\n";
    out << "hugin_frames_duplicate_total " << frames_duplicate.load() << '\n';
    out << "# TYPE hugin_frames_gap_total counter\n";
    out << "hugin_frames_gap_total " << frames_gap.load() << '\n';
    out << "# TYPE hugin_frames_validation_failed_total counter\n";
    out << "hugin_frames_validation_failed_total " << frames_validation_failed.load() << '\n';
    out << "# TYPE hugin_frames_storage_failed_total counter\n";
    out << "hugin_frames_storage_failed_total " << frames_storage_failed.load() << '\n';
    out << "# TYPE hugin_sink_failures_total counter\n";
    out << "hugin_sink_failures_total " << sink_failures.load() << '\n';
    return out.str();
}

} // namespace hugin::observability
