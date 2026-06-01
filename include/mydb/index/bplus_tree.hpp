#pragma once

#include <mydb/storage/buffer_pool.hpp>
#include <mydb/catalog/tuple.hpp>

#include <vector>
#include <optional>
#include <functional>

namespace mydb {

enum class BPlusTreeNodeType : uint8_t {
    INVALID = 0,
    INTERNAL,
    LEAF
};

struct BPlusTreePageHeader {
    BPlusTreeNodeType node_type{BPlusTreeNodeType::INVALID};
    uint32_t size{0};
    uint32_t max_size{0};
    page_id_t parent_page_id{INVALID_PAGE_ID};
    lsn_t lsn{0};
};

struct GenericKey {
    int64_t value{0};
    
    GenericKey() = default;
    explicit GenericKey(int64_t v) : value(v) {}
    explicit GenericKey(int32_t v) : value(v) {}
    
    bool operator<(const GenericKey& other) const { return value < other.value; }
    bool operator==(const GenericKey& other) const { return value == other.value; }
    bool operator<=(const GenericKey& other) const { return value <= other.value; }
    bool operator>(const GenericKey& other) const { return value > other.value; }
    bool operator>=(const GenericKey& other) const { return value >= other.value; }
};

class BPlusTreeInternalPage {
public:
    void Init(uint32_t max_size, page_id_t parent = INVALID_PAGE_ID) {
        header_.node_type = BPlusTreeNodeType::INTERNAL;
        header_.size = 0;
        header_.max_size = max_size;
        header_.parent_page_id = parent;
    }

    BPlusTreeNodeType GetNodeType() const { return header_.node_type; }
    bool IsLeaf() const { return false; }
    uint32_t GetSize() const { return header_.size; }
    uint32_t GetMaxSize() const { return header_.max_size; }
    uint32_t GetMinSize() const { return header_.max_size / 2; }
    page_id_t GetParentPageId() const { return header_.parent_page_id; }
    void SetParentPageId(page_id_t parent) { header_.parent_page_id = parent; }

    GenericKey KeyAt(int index) const {
        return keys_[index];
    }

    void SetKeyAt(int index, const GenericKey& key) {
        keys_[index] = key;
    }

    page_id_t ValueAt(int index) const {
        return children_[index];
    }

    void SetValueAt(int index, page_id_t value) {
        children_[index] = value;
    }

    page_id_t Lookup(const GenericKey& key) const {
        int left = 1; 
        int right = header_.size;
        
        while (left < right) {
            int mid = (left + right) / 2;
            if (keys_[mid] <= key) {
                left = mid + 1;
            } else {
                right = mid;
            }
        }
        
        return children_[left - 1];
    }

    void Insert(const GenericKey& key, page_id_t value) {
        int pos = header_.size;
        while (pos > 1 && keys_[pos - 1] > key) {
            keys_[pos] = keys_[pos - 1];
            children_[pos] = children_[pos - 1];
            pos--;
        }
        
        keys_[pos] = key;
        children_[pos] = value;
        header_.size++;
    }

    bool IsFull() const {
        return header_.size >= header_.max_size;
    }

    bool IsUnderflow() const {
        return header_.size < GetMinSize();
    }

private:
    static constexpr size_t MAX_CHILDREN = 
        (PAGE_SIZE - sizeof(BPlusTreePageHeader)) / (sizeof(GenericKey) + sizeof(page_id_t));
    
    BPlusTreePageHeader header_;
    GenericKey keys_[MAX_CHILDREN];
    page_id_t children_[MAX_CHILDREN];
};

class BPlusTreeLeafPage {
public:
    void Init(uint32_t max_size, page_id_t parent = INVALID_PAGE_ID) {
        header_.node_type = BPlusTreeNodeType::LEAF;
        header_.size = 0;
        header_.max_size = max_size;
        header_.parent_page_id = parent;
        next_page_id_ = INVALID_PAGE_ID;
    }

    BPlusTreeNodeType GetNodeType() const { return header_.node_type; }
    bool IsLeaf() const { return true; }
    uint32_t GetSize() const { return header_.size; }
    uint32_t GetMaxSize() const { return header_.max_size; }
    uint32_t GetMinSize() const { return (header_.max_size + 1) / 2; }
    page_id_t GetParentPageId() const { return header_.parent_page_id; }
    void SetParentPageId(page_id_t parent) { header_.parent_page_id = parent; }

