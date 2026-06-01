#pragma once

#include <mydb/execution/executor.hpp>
#include <mydb/execution/expression.hpp>

namespace mydb {

class FilterExecutor : public AbstractExecutor {
public:
    FilterExecutor(ExecutorContext* ctx, 
                   ExecutorPtr child,
                   AbstractExpression* predicate)
        : AbstractExecutor(ctx), 
          child_(std::move(child)),
          predicate_(predicate) {}

    void Init() override {
        child_->Init();
    }

    bool Next(Tuple* tuple, RID* rid) override {
        while (child_->Next(tuple, rid)) {
            if (predicate_ == nullptr) {
                return true;
            }
            
            if (predicate_->EvaluateAsBool(tuple, GetOutputSchema())) {
                return true;
            }
        }
        return false;
    }

    const Schema* GetOutputSchema() const override {
        return child_->GetOutputSchema();
    }

private:
    ExecutorPtr child_;
    AbstractExpression* predicate_;
};

}
