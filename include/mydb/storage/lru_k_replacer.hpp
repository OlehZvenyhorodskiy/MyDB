#pragma once

#include <mydb/storage/page.hpp>

#include <list>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <limits>
#include <optional>
#include <chrono>

namespace mydb {

class LRUKReplacer {
public:
    using timestamp_t = uint64_t;

    explicit LRUKReplacer(size_t num_frames, size_t k = 2)
        : num_frames_(num_frames), k_(k), current_timestamp_(0) {}

    void RecordAccess(frame_id_t frame_id) {
        std::lock_guard<std::mutex> lock(latch_);
        
        current_timestamp_++;
        
        auto& history = access_history_[frame_id];
        history.push_back(current_timestamp_);
        
        while (history.size() > k_) {
            history.pop_front();
        }
    }

    void SetEvictable(frame_id_t frame_id, bool evictable) {
        std::lock_guard<std::mutex> lock(latch_);
        
        if (evictable) {
            evictable_frames_.insert(frame_id);
        } else {
            evictable_frames_.erase(frame_id);
        }
    }

    bool Evict(frame_id_t* frame_id) {
        std::lock_guard<std::mutex> lock(latch_);
        
        if (evictable_frames_.empty()) {
            return false;
        }
        
        frame_id_t victim = INVALID_FRAME_ID;
        timestamp_t max_k_distance = 0;
        timestamp_t earliest_first_access = std::numeric_limits<timestamp_t>::max();
        bool found_inf = false;
        
        for (frame_id_t fid : evictable_frames_) {
            auto it = access_history_.find(fid);
            
            if (it == access_history_.end() || it->second.empty()) {
                if (!found_inf || fid < victim) {
                    victim = fid;
                    found_inf = true;
                }
                continue;
            }
            
            const auto& history = it->second;
            
            if (history.size() < k_) {
                timestamp_t first_access = history.front();
                if (!found_inf || first_access < earliest_first_access) {
                    victim = fid;
                    earliest_first_access = first_access;
                    found_inf = true;
                }
            } else if (!found_inf) {
                timestamp_t kth_access = history.front();
                timestamp_t k_distance = current_timestamp_ - kth_access;
                
                if (k_distance > max_k_distance) {
                    max_k_distance = k_distance;
                    victim = fid;
                }
            }
        }
        
        if (victim == INVALID_FRAME_ID) {
            return false;
        }
        
        evictable_frames_.erase(victim);
        access_history_.erase(victim);
        
        *frame_id = victim;
        return true;
    }

    void Remove(frame_id_t frame_id) {
        std::lock_guard<std::mutex> lock(latch_);
        
        evictable_frames_.erase(frame_id);
        access_history_.erase(frame_id);
    }

    size_t Size() const {
        std::lock_guard<std::mutex> lock(latch_);
        return evictable_frames_.size();
    }

    size_t GetK() const { return k_; }

private:
    size_t num_frames_;
    size_t k_;
    timestamp_t current_timestamp_;
    
    std::unordered_map<frame_id_t, std::list<timestamp_t>> access_history_;
    
    std::unordered_set<frame_id_t> evictable_frames_;
    
    mutable std::mutex latch_;
};

}