    page_id_t GetNextPageId() const { return next_page_id_; }
    void SetNextPageId(page_id_t next) { next_page_id_ = next; }

    GenericKey KeyAt(int index) const {
        return keys_[index];
    }

    RID ValueAt(int index) const {
        return values_[index];
    }

    std::optional<RID> Lookup(const GenericKey& key) const {
        for (uint32_t i = 0; i < header_.size; ++i) {
            if (keys_[i] == key) {
                return values_[i];
            }
        }
        return std::nullopt;
    }

    bool Insert(const GenericKey& key, const RID& rid) {
        if (header_.size >= header_.max_size) {
            return false;
        }
        
        uint32_t pos = header_.size;
        while (pos > 0 && keys_[pos - 1] > key) {
            keys_[pos] = keys_[pos - 1];
            values_[pos] = values_[pos - 1];
            pos--;
        }
        
        if (pos > 0 && keys_[pos - 1] == key) {
            return false;
        }
        
        keys_[pos] = key;
        values_[pos] = rid;
        header_.size++;
        return true;
    }

    bool Delete(const GenericKey& key) {
        for (uint32_t i = 0; i < header_.size; ++i) {
            if (keys_[i] == key) {
                for (uint32_t j = i; j < header_.size - 1; ++j) {
                    keys_[j] = keys_[j + 1];
                    values_[j] = values_[j + 1];
                }
                header_.size--;
                return true;
            }
        }
        return false;
    }

    bool IsFull() const {
        return header_.size >= header_.max_size;
    }

    bool IsUnderflow() const {
        return header_.size < GetMinSize();
    }

private:
    static constexpr size_t MAX_ENTRIES = 
        (PAGE_SIZE - sizeof(BPlusTreePageHeader) - sizeof(page_id_t)) / 
        (sizeof(GenericKey) + sizeof(RID));
    
    BPlusTreePageHeader header_;
    GenericKey keys_[MAX_ENTRIES];
    RID values_[MAX_ENTRIES];
    page_id_t next_page_id_{INVALID_PAGE_ID};
};

class BPlusTree {
public:
    BPlusTree(BufferPoolManager* bpm, 
              page_id_t root_page_id = INVALID_PAGE_ID,
              int leaf_max_size = 10,
              int internal_max_size = 10)
        : bpm_(bpm), 
          root_page_id_(root_page_id),
          leaf_max_size_(leaf_max_size),
          internal_max_size_(internal_max_size) {}

    bool IsEmpty() const { return root_page_id_ == INVALID_PAGE_ID; }

    page_id_t GetRootPageId() const { return root_page_id_; }

    bool GetValue(const GenericKey& key, std::vector<RID>* result) {
        if (IsEmpty()) {
            return false;
        }
        
        Page* page = FindLeafPage(key);
        if (page == nullptr) {
            return false;
        }
        
        auto* leaf = reinterpret_cast<BPlusTreeLeafPage*>(page->GetData());
        auto rid = leaf->Lookup(key);
        
        bpm_->UnpinPage(page->GetPageId(), false);
        
        if (rid.has_value()) {
            result->push_back(rid.value());
            return true;
        }
        return false;
    }

    bool Insert(const GenericKey& key, const RID& rid) {
        if (IsEmpty()) {
            page_id_t new_page_id;
            Page* page = bpm_->NewPage(&new_page_id);
            if (page == nullptr) {
                return false;
            }
            
            auto* leaf = reinterpret_cast<BPlusTreeLeafPage*>(page->GetData());
            leaf->Init(leaf_max_size_);
            leaf->Insert(key, rid);
            
            root_page_id_ = new_page_id;
            bpm_->UnpinPage(new_page_id, true);
            return true;
        }
        
        Page* page = FindLeafPage(key);
        if (page == nullptr) {
            return false;
        }
        
        auto* leaf = reinterpret_cast<BPlusTreeLeafPage*>(page->GetData());
        
        if (!leaf->IsFull()) {
            bool success = leaf->Insert(key, rid);
            bpm_->UnpinPage(page->GetPageId(), success);
            return success;
        }
        
        bpm_->UnpinPage(page->GetPageId(), false);
        
        return false;
    }

