#pragma once

#include <mydb/catalog/schema.hpp>
#include <mydb/catalog/value.hpp>
#include <mydb/storage/page.hpp>

#include <vector>
#include <cstring>

namespace mydb {

struct RID {
    page_id_t page_id{INVALID_PAGE_ID};
    uint32_t slot_num{0};

    RID() = default;
    RID(page_id_t pid, uint32_t slot) : page_id(pid), slot_num(slot) {}

    bool operator==(const RID& other) const {
        return page_id == other.page_id && slot_num == other.slot_num;
    }

    bool operator!=(const RID& other) const {
        return !(*this == other);
    }

    bool IsValid() const {
        return page_id != INVALID_PAGE_ID;
    }

    std::string ToString() const {
        return "(" + std::to_string(page_id) + ", " + std::to_string(slot_num) + ")";
    }
};

struct TupleHeader {
    uint32_t size{0};           
    float veracity{1.0f};       
    uint32_t timestamp{0};      
    bool is_deleted{false};     
};

class Tuple {
public:
    Tuple() = default;

    Tuple(std::vector<Value> values, const Schema* schema, float veracity = 1.0f)
        : values_(std::move(values)), veracity_(veracity) {
        
        if (schema && values_.size() != schema->GetColumnCount()) {
            throw std::runtime_error("Value count mismatch with schema");
        }
    }

    Value GetValue(const Schema* schema, uint32_t column_idx) const {
        if (column_idx >= values_.size()) {
            throw std::out_of_range("Column index out of range");
        }
        return values_[column_idx];
    }

    Value GetValue(const Schema* schema, const std::string& column_name) const {
        int idx = schema->GetColumnIndex(column_name);
        if (idx < 0) {
            throw std::runtime_error("Column not found: " + column_name);
        }
        return values_[idx];
    }

    const std::vector<Value>& GetValues() const { return values_; }

    float GetVeracity() const { return veracity_; }

    void SetVeracity(float veracity) { 
        veracity_ = std::clamp(veracity, 0.0f, 1.0f); 
    }

    const RID& GetRID() const { return rid_; }

    void SetRID(const RID& rid) { rid_ = rid; }

    bool IsEmpty() const { return values_.empty(); }

    size_t GetColumnCount() const { return values_.size(); }

    size_t SerializeTo(char* buffer, const Schema* schema) const {
        char* ptr = buffer;
        
        TupleHeader header;
        header.veracity = veracity_;
        header.is_deleted = false;
        std::memcpy(ptr, &header, sizeof(TupleHeader));
        ptr += sizeof(TupleHeader);
        
        for (size_t i = 0; i < values_.size(); ++i) {
            ptr += values_[i].SerializeTo(ptr);
        }
        
        size_t total_size = ptr - buffer;
        header.size = static_cast<uint32_t>(total_size);
        std::memcpy(buffer, &header, sizeof(TupleHeader));
        
        return total_size;
    }

    size_t GetSerializedSize(const Schema* schema) const {
        size_t size = sizeof(TupleHeader);
        for (size_t i = 0; i < values_.size(); ++i) {
            const auto& col = schema->GetColumn(static_cast<uint32_t>(i));
            if (col.IsVariableLength()) {
                size += sizeof(uint32_t) + values_[i].GetAsString().size();
            } else {
                size += col.GetLength();
            }
        }
        return size;
    }

    static Tuple DeserializeFrom(const char* buffer, const Schema* schema) {
        const char* ptr = buffer;
        
        TupleHeader header;
        std::memcpy(&header, ptr, sizeof(TupleHeader));
        ptr += sizeof(TupleHeader);
        
        std::vector<Value> values;
        values.reserve(schema->GetColumnCount());
        
        for (size_t i = 0; i < schema->GetColumnCount(); ++i) {
            const auto& col = schema->GetColumn(static_cast<uint32_t>(i));
            Value val = Value::DeserializeFrom(col.GetType(), ptr, col.GetLength());
            values.push_back(std::move(val));
            
            if (col.IsVariableLength()) {
                uint32_t len;
                std::memcpy(&len, ptr, sizeof(len));
                ptr += sizeof(len) + len;
            } else {
                ptr += col.GetLength();
            }
        }
        
        Tuple tuple(std::move(values), schema, header.veracity);
        return tuple;
    }

    std::string ToString(const Schema* schema = nullptr) const {
        std::string result = "(";
        for (size_t i = 0; i < values_.size(); ++i) {
            if (i > 0) result += ", ";
            result += values_[i].ToString();
        }
        result += ") [veracity=" + std::to_string(veracity_) + "]";
        return result;
    }

private:
    std::vector<Value> values_;
    float veracity_{1.0f};
    RID rid_;
};

}  

namespace std {
    template <>
    struct hash<mydb::RID> {
        size_t operator()(const mydb::RID& rid) const {
            return hash<int64_t>()(
                (static_cast<int64_t>(rid.page_id) << 32) | rid.slot_num
            );
        }
    };
}