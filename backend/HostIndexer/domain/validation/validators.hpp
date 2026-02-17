#ifndef HOSTINDEXER_VALIDATORS_HPP
#define HOSTINDEXER_VALIDATORS_HPP

#include <memory>
#include <vector>

#include "Ivalidator.hpp"

namespace host_indexer::validation {

class SnapshotHeaderValidationPass final : public ISnapshotValidationPass {
public:
    [[nodiscard]] std::string_view name() const noexcept override;
    void run(const domain::Snapshot& snapshot, ValidationCollector& collector) const override;
};

class EntryIdentityValidationPass final : public ISnapshotValidationPass {
public:
    [[nodiscard]] std::string_view name() const noexcept override;
    void run(const domain::Snapshot& snapshot, ValidationCollector& collector) const override;
};

class PathNormalizationValidationPass final : public ISnapshotValidationPass {
public:
    [[nodiscard]] std::string_view name() const noexcept override;
    void run(const domain::Snapshot& snapshot, ValidationCollector& collector) const override;
};

class ParentGraphValidationPass final : public ISnapshotValidationPass {
public:
    [[nodiscard]] std::string_view name() const noexcept override;
    void run(const domain::Snapshot& snapshot, ValidationCollector& collector) const override;
};

class TimestampSanityValidationPass final : public ISnapshotValidationPass {
public:
    [[nodiscard]] std::string_view name() const noexcept override;
    void run(const domain::Snapshot& snapshot, ValidationCollector& collector) const override;
};

class DeltaReferentialIntegrityPass final : public IDeltaValidationPass {
public:
    [[nodiscard]] std::string_view name() const noexcept override;
    void run(const domain::DeltaSnapshot& delta,
             const domain::Snapshot* base_snapshot,
             const domain::Snapshot* target_snapshot,
             ValidationCollector& collector) const override;
};

class SnapshotValidator {
public:
    SnapshotValidator();
    explicit SnapshotValidator(std::vector<std::unique_ptr<ISnapshotValidationPass>> passes);

    [[nodiscard]] ValidationReport validate(const domain::Snapshot& snapshot) const;

private:
    std::vector<std::unique_ptr<ISnapshotValidationPass>> passes_;
};

class DeltaValidator {
public:
    DeltaValidator();
    explicit DeltaValidator(std::vector<std::unique_ptr<IDeltaValidationPass>> passes);

    [[nodiscard]] ValidationReport validate(const domain::DeltaSnapshot& delta,
                                            const domain::Snapshot* base_snapshot = nullptr,
                                            const domain::Snapshot* target_snapshot = nullptr) const;

private:
    std::vector<std::unique_ptr<IDeltaValidationPass>> passes_;
};

} // namespace host_indexer::validation

#endif // HOSTINDEXER_VALIDATORS_HPP
