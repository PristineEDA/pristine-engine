#pragma once

#include <atomic>
#include <memory>
#include <stdexcept>

namespace pristine {

class OperationCancelled final : public std::runtime_error {
public:
    OperationCancelled() : std::runtime_error("Request cancelled") {}
};

class CancellationToken {
public:
    CancellationToken() = default;

    [[nodiscard]] bool cancellationRequested() const {
        return state_ && state_->load(std::memory_order_acquire);
    }

    void throwIfCancellationRequested() const {
        if (cancellationRequested()) {
            throw OperationCancelled{};
        }
    }

private:
    explicit CancellationToken(std::shared_ptr<std::atomic_bool> state) : state_(std::move(state)) {}

    std::shared_ptr<std::atomic_bool> state_;
    friend class CancellationSource;
};

class CancellationSource {
public:
    CancellationSource() : state_(std::make_shared<std::atomic_bool>(false)) {}

    [[nodiscard]] CancellationToken token() const { return CancellationToken(state_); }
    void cancel() const { state_->store(true, std::memory_order_release); }

private:
    std::shared_ptr<std::atomic_bool> state_;
};

} // namespace pristine
