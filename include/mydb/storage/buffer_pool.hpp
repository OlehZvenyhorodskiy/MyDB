#pragma once

#include <mydb/storage/page.hpp>
#include <mydb/storage/disk_manager.hpp>
#include <mydb/storage/lru_k_replacer.hpp>

#include <memory>
#include <unordered_map>
#include <list>
#include <mutex>
#include <condition_variable>

namespace mydb {

struct BufferPoolConfig {
    size_t pool_size{1024};
    size_t replacer_k{2};
    std::string db_file;
};

class BufferPoolManager {
public:
    explicit BufferPoolManager(const BufferPoolConfig& config)
        : pool_size_(config.pool_size),
          disk_manager_(std::make_unique<DiskManager>(config.db_file)),
          replacer_(std::make_unique<LRUKReplacer>(config.pool_size, config.replacer_k)) {
        
        pages_.resize(pool_size_);
        
        for (frame_id_t i = 0; i < static_cast<frame_id_t>(pool_size_); ++i) {
            free_list_.push_back(i);
        }
    }

    BufferPoolManager(size_t pool_size, std::unique_ptr<DiskManager> disk_manager)
        : pool_size_(pool_size),
          disk_manager_(std::move(disk_manager)),
          replacer_(std::make_unique<LRUKReplacer>(pool_size, 2)) {
        
        pages_.resize(pool_size_);
        for (frame_id_t i = 0; i < static_cast<frame_id_t>(pool_size_); ++i) {
            free_list_.push_back(i);
        }
    }

    ~BufferPoolManager() {
        FlushAllPages();
    }

    BufferPoolManager(const BufferPoolManager&) = delete;
    BufferPoolManager& operator=(const BufferPoolManager&) = delete;

    Page* FetchPage(page_id_t page_id) {
        std::lock_guard<std::mutex> lock(latch_);
        
        if (page_id == INVALID_PAGE_ID) {
            return nullptr;
        }
        
        auto it = page_table_.find(page_id);
        if (it != page_table_.end()) {
            frame_id_t frame_id = it->second;
            Page& page = pages_[frame_id];
            page.IncrementPinCount();
            replacer_->SetEvictable(frame_id, false);
            replacer_->RecordAccess(frame_id);
            return &page;
        }
        
        frame_id_t frame_id = INVALID_FRAME_ID;
        
        if (!free_list_.empty()) {
            frame_id = free_list_.front();
            free_list_.pop_front();
        } else {
            if (!replacer_->Evict(&frame_id)) {
                return nullptr;
            }
            
            Page& old_page = pages_[frame_id];
            if (old_page.IsDirty()) {
                disk_manager_->WritePage(old_page.GetPageId(), old_page.GetData());
            }
            
            page_table_.erase(old_page.GetPageId());
        }
        
        Page& page = pages_[frame_id];
        page.ResetMemory();
        page.SetPageId(page_id);
        disk_manager_->ReadPage(page_id, page.GetData());
        
        page_table_[page_id] = frame_id;
        page.IncrementPinCount();
        replacer_->RecordAccess(frame_id);
        replacer_->SetEvictable(frame_id, false);
        
        return &page;
    }

    bool UnpinPage(page_id_t page_id, bool is_dirty) {
        std::lock_guard<std::mutex> lock(latch_);
        
        auto it = page_table_.find(page_id);
        if (it == page_table_.end()) {
            return false;
        }
        
        frame_id_t frame_id = it->second;
        Page& page = pages_[frame_id];
        
        if (page.GetPinCount() <= 0) {
            return false;
        }
        
        page.DecrementPinCount();
        if (is_dirty) {
            page.SetDirty(true);
        }
        
        if (page.GetPinCount() == 0) {
            replacer_->SetEvictable(frame_id, true);
        }
        
        return true;
    }

    Page* NewPage(page_id_t* page_id) {
        std::lock_guard<std::mutex> lock(latch_);
        
        frame_id_t frame_id = INVALID_FRAME_ID;
        
        if (!free_list_.empty()) {
            frame_id = free_list_.front();
            free_list_.pop_front();
        } else {
            if (!replacer_->Evict(&frame_id)) {
                return nullptr;
            }
            
            Page& old_page = pages_[frame_id];
            if (old_page.IsDirty()) {
                disk_manager_->WritePage(old_page.GetPageId(), old_page.GetData());
            }
            page_table_.erase(old_page.GetPageId());
        }
        
        page_id_t new_page_id = disk_manager_->AllocatePage();
        
        Page& page = pages_[frame_id];
        page.ResetMemory();
        page.SetPageId(new_page_id);
        page.IncrementPinCount();
        page.SetDirty(true);
        
        page_table_[new_page_id] = frame_id;
        replacer_->RecordAccess(frame_id);
        replacer_->SetEvictable(frame_id, false);
        
        *page_id = new_page_id;
        return &page;
    }

    bool FlushPage(page_id_t page_id) {
        std::lock_guard<std::mutex> lock(latch_);
        
        auto it = page_table_.find(page_id);
        if (it == page_table_.end()) {
            return false;
        }
        
        frame_id_t frame_id = it->second;
        Page& page = pages_[frame_id];
        
        disk_manager_->WritePage(page_id, page.GetData());
        page.SetDirty(false);
        
        return true;
    }

    void FlushAllPages() {
        std::lock_guard<std::mutex> lock(latch_);
        
        for (auto& [page_id, frame_id] : page_table_) {
            Page& page = pages_[frame_id];
            if (page.IsDirty()) {
                disk_manager_->WritePage(page_id, page.GetData());
                page.SetDirty(false);
            }
        }
    }

    bool DeletePage(page_id_t page_id) {
        std::lock_guard<std::mutex> lock(latch_);
        
        auto it = page_table_.find(page_id);
        if (it == page_table_.end()) {
            disk_manager_->DeallocatePage(page_id);
            return true;
        }
        
        frame_id_t frame_id = it->second;
        Page& page = pages_[frame_id];
        
        if (page.GetPinCount() > 0) {
            return false;
        }
        
        page_table_.erase(it);
        replacer_->Remove(frame_id);
        
        page.ResetMemory();
        free_list_.push_back(frame_id);
        
        disk_manager_->DeallocatePage(page_id);
        return true;
    }

    size_t GetPoolSize() const { return pool_size_; }

    struct FrameInfo {
        page_id_t page_id{INVALID_PAGE_ID};
        int pin_count{0};
        bool is_dirty{false};
    };

    std::vector<FrameInfo> GetState() {
        std::lock_guard<std::mutex> lock(latch_);
        std::vector<FrameInfo> state(pool_size_);
        
        for (const auto& [pid, fid] : page_table_) {
            if (fid < pool_size_) {
                state[fid].page_id = pid;
                state[fid].pin_count = pages_[fid].GetPinCount();
                state[fid].is_dirty = pages_[fid].IsDirty();
            }
        }
        return state;
    }

    DiskManager* GetDiskManager() { return disk_manager_.get(); }

private:
    size_t pool_size_;
    std::vector<Page> pages_;
    std::unordered_map<page_id_t, frame_id_t> page_table_;
    std::list<frame_id_t> free_list_;
    std::unique_ptr<DiskManager> disk_manager_;
    std::unique_ptr<LRUKReplacer> replacer_;
    std::mutex latch_;
};

}