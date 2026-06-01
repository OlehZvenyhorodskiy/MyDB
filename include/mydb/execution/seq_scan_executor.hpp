#pragma once

#include <mydb/execution/executor.hpp>
#include <mydb/catalog/table_heap.hpp>

namespace mydb {

class SeqScanExecutor : public AbstractExecutor {
public:
    SeqScanExecutor(ExecutorContext* ctx, const std::string& table_name)
        : AbstractExecutor(ctx), table_name_(table_name) {}

    void Init() override {
        TableInfo* table_info = ctx_->GetCatalog()->GetTable(table_name_);
        if (table_info == nullptr) {
            throw std::runtime_error("Table not found: " + table_name_);
        }
        
        schema_ = &table_info->schema;
        
        table_heap_ = std::make_unique<TableHeap>(
            ctx_->GetBufferPoolManager(),
            schema_,
            table_info->first_page_id
        );
        
        iterator_ = table_heap_->Begin();
    }

    bool Next(Tuple* tuple, RID* rid) override {
        while (!iterator_.IsEnd()) {
            *tuple = *iterator_;
            *rid = iterator_.GetRID();
            ++iterator_;
            return true;
        }
        return false;
    }

    const Schema* GetOutputSchema() const override {
        return schema_;
    }

private:
    std::string table_name_;
    const Schema* schema_{nullptr};
    std::unique_ptr<TableHeap> table_heap_;
    TableIterator iterator_;
};

}
