#pragma once

#include <mydb/storage/page.hpp>

#include <fstream>
#include <string>
#include <mutex>
#include <atomic>
#include <vector>
#include <filesystem>

namespace mydb {

class DiskManager {
public:
    explicit DiskManager(const std::string& db_file) 
        : db_file_path_(db_file) {
        
        auto parent = std::filesystem::path(db_file).parent_path();
        if (!parent.empty() && !std::filesystem::exists(parent)) {
            std::filesystem::create_directories(parent);
        }
        
        db_io_.open(db_file, 
                    std::ios::binary | std::ios::in | std::ios::out);
        
        if (!db_io_.is_open()) {
            db_io_.clear();
            db_io_.open(db_file, 
                        std::ios::binary | std::ios::trunc | std::ios::out);
            db_io_.close();
            db_io_.open(db_file, 
                        std::ios::binary | std::ios::in | std::ios::out);
        }
        
        if (!db_io_.is_open()) {
            throw std::runtime_error("Failed to open database file: " + db_file);
        }
        
        db_io_.seekg(0, std::ios::end);
        size_t file_size = db_io_.tellg();
        next_page_id_ = static_cast<page_id_t>(file_size / PAGE_SIZE);
    }

    ~DiskManager() {
        ShutDown();
    }

    DiskManager(const DiskManager&) = delete;
    DiskManager& operator=(const DiskManager&) = delete;

    void ShutDown() {
        std::lock_guard<std::mutex> lock(latch_);
        if (db_io_.is_open()) {
            db_io_.flush();
            db_io_.close();
        }
    }

    void ReadPage(page_id_t page_id, char* page_data) {
        std::lock_guard<std::mutex> lock(latch_);
        
        if (page_id < 0) {
            throw std::invalid_argument("Invalid page ID: " + std::to_string(page_id));
        }
        
        size_t offset = static_cast<size_t>(page_id) * PAGE_SIZE;
        
        db_io_.seekg(offset);
        if (!db_io_.good()) {
            std::memset(page_data, 0, PAGE_SIZE);
            return;
        }
        
        db_io_.read(page_data, PAGE_SIZE);
        
        if (db_io_.gcount() < static_cast<std::streamsize>(PAGE_SIZE)) {
            std::memset(page_data + db_io_.gcount(), 0, 
                        PAGE_SIZE - db_io_.gcount());
        }
        
        db_io_.clear();
    }

    void WritePage(page_id_t page_id, const char* page_data) {
        std::lock_guard<std::mutex> lock(latch_);
        
        if (page_id < 0) {
            throw std::invalid_argument("Invalid page ID: " + std::to_string(page_id));
        }
        
        size_t offset = static_cast<size_t>(page_id) * PAGE_SIZE;
        
        db_io_.seekp(offset);
        db_io_.write(page_data, PAGE_SIZE);
        
        if (!db_io_.good()) {
            throw std::runtime_error("Failed to write page " + std::to_string(page_id));
        }
        
        db_io_.flush();
    }

    page_id_t AllocatePage() {
        return next_page_id_.fetch_add(1);
    }

    void DeallocatePage(page_id_t page_id) {
        std::lock_guard<std::mutex> lock(latch_);
        free_pages_.push_back(page_id);
    }

    page_id_t GetNumPages() const {
        return next_page_id_.load();
    }

    const std::string& GetFilePath() const {
        return db_file_path_;
    }

    void Flush() {
        std::lock_guard<std::mutex> lock(latch_);
        if (db_io_.is_open()) {
            db_io_.flush();
        }
    }

private:
    std::string db_file_path_;
    std::fstream db_io_;
    std::atomic<page_id_t> next_page_id_{0};
    std::vector<page_id_t> free_pages_;
    std::mutex latch_;
};

}
