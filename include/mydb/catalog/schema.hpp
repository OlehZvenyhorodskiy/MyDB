#pragma once

#include <mydb/catalog/value.hpp>

#include <string>
#include <vector>
#include <stdexcept>
#include <unordered_map>

namespace mydb {

class Column {
public:
    Column(std::string name, TypeId type)
        : name_(std::move(name)), 
          type_(type), 
          length_(GetTypeSize(type)),
          variable_length_(false) {
        if (type == TypeId::VARCHAR) {
            throw std::invalid_argument("VARCHAR requires explicit length");
        }
    }

    Column(std::string name, TypeId type, uint32_t length)
        : name_(std::move(name)), 
          type_(type), 
          length_(length),
          variable_length_(type == TypeId::VARCHAR) {}

    const std::string& GetName() const { return name_; }

    TypeId GetType() const { return type_; }

    uint32_t GetLength() const { return length_; }

    bool IsVariableLength() const { return variable_length_; }

    uint32_t GetStorageSize() const {
        if (variable_length_) {
            return sizeof(uint32_t) + length_;  
        }
        return length_;
    }

    uint32_t GetOffset() const { return offset_; }

    void SetOffset(uint32_t offset) { offset_ = offset; }

    std::string ToString() const {
        std::string result = name_ + " " + TypeIdToString(type_);
        if (type_ == TypeId::VARCHAR) {
            result += "(" + std::to_string(length_) + ")";
        }
        return result;
    }

private:
    std::string name_;
    TypeId type_;
    uint32_t length_;
    bool variable_length_;
    uint32_t offset_{0};  
};

class Schema {
public:
    explicit Schema(std::vector<Column> columns)
        : columns_(std::move(columns)) {
        
        uint32_t offset = 0;
        for (size_t i = 0; i < columns_.size(); ++i) {
            columns_[i].SetOffset(offset);
            column_indices_[columns_[i].GetName()] = i;
            offset += columns_[i].GetStorageSize();
        }
        tuple_size_ = offset;
    }

    const Column& GetColumn(uint32_t idx) const {
        if (idx >= columns_.size()) {
            throw std::out_of_range("Column index out of range");
        }
        return columns_[idx];
    }

    const Column& GetColumn(const std::string& name) const {
        auto it = column_indices_.find(name);
        if (it == column_indices_.end()) {
            throw std::out_of_range("Column not found: " + name);
        }
        return columns_[it->second];
    }

    int GetColumnIndex(const std::string& name) const {
        auto it = column_indices_.find(name);
        if (it == column_indices_.end()) {
            return -1;
        }
        return static_cast<int>(it->second);
    }

    const std::vector<Column>& GetColumns() const { return columns_; }

    size_t GetColumnCount() const { return columns_.size(); }

    uint32_t GetTupleSize() const { return tuple_size_; }

    bool HasVariableLengthColumns() const {
        for (const auto& col : columns_) {
            if (col.IsVariableLength()) return true;
        }
        return false;
    }

    std::string ToString() const {
        std::string result = "(";
        for (size_t i = 0; i < columns_.size(); ++i) {
            if (i > 0) result += ", ";
            result += columns_[i].ToString();
        }
        result += ")";
        return result;
    }

private:
    std::vector<Column> columns_;
    std::unordered_map<std::string, size_t> column_indices_;
    uint32_t tuple_size_{0};
};

}