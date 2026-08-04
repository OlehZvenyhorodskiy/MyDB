#pragma once

#include <mydb/catalog/tuple.hpp>
#include <mydb/catalog/schema.hpp>
#include <mydb/storage/buffer_pool.hpp>

#include <memory>

namespace mydb {

class TableIterator {
public:
    TableIterator() = default;
    
    TableIterator(BufferPoolManager* bpm, const Schema* schema,
                  page_id_t page_id, uint32_t slot_num)
        : bpm_(bpm), schema_(schema), rid_(page_id, slot_num) {
        if (rid_.page_id != INVALID_PAGE_ID) {
            LoadCurrentTuple();
        }
    }

    const Tuple& operator*() const { return current_tuple_; }
    const Tuple* operator->() const { return &current_tuple_; }

    const RID& GetRID() const { return rid_; }

    TableIterator& operator++() {
        if (rid_.page_id == INVALID_PAGE_ID) {
            return *this;
        }
        
        rid_.slot_num++;
        
        Page* page = bpm_->FetchPage(rid_.page_id);
        if (page == nullptr) {
            rid_.page_id = INVALID_PAGE_ID;
            return *this;
        }
        
        if (rid_.slot_num >= page->GetTupleCount()) {
            bpm_->UnpinPage(rid_.page_id, false);
            
            page_id_t next_page = rid_.page_id + 1;
            Page* next = bpm_->FetchPage(next_page);
            if (next == nullptr || next->GetTupleCount() == 0) {
                if (next != nullptr) {
                    bpm_->UnpinPage(next_page, false);
                }
                rid_.page_id = INVALID_PAGE_ID;
                return *this;
            }
            
            rid_.page_id = next_page;
            rid_.slot_num = 0;
            bpm_->UnpinPage(next_page, false);
        } else {
            bpm_->UnpinPage(rid_.page_id, false);
        }
        
        LoadCurrentTuple();
        return *this;
    }

    bool IsEnd() const {
        return rid_.page_id == INVALID_PAGE_ID;
    }

    bool operator==(const TableIterator& other) const {
        return rid_ == other.rid_;
    }

    bool operator!=(const TableIterator& other) const {
        return !(*this == other);
    }

private:
    void LoadCurrentTuple() {
        if (rid_.page_id == INVALID_PAGE_ID) return;
        
        Page* page = bpm_->FetchPage(rid_.page_id);
        if (page == nullptr) {
            rid_.page_id = INVALID_PAGE_ID;
            return;
        }
        
        size_t size;
        const char* data = page->GetTuple(rid_.slot_num, &size);
        if (data == nullptr) {
            bpm_->UnpinPage(rid_.page_id, false);
            rid_.page_id = INVALID_PAGE_ID;
            return;
        }
        
        current_tuple_ = Tuple::DeserializeFrom(data, schema_);
        current_tuple_.SetRID(rid_);
        bpm_->UnpinPage(rid_.page_id, false);
    }

    BufferPoolManager* bpm_{nullptr};
    const Schema* schema_{nullptr};
    RID rid_;
    Tuple current_tuple_;
};

class TableHeap {
public:
    TableHeap(BufferPoolManager* bpm, const Schema* schema, page_id_t first_page_id)
        : bpm_(bpm), schema_(schema), first_page_id_(first_page_id) {}

    bool InsertTuple(const Tuple& tuple, RID* rid) {
        std::vector<char> buffer(tuple.GetSerializedSize(schema_) + sizeof(TupleHeader));
        tuple.SerializeTo(buffer.data(), schema_);
        
        page_id_t current_page_id = first_page_id_;
        
        while (current_page_id != INVALID_PAGE_ID) {
            Page* page = bpm_->FetchPage(current_page_id);
            if (page == nullptr) break;
            
            int slot = page->InsertTuple(buffer.data(), buffer.size());
            if (slot >= 0) {
                *rid = RID(current_page_id, static_cast<uint32_t>(slot));
                bpm_->UnpinPage(current_page_id, true);
                return true;
            }
            
            bpm_->UnpinPage(current_page_id, false);
            current_page_id++;  
        }
        
        page_id_t new_page_id;
        Page* new_page = bpm_->NewPage(&new_page_id);
        if (new_page == nullptr) {
            return false;
        }
        
        int slot = new_page->InsertTuple(buffer.data(), buffer.size());
        if (slot < 0) {
            bpm_->UnpinPage(new_page_id, false);
            return false;
        }
        
        *rid = RID(new_page_id, static_cast<uint32_t>(slot));
        bpm_->UnpinPage(new_page_id, true);
        return true;
    }

    bool DeleteTuple(const RID& rid) {
        Page* page = bpm_->FetchPage(rid.page_id);
        if (page == nullptr) {
            return false;
        }
        
        bool success = page->DeleteTuple(rid.slot_num);
        bpm_->UnpinPage(rid.page_id, success);
        return success;
    }

    bool GetTuple(const RID& rid, Tuple* tuple) {
        Page* page = bpm_->FetchPage(rid.page_id);
        if (page == nullptr) {
            return false;
        }
        
        size_t size;
        const char* data = page->GetTuple(rid.slot_num, &size);
        if (data == nullptr) {
            bpm_->UnpinPage(rid.page_id, false);
            return false;
        }
        
        *tuple = Tuple::DeserializeFrom(data, schema_);
        tuple->SetRID(rid);
        bpm_->UnpinPage(rid.page_id, false);
        return true;
    }

    TableIterator Begin() {
        return TableIterator(bpm_, schema_, first_page_id_, 0);
    }

    TableIterator End() {
        return TableIterator(bpm_, schema_, INVALID_PAGE_ID, 0);
    }

    const Schema* GetSchema() const { return schema_; }

    page_id_t GetFirstPageId() const { return first_page_id_; }

private:
    BufferPoolManager* bpm_;
    const Schema* schema_;
    page_id_t first_page_id_;
};

}