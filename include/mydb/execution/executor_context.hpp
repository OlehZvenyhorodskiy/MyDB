#pragma once

#include <mydb/catalog/catalog.hpp>
#include <mydb/storage/buffer_pool.hpp>

namespace mydb {

class ExecutorContext {
public:
    ExecutorContext(BufferPoolManager* bpm, Catalog* catalog)
        : bpm_(bpm), catalog_(catalog) {}

    BufferPoolManager* GetBufferPoolManager() { return bpm_; }
    Catalog* GetCatalog() { return catalog_; }

private:
    BufferPoolManager* bpm_;
    Catalog* catalog_;
};

}
