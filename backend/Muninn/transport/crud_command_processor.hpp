#ifndef MUNINN_TRANSPORT_CRUD_COMMAND_PROCESSOR_HPP
#define MUNINN_TRANSPORT_CRUD_COMMAND_PROCESSOR_HPP

#include <mutex>
#include <string_view>

#include "../../shared/crud_protocol.hpp"
#include "crud_server.hpp"

namespace muninn::transport {

class CrudCommandProcessor final {
public:
    CrudCommandProcessor(const CrudServerOptions& options,
                         storage::LmdbStore& store,
                         api::IndexingPipeline& pipeline,
                         std::mutex& pipeline_mutex);

    backend::shared::crud::CrudResultMessage process(const backend::shared::crud::CrudCommandMessage& command,
                                                     std::string_view consumer_id) const;

private:
    CrudServerOptions options_ {};
    storage::LmdbStore* store_ {nullptr};
    api::IndexingPipeline* pipeline_ {nullptr};
    std::mutex* pipeline_mutex_ {nullptr};
};

} // namespace muninn::transport

#endif // MUNINN_TRANSPORT_CRUD_COMMAND_PROCESSOR_HPP
