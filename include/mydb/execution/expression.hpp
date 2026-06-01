#pragma once

#include <mydb/catalog/value.hpp>
#include <mydb/catalog/tuple.hpp>
#include <mydb/catalog/schema.hpp>
#include <mydb/util/similarity.hpp>

#include <memory>
#include <string>
#include <functional>

namespace mydb {

enum class ComparisonType {
    Equal,
    NotEqual,
    LessThan,
    LessEqual,
    GreaterThan,
    GreaterEqual,
    FuzzyLike
};

class AbstractExpression {
public:
    virtual ~AbstractExpression() = default;
    
    virtual Value Evaluate(const Tuple* tuple, const Schema* schema) const = 0;
    
    virtual bool EvaluateAsBool(const Tuple* tuple, const Schema* schema) const {
        return Evaluate(tuple, schema).GetAsBoolean();
    }
};

using ExpressionPtr = std::unique_ptr<AbstractExpression>;

class ConstantExpression : public AbstractExpression {
public:
    explicit ConstantExpression(Value value) : value_(std::move(value)) {}
    
    Value Evaluate(const Tuple*, const Schema*) const override {
        return value_;
    }

private:
    Value value_;
};

class ColumnExpression : public AbstractExpression {
public:
    explicit ColumnExpression(uint32_t column_idx) : column_idx_(column_idx) {}
    
    explicit ColumnExpression(std::string column_name) 
        : column_name_(std::move(column_name)), use_name_(true) {}
    
    Value Evaluate(const Tuple* tuple, const Schema* schema) const override {
        if (use_name_) {
            return tuple->GetValue(schema, column_name_);
        }
        return tuple->GetValue(schema, column_idx_);
    }

private:
    uint32_t column_idx_{0};
    std::string column_name_;
    bool use_name_{false};
};

class ComparisonExpression : public AbstractExpression {
public:
    ComparisonExpression(ExpressionPtr left, ExpressionPtr right, ComparisonType comp_type)
        : left_(std::move(left)), right_(std::move(right)), comp_type_(comp_type) {}
    
    Value Evaluate(const Tuple* tuple, const Schema* schema) const override {
        Value left_val = left_->Evaluate(tuple, schema);
        Value right_val = right_->Evaluate(tuple, schema);
        
        bool result = false;
        
        switch (comp_type_) {
            case ComparisonType::Equal:
                result = left_val == right_val;
                break;
            case ComparisonType::NotEqual:
                result = left_val != right_val;
                break;
            case ComparisonType::LessThan:
                result = left_val < right_val;
                break;
            case ComparisonType::LessEqual:
                result = left_val < right_val || left_val == right_val;
                break;
            case ComparisonType::GreaterThan:
                result = !(left_val < right_val || left_val == right_val);
                break;
            case ComparisonType::GreaterEqual:
                result = !(left_val < right_val);
                break;
            default:
                break;
        }
        
        return Value(result);
    }

private:
    ExpressionPtr left_;
    ExpressionPtr right_;
    ComparisonType comp_type_;
};

class FuzzyLikeExpression : public AbstractExpression {
public:
    FuzzyLikeExpression(ExpressionPtr column, std::string pattern,
                        double threshold = 0.8,
                        std::string algorithm = "jaro_winkler")
        : column_(std::move(column)), 
          pattern_(std::move(pattern)),
          threshold_(threshold),
          algorithm_(std::move(algorithm)) {}
    
    Value Evaluate(const Tuple* tuple, const Schema* schema) const override {
        Value col_val = column_->Evaluate(tuple, schema);
        
        if (col_val.GetTypeId() != TypeId::VARCHAR) {
            return Value(false);
        }
        
        const std::string& str = col_val.GetAsString();
        double similarity = Similarity::GetSimilarity(str, pattern_, algorithm_);
        
        return Value(similarity >= threshold_);
    }
    
    double GetSimilarity(const Tuple* tuple, const Schema* schema) const {
        Value col_val = column_->Evaluate(tuple, schema);
        
        if (col_val.GetTypeId() != TypeId::VARCHAR) {
            return 0.0;
        }
        
        return Similarity::GetSimilarity(col_val.GetAsString(), pattern_, algorithm_);
    }

private:
    ExpressionPtr column_;
    std::string pattern_;
    double threshold_;
    std::string algorithm_;
};

class VeracityExpression : public AbstractExpression {
public:
    VeracityExpression(ComparisonType comp_type, double threshold)
        : comp_type_(comp_type), threshold_(threshold) {}
    
    Value Evaluate(const Tuple* tuple, const Schema*) const override {
        float veracity = tuple->GetVeracity();
        bool result = false;
        
        switch (comp_type_) {
            case ComparisonType::Equal:
                result = std::abs(veracity - threshold_) < 0.0001;
                break;
            case ComparisonType::NotEqual:
                result = std::abs(veracity - threshold_) >= 0.0001;
                break;
            case ComparisonType::LessThan:
                result = veracity < threshold_;
                break;
            case ComparisonType::LessEqual:
                result = veracity <= threshold_;
                break;
            case ComparisonType::GreaterThan:
                result = veracity > threshold_;
                break;
            case ComparisonType::GreaterEqual:
                result = veracity >= threshold_;
                break;
            default:
                break;
        }
        
        return Value(result);
    }

private:
    ComparisonType comp_type_;
    double threshold_;
};

class AndExpression : public AbstractExpression {
public:
    AndExpression(ExpressionPtr left, ExpressionPtr right)
        : left_(std::move(left)), right_(std::move(right)) {}
    
    Value Evaluate(const Tuple* tuple, const Schema* schema) const override {
        bool left_result = left_->EvaluateAsBool(tuple, schema);
        if (!left_result) {
            return Value(false);
        }
        return Value(right_->EvaluateAsBool(tuple, schema));
    }

private:
    ExpressionPtr left_;
    ExpressionPtr right_;
};

class OrExpression : public AbstractExpression {
public:
    OrExpression(ExpressionPtr left, ExpressionPtr right)
        : left_(std::move(left)), right_(std::move(right)) {}
    
    Value Evaluate(const Tuple* tuple, const Schema* schema) const override {
        bool left_result = left_->EvaluateAsBool(tuple, schema);
        if (left_result) {
            return Value(true);
        }
        return Value(right_->EvaluateAsBool(tuple, schema));
    }

private:
    ExpressionPtr left_;
    ExpressionPtr right_;
};

}
