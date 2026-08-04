#pragma once

#include <cstdint>
#include <cstring>
#include <atomic>

namespace mydb {

using page_id_t = int32_t;
using frame_id_t = int32_t;
using lsn_t = int64_t;
using txn_id_t = int64_t;
using slot_offset_t = uint16_t;

static constexpr page_id_t INVALID_PAGE_ID = -1;
static constexpr frame_id_t INVALID_FRAME_ID = -1;
static constexpr size_t PAGE_SIZE = 4096;

struct Slot {
    slot_offset_t offset{0};
    uint16_t length{0};
    bool is_valid{false};
};

struct PageHeader {
    page_id_t page_id{INVALID_PAGE_ID};
    lsn_t lsn{0};
    uint16_t tuple_count{0};
    slot_offset_t free_space_pointer{0};
    slot_offset_t slot_array_end{0};
    uint32_t checksum{0};
};

class Page {
public:
    Page() {
        ResetMemory();
    }

    char* GetData() { return data_; }
    const char* GetData() const { return data_; }

    page_id_t GetPageId() const { return GetHeader()->page_id; }
    void SetPageId(page_id_t page_id) { GetHeader()->page_id = page_id; }

    lsn_t GetLSN() const { return GetHeader()->lsn; }
    void SetLSN(lsn_t lsn) { GetHeader()->lsn = lsn; }

    int GetPinCount() const { return pin_count_; }
    void IncrementPinCount() { ++pin_count_; }
    void DecrementPinCount() { 
        if (pin_count_ > 0) --pin_count_; 
    }

    bool IsDirty() const { return is_dirty_; }
    void SetDirty(bool dirty) { is_dirty_ = dirty; }

    uint16_t GetTupleCount() const { return GetHeader()->tuple_count; }

    size_t GetFreeSpace() const {
        const auto* header = GetHeader();
        size_t header_size = sizeof(PageHeader);
        size_t slots_size = header->tuple_count * sizeof(Slot);
        return PAGE_SIZE - header_size - slots_size - 
               (PAGE_SIZE - header->free_space_pointer);
    }

    int InsertTuple(const char* data, size_t size) {
        if (size + sizeof(Slot) > GetFreeSpace()) {
            return -1;
        }

        auto* header = GetHeader();
        
        slot_offset_t tuple_offset = header->free_space_pointer - static_cast<slot_offset_t>(size);
        
        std::memcpy(data_ + tuple_offset, data, size);
        
        Slot* slots = GetSlotArray();
        int slot_idx = header->tuple_count;
        slots[slot_idx].offset = tuple_offset;
        slots[slot_idx].length = static_cast<uint16_t>(size);
        slots[slot_idx].is_valid = true;
        
        header->free_space_pointer = tuple_offset;
        header->tuple_count++;
        
        is_dirty_ = true;
        return slot_idx;
    }

    const char* GetTuple(int slot_idx, size_t* size) const {
        const auto* header = GetHeader();
        if (slot_idx < 0 || slot_idx >= header->tuple_count) {
            return nullptr;
        }
        
        const Slot* slots = GetSlotArray();
        if (!slots[slot_idx].is_valid) {
            return nullptr;
        }
        
        *size = slots[slot_idx].length;
        return data_ + slots[slot_idx].offset;
    }

    bool DeleteTuple(int slot_idx) {
        auto* header = GetHeader();
        if (slot_idx < 0 || slot_idx >= header->tuple_count) {
            return false;
        }
        
        Slot* slots = GetSlotArray();
        if (!slots[slot_idx].is_valid) {
            return false;
        }
        
        slots[slot_idx].is_valid = false;
        is_dirty_ = true;
        return true;
    }

    void ResetMemory() {
        std::memset(data_, 0, PAGE_SIZE);
        auto* header = GetHeader();
        header->page_id = INVALID_PAGE_ID;
        header->lsn = 0;
        header->tuple_count = 0;
        header->free_space_pointer = PAGE_SIZE;
        header->slot_array_end = sizeof(PageHeader);
        pin_count_ = 0;
        is_dirty_ = false;
    }

private:
    PageHeader* GetHeader() {
        return reinterpret_cast<PageHeader*>(data_);
    }
    const PageHeader* GetHeader() const {
        return reinterpret_cast<const PageHeader*>(data_);
    }

    Slot* GetSlotArray() {
        return reinterpret_cast<Slot*>(data_ + sizeof(PageHeader));
    }
    const Slot* GetSlotArray() const {
        return reinterpret_cast<const Slot*>(data_ + sizeof(PageHeader));
    }

    char data_[PAGE_SIZE]{};
    
    int pin_count_{0};
    bool is_dirty_{false};
};

}