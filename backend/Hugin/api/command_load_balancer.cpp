#include "command_load_balancer.hpp"

#include <algorithm>
#include <utility>

namespace hugin::api {

CommandLoadBalancer::CommandLoadBalancer(CommandDispatchHandler handler, CommandLoadBalancerOptions options)
    : handler_(std::move(handler)),
      options_(std::move(options)) {
    if (options_.worker_threads == 0U) {
        options_.worker_threads = 1U;
    }
    if (options_.max_queue_per_worker == 0U) {
        options_.max_queue_per_worker = 1U;
    }
    if (options_.execution_timeout.count() <= 0) {
        options_.execution_timeout = std::chrono::milliseconds(1'500);
    }

    workers_.reserve(options_.worker_threads);
    for (std::size_t i = 0; i < options_.worker_threads; ++i) {
        auto worker = std::make_unique<Worker>();
        worker->thread = std::thread([this, ptr = worker.get()]() {
            worker_loop(*ptr);
        });
        workers_.push_back(std::move(worker));
    }
}

CommandLoadBalancer::~CommandLoadBalancer() {
    for (auto& worker : workers_) {
        {
            std::lock_guard<std::mutex> lock(worker->mutex);
            worker->stop = true;
        }
        worker->cv.notify_all();
    }

    for (auto& worker : workers_) {
        if (worker->thread.joinable()) {
            worker->thread.join();
        }
    }
}

common::Status CommandLoadBalancer::execute(const backend::shared::crud::CrudCommandMessage& command) {
    if (!handler_) {
        return common::Status::failure(common::ErrorCode::Internal, "Command handler is not configured.");
    }
    if (workers_.empty()) {
        return common::Status::failure(common::ErrorCode::Internal, "No command workers are available.");
    }

    const std::size_t index = next_worker_.fetch_add(1U) % workers_.size();
    auto& worker = *workers_[index];

    Task task;
    task.command = command;
    auto future = task.promise.get_future();

    {
        std::lock_guard<std::mutex> lock(worker.mutex);
        if (worker.queue.size() >= options_.max_queue_per_worker) {
            return common::Status::failure(
                common::ErrorCode::Internal,
                "Command queue is full on selected worker."
            );
        }
        worker.queue.push_back(std::move(task));
    }
    worker.cv.notify_one();

    if (future.wait_for(options_.execution_timeout) != std::future_status::ready) {
        return common::Status::failure(common::ErrorCode::Internal, "Command dispatch timed out.");
    }
    return future.get();
}

void CommandLoadBalancer::worker_loop(Worker& worker) {
    while (true) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(worker.mutex);
            worker.cv.wait(lock, [&worker]() {
                return worker.stop || !worker.queue.empty();
            });

            if (worker.stop && worker.queue.empty()) {
                return;
            }

            task = std::move(worker.queue.front());
            worker.queue.pop_front();
        }

        auto status = handler_(task.command);
        task.promise.set_value(std::move(status));
    }
}

} // namespace hugin::api
