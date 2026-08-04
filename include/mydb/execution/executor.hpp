#pragma once

#include <mydb/catalog/tuple.hpp>
#include <mydb/catalog/schema.hpp>
#include <mydb/execution/executor_context.hpp>

#include <memory>

namespace mydb {

class AbstractExecutor {
public:
    explicit AbstractExecutor(ExecutorContext* ctx) : ctx_(ctx) {}
    virtual ~AbstractExecutor() = default;

    virtual void Init() = 0;
    virtual bool Next(Tuple* tuple, RID* rid) = 0;
    virtual const Schema* GetOutputSchema() const = 0;

protected:
    ExecutorContext* ctx_;
};

using ExecutorPtr = std::unique_ptr<AbstractExecutor>;

}