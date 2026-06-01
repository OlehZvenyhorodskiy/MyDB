#include <mydb/engine/sstable.hpp>
#include <mydb/engine/bloom_filter.hpp>

#include <spdlog/spdlog.h>

#include <filesystem>
#include <algorithm>

namespace mydb {

class SSTableIteratorImpl : public SSTableIterator {
public:
    explicit SSTableIteratorImpl(const SSTableReader* reader)
        : reader_(reader) {}
    
    bool Valid() const override { return valid_; }
    
    void SeekToFirst() override {
        block_index_ = 0;
        entry_index_ = 0;
        LoadBlock(0);
        if (!block_data_.empty()) {
            ParseCurrentEntry();
        }
    }
    
    void SeekToLast() override {
        valid_ = false;
    }
    
    void Seek(const Slice& key) override {
        auto result = reader_->FindBlock(key);
        if (!result.ok()) {
            valid_ = false;
            return;
        }
        
        block_index_ = result.value();
        LoadBlock(block_index_);
        
        entry_index_ = 0;
        offset_in_block_ = 0;
        
        while (ParseCurrentEntry()) {
            if (current_key_ >= key.ToString()) {
                return;
            }
            entry_index_++;
        }
        
        if (block_index_ + 1 < reader_->GetIndex().size()) {
            block_index_++;
            LoadBlock(block_index_);
            entry_index_ = 0;
            offset_in_block_ = 0;
            ParseCurrentEntry();
        } else {
            valid_ = false;
        }
    }
    
    void Next() override {
        entry_index_++;
        if (!ParseCurrentEntry()) {
            block_index_++;
            if (block_index_ < reader_->GetIndex().size()) {
                LoadBlock(block_index_);
                entry_index_ = 0;
                offset_in_block_ = 0;
                ParseCurrentEntry();
            } else {
                valid_ = false;
            }
        }
    }
    
    void Prev() override {
        valid_ = false;
    }
    
    Slice key() const override { return current_key_; }
    Slice value() const override { return current_value_; }
    
private:
    void LoadBlock(size_t index) {
        if (index >= reader_->GetIndex().size()) {
            block_data_.clear();
            valid_ = false;
            return;
        }
        
        const auto& entry = reader_->GetIndex()[index];
        auto result = reader_->ReadBlock(entry.block_offset, entry.block_size);
        if (result.ok()) {
            block_data_ = std::move(result.value());
            offset_in_block_ = 0;
        } else {
            block_data_.clear();
            valid_ = false;
        }
    }
    
    bool ParseCurrentEntry() {
        if (offset_in_block_ + 8 > block_data_.size()) {
            valid_ = false;
            return false;
        }
        
        uint32_t key_len = 0;
        for (int i = 0; i < 4; ++i) {
            key_len |= (static_cast<uint32_t>(
                static_cast<unsigned char>(block_data_[offset_in_block_ + i])
            ) << (i * 8));
        }
        offset_in_block_ += 4;
        
        if (offset_in_block_ + key_len + 4 > block_data_.size()) {
            valid_ = false;
            return false;
        }
        
        current_key_ = std::string(block_data_.data() + offset_in_block_, key_len);
        offset_in_block_ += key_len;
        
        uint32_t val_len = 0;
        for (int i = 0; i < 4; ++i) {
            val_len |= (static_cast<uint32_t>(
                static_cast<unsigned char>(block_data_[offset_in_block_ + i])
            ) << (i * 8));
        }
        offset_in_block_ += 4;
        
        if (offset_in_block_ + val_len > block_data_.size()) {
            valid_ = false;
            return false;
        }
        
        current_value_ = std::string(block_data_.data() + offset_in_block_, val_len);
        offset_in_block_ += val_len;
        
        valid_ = true;
        return true;
    }
    
    const SSTableReader* reader_;
    bool valid_{false};
    
    size_t block_index_{0};
    size_t entry_index_{0};
    
    std::string block_data_;
    size_t offset_in_block_{0};
    
    std::string current_key_;
    std::string current_value_;
};

SSTableReader::SSTableReader(const std::string& filename)
    : filename_(filename) {}

SSTableReader::~SSTableReader() {
    if (file_.is_open()) {
        file_.close();
    }
}

Result<std::unique_ptr<SSTableReader>> SSTableReader::Open(const std::string& filename) {
    if (!std::filesystem::exists(filename)) {
        return Status::NotFound("SSTable file not found: " + filename);
    }
    
    auto reader = std::unique_ptr<SSTableReader>(new SSTableReader(filename));
    
    Status s = reader->Initialize();
    if (!s.ok()) {
        return s;
    }
    
    return std::move(reader);
}

Status SSTableReader::Initialize() {
    file_.open(filename_, std::ios::binary);
    if (!file_.is_open()) {
        return Status::IOError("Failed to open SSTable: " + filename_);
    }
    
    file_.seekg(0, std::ios::end);
    file_size_ = static_cast<uint64_t>(file_.tellg());
    
    if (file_size_ < SSTableFooter::kEncodedLength) {
        return Status::Corruption("SSTable too small");
    }
    
    MYDB_RETURN_IF_ERROR(ReadFooter());
    MYDB_RETURN_IF_ERROR(ReadIndex());
    MYDB_RETURN_IF_ERROR(ReadBloomFilter());
    
    if (!index_.empty()) {
        smallest_key_ = index_.front().first_key;
        
        auto block_result = ReadBlock(
            index_.back().block_offset, 
            index_.back().block_size
        );
        if (block_result.ok()) {
            const auto& block = block_result.value();
            size_t offset = 0;
            std::string last_key;
            
            while (offset + 8 <= block.size()) {
                uint32_t key_len = 0;
                for (int i = 0; i < 4; ++i) {
                    key_len |= (static_cast<uint32_t>(
                        static_cast<unsigned char>(block[offset + i])
                    ) << (i * 8));
                }
                offset += 4;
                
                if (offset + key_len + 4 > block.size()) break;
                
                last_key = std::string(block.data() + offset, key_len);
                offset += key_len;
                
                uint32_t val_len = 0;
                for (int i = 0; i < 4; ++i) {
                    val_len |= (static_cast<uint32_t>(
                        static_cast<unsigned char>(block[offset + i])
                    ) << (i * 8));
                }
                offset += 4 + val_len;
            }
            
            largest_key_ = last_key;
        }
    }
    
    spdlog::info("Opened SSTable: {}, {} entries, {} bytes", 
                 filename_, footer_.entry_count, file_size_);
    
    return Status::Ok();
}

Status SSTableReader::ReadFooter() {
    file_.seekg(-static_cast<std::streamoff>(SSTableFooter::kEncodedLength), std::ios::end);
    
    std::string footer_data(SSTableFooter::kEncodedLength, '\0');
    file_.read(footer_data.data(), SSTableFooter::kEncodedLength);
    
    if (!file_.good()) {
        return Status::IOError("Failed to read footer");
    }
    
    auto result = SSTableFooter::Decode(footer_data);
    if (!result.ok()) {
        return result.status();
    }
    
    footer_ = result.value();
    return Status::Ok();
}

Status SSTableReader::ReadIndex() {
    auto block_result = ReadBlock(footer_.index_block_offset, footer_.index_block_size);
    if (!block_result.ok()) {
        return block_result.status();
    }
    
    const auto& data = block_result.value();
    if (data.size() < 4) {
        return Status::Corruption("Index block too small");
    }
    
    size_t offset = 0;
    
    uint32_t num_entries = 0;
    for (int i = 0; i < 4; ++i) {
        num_entries |= (static_cast<uint32_t>(
            static_cast<unsigned char>(data[offset + i])
        ) << (i * 8));
    }
    offset += 4;
    
    for (uint32_t i = 0; i < num_entries; ++i) {
        Slice remaining(data.data() + offset, data.size() - offset);
        auto entry_result = IndexEntry::Decode(remaining);
        if (!entry_result.ok()) {
            return entry_result.status();
        }
        
        const auto& entry = entry_result.value();
        offset += 4 + entry.first_key.size() + 16;
        
        index_.push_back(entry);
    }
    
    return Status::Ok();
}

Status SSTableReader::ReadBloomFilter() {
    auto block_result = ReadBlock(footer_.bloom_filter_offset, footer_.bloom_filter_size);
    if (!block_result.ok()) {
        return block_result.status();
    }
    
    bloom_filter_ = BloomFilter::Deserialize(block_result.value());
    if (!bloom_filter_) {
        return Status::Corruption("Failed to deserialize bloom filter");
    }
    
    return Status::Ok();
}

Result<std::string> SSTableReader::ReadBlock(uint64_t offset, uint64_t size) const {
    file_.seekg(static_cast<std::streamoff>(offset));
    
    std::string block(size, '\0');
    file_.read(block.data(), static_cast<std::streamsize>(size));
    
    if (!file_.good() && !file_.eof()) {
        return Status::IOError("Failed to read block");
    }
    
    return block;
}

Result<size_t> SSTableReader::FindBlock(const Slice& key) const {
    if (index_.empty()) {
        return Status::NotFound("Empty SSTable");
    }
    
    size_t left = 0;
    size_t right = index_.size();
    
    while (left < right) {
        size_t mid = left + (right - left) / 2;
        if (index_[mid].first_key <= key.ToString()) {
            left = mid + 1;
        } else {
            right = mid;
        }
    }
    
    return left > 0 ? left - 1 : 0;
}

bool SSTableReader::MayContain(const Slice& key) const {
    if (!bloom_filter_) return true;
    return bloom_filter_->MayContain(key);
}

Result<std::string> SSTableReader::Get(const Slice& key) {
    if (!MayContain(key)) {
        return Status::NotFound("Key not in bloom filter");
    }
    
    auto iter = NewIterator();
    iter->Seek(key);
    
    if (iter->Valid() && iter->key() == key) {
        return iter->value().ToString();
    }
    
    return Status::NotFound("Key not found in SSTable");
}

std::unique_ptr<SSTableIterator> SSTableReader::NewIterator() const {
    return std::make_unique<SSTableIteratorImpl>(this);
}

Status SSTableMerger::Merge(
    const std::vector<SSTableReader*>& inputs,
    const std::string& output_filename
) {
    if (inputs.empty()) {
        return Status::InvalidArgument("No inputs to merge");
    }
    
    SSTableBuilder builder(output_filename);
    
    std::vector<std::unique_ptr<SSTableIterator>> iters;
    for (auto* reader : inputs) {
        auto iter = reader->NewIterator();
        iter->SeekToFirst();
        if (iter->Valid()) {
            iters.push_back(std::move(iter));
        }
    }
    
    while (!iters.empty()) {
        size_t min_idx = 0;
        for (size_t i = 1; i < iters.size(); ++i) {
            if (iters[i]->key() < iters[min_idx]->key()) {
                min_idx = i;
            }
        }
        
        Status s = builder.Add(iters[min_idx]->key(), iters[min_idx]->value());
        if (!s.ok()) {
            builder.Abandon();
            return s;
        }
        
        iters[min_idx]->Next();
        
        if (!iters[min_idx]->Valid()) {
            iters.erase(iters.begin() + min_idx);
        }
    }
    
    return builder.Finish();
}

}
