#pragma once

#include <functional>
#include <utility>

#include "Result.h"
#include "tasks/Task.h"

// Runs a function as a task, so it can be a step of a SequentialTask that depends on the results of earlier steps
class FunctionTask : public Task {
   public:
    using Ptr = shared_qobject_ptr<FunctionTask>;

    explicit FunctionTask(std::function<Result<>()> function) : m_function(std::move(function)) {}

   protected:
    void executeTask() override
    {
        if (auto result = m_function(); result) {
            emitSucceeded();
        } else {
            emitFailed(result.error());
        }
    }

   private:
    std::function<Result<>()> m_function;
};
