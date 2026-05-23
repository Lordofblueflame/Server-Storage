#ifndef HUGIN_API_COMMAND_LOAD_BALANCER_HPP
#define HUGIN_API_COMMAND_LOAD_BALANCER_HPP

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "../common/status.hpp"
#include "shared/crud_protocol.hpp"

namespace hugin::api {

using CommandDispatchHandler = std::function<common::Status(const backend::shared::crud::CrudCommandMessage&)>;

struct CommandLoadBalancerOptions {
    std::size_t worker_threads {4U};
    std::size_t max_queue_per_worker {1024U};
    std::chrono::milliseconds execution_timeout {1'500};
};

class CommandLoadBalancer final {
public:
    CommandLoadBalancer(CommandDispatchHandler handler, CommandLoadBalancerOptions options = {});
    ~CommandLoadBalancer();

    CommandLoadBalancer(const CommandLoadBalancer&) = delete;
    CommandLoadBalancer& operator=(const CommandLoadBalancer&) = delete;

    common::Status execute(const backend::shared::crud::CrudCommandMessage& command);

private:
    struct Task {
        backend::shared::crud::CrudCommandMessage command {};
        std::promise<common::Status> promise {};
    };

    struct Worker {
        std::thread thread {};
        std::mutex mutex {};
        std::condition_variable cv {};
        std::deque<Task> queue {};
        bool stop {false};
    };

    void worker_loop(Worker& worker);

    CommandDispatchHandler handler_ {};
    CommandLoadBalancerOptions options_ {};
    std::atomic<std::size_t> next_worker_ {0U};
    std::vector<std::unique_ptr<Worker>> workers_ {};
};

} // namespace hugin::api

#endif // HUGIN_API_COMMAND_LOAD_BALANCER_HPP