    bool Remove(const GenericKey& key) {
        if (IsEmpty()) {
            return false;
        }
        
        Page* page = FindLeafPage(key);
        if (page == nullptr) {
            return false;
        }
        
        auto* leaf = reinterpret_cast<BPlusTreeLeafPage*>(page->GetData());
        bool deleted = leaf->Delete(key);
        
        bpm_->UnpinPage(page->GetPageId(), deleted);
        
        return deleted;
    }

    class Iterator {
    public:
        Iterator() = default;
        
        Iterator(BufferPoolManager* bpm, page_id_t page_id, int index)
            : bpm_(bpm), page_id_(page_id), index_(index) {
            if (page_id_ != INVALID_PAGE_ID) {
                page_ = bpm_->FetchPage(page_id_);
            }
        }
        
        ~Iterator() {
            if (page_ != nullptr) {
                bpm_->UnpinPage(page_id_, false);
            }
        }
        
        bool IsEnd() const { return page_id_ == INVALID_PAGE_ID; }
        
        GenericKey GetKey() const {
            auto* leaf = reinterpret_cast<BPlusTreeLeafPage*>(page_->GetData());
            return leaf->KeyAt(index_);
        }
        
        RID GetValue() const {
            auto* leaf = reinterpret_cast<BPlusTreeLeafPage*>(page_->GetData());
            return leaf->ValueAt(index_);
        }
        
        Iterator& operator++() {
            if (page_ == nullptr) return *this;
            
            auto* leaf = reinterpret_cast<BPlusTreeLeafPage*>(page_->GetData());
            index_++;
            
            if (static_cast<uint32_t>(index_) >= leaf->GetSize()) {
                page_id_t next = leaf->GetNextPageId();
                bpm_->UnpinPage(page_id_, false);
                
                if (next == INVALID_PAGE_ID) {
                    page_ = nullptr;
                    page_id_ = INVALID_PAGE_ID;
                } else {
                    page_id_ = next;
                    page_ = bpm_->FetchPage(next);
                    index_ = 0;
                }
            }
            
            return *this;
        }
        
    private:
        BufferPoolManager* bpm_{nullptr};
        Page* page_{nullptr};
        page_id_t page_id_{INVALID_PAGE_ID};
        int index_{0};
    };

    Iterator Begin() {
        if (IsEmpty()) {
            return Iterator();
        }
        
        Page* page = bpm_->FetchPage(root_page_id_);
        while (page != nullptr) {
            auto* node = reinterpret_cast<BPlusTreeInternalPage*>(page->GetData());
            if (node->IsLeaf()) {
                page_id_t pid = page->GetPageId();
                bpm_->UnpinPage(pid, false);
                return Iterator(bpm_, pid, 0);
            }
            
            page_id_t child = node->ValueAt(0);
            bpm_->UnpinPage(page->GetPageId(), false);
            page = bpm_->FetchPage(child);
        }
        
        return Iterator();
    }

    Iterator Begin(const GenericKey& key) {
        Page* page = FindLeafPage(key);
        if (page == nullptr) {
            return Iterator();
        }
        
        auto* leaf = reinterpret_cast<BPlusTreeLeafPage*>(page->GetData());
        
        int index = 0;
        for (uint32_t i = 0; i < leaf->GetSize(); ++i) {
            if (leaf->KeyAt(i) >= key) {
                index = i;
                break;
            }
        }
        
        page_id_t pid = page->GetPageId();
        bpm_->UnpinPage(pid, false);
        return Iterator(bpm_, pid, index);
    }

    Iterator End() {
        return Iterator();
    }

private:
    Page* FindLeafPage(const GenericKey& key) {
        if (IsEmpty()) {
            return nullptr;
        }
        
        Page* page = bpm_->FetchPage(root_page_id_);
        
        while (page != nullptr) {
            auto* header = reinterpret_cast<BPlusTreePageHeader*>(page->GetData());
            
            if (header->node_type == BPlusTreeNodeType::LEAF) {
                return page;
            }
            
            auto* internal = reinterpret_cast<BPlusTreeInternalPage*>(page->GetData());
            page_id_t child = internal->Lookup(key);
            
            bpm_->UnpinPage(page->GetPageId(), false);
            page = bpm_->FetchPage(child);
        }
        
        return nullptr;
    }

    BufferPoolManager* bpm_;
    page_id_t root_page_id_;
    int leaf_max_size_;
    int internal_max_size_;
};

}
