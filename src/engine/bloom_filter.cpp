#include <mydb/engine/bloom_filter.hpp>

#include <cmath>
#include <cstring>
#include <memory>

namespace mydb {

namespace {

inline uint32_t rotl32(uint32_t x, int8_t r) {
    return (x << r) | (x >> (32 - r));
}

inline uint32_t fmix32(uint32_t h) {
    h ^= h >> 16;
    h *= 0x85ebca6b;
    h ^= h >> 13;
    h *= 0xc2b2ae35;
    h ^= h >> 16;
    return h;
}

} 

uint32_t MurmurHash3_32(const void* key, size_t len, uint32_t seed) {
    const uint8_t* data = static_cast<const uint8_t*>(key);
    const int nblocks = static_cast<int>(len / 4);
    
    uint32_t h1 = seed;
    
    const uint32_t c1 = 0xcc9e2d51;
    const uint32_t c2 = 0x1b873593;
    
    const uint32_t* blocks = reinterpret_cast<const uint32_t*>(data + nblocks * 4);
    
    for (int i = -nblocks; i; i++) {
        uint32_t k1;
        std::memcpy(&k1, blocks + i, sizeof(k1));
        
        k1 *= c1;
        k1 = rotl32(k1, 15);
        k1 *= c2;
        
        h1 ^= k1;
        h1 = rotl32(h1, 13);
        h1 = h1 * 5 + 0xe6546b64;
    }
    
    const uint8_t* tail = data + nblocks * 4;
    uint32_t k1 = 0;
    
    switch (len & 3) {
        case 3: k1 ^= static_cast<uint32_t>(tail[2]) << 16; [[fallthrough]];
        case 2: k1 ^= static_cast<uint32_t>(tail[1]) << 8;  [[fallthrough]];
        case 1: k1 ^= static_cast<uint32_t>(tail[0]);
                k1 *= c1;
                k1 = rotl32(k1, 15);
                k1 *= c2;
                h1 ^= k1;
    }
    
    h1 ^= static_cast<uint32_t>(len);
    h1 = fmix32(h1);
    
    return h1;
}

void MurmurHash3_128(const void* key, size_t len, uint32_t seed, void* out) {
    uint32_t* result = static_cast<uint32_t*>(out);
    result[0] = MurmurHash3_32(key, len, seed);
    result[1] = MurmurHash3_32(key, len, seed + 1);
    result[2] = MurmurHash3_32(key, len, seed + 2);
    result[3] = MurmurHash3_32(key, len, seed + 3);
}

std::unique_ptr<BloomFilter> BloomFilter::Create(
    const std::vector<std::string>& keys,
    size_t bits_per_key
) {
    auto filter = std::unique_ptr<BloomFilter>(new BloomFilter());
    
    size_t num_bits = keys.size() * bits_per_key;
    
    size_t num_bytes = (num_bits + 7) / 8;
    num_bits = num_bytes * 8;
    
    if (num_bytes < 8) num_bytes = 8;
    
    filter->bits().resize(num_bytes, 0);
    
    filter->num_hashes() = static_cast<size_t>(
        std::ceil(static_cast<double>(bits_per_key) * 0.693147)
    );
    if (filter->num_hashes() < 1) filter->num_hashes() = 1;
    if (filter->num_hashes() > 30) filter->num_hashes() = 30;
    
    filter->num_keys() = keys.size();
    
    for (const auto& key : keys) {
        filter->AddKey(Slice(key));
    }
    
    return filter;
}

std::unique_ptr<BloomFilter> BloomFilter::Deserialize(const Slice& data) {
    if (data.size() < 5) {
        return nullptr;
    }
    
    auto filter = std::unique_ptr<BloomFilter>(new BloomFilter());
    
    filter->num_hashes() = 0;
    for (int i = 0; i < 4; ++i) {
        filter->num_hashes() |= (static_cast<size_t>(static_cast<unsigned char>(data[i])) << (i * 8));
    }
    
    filter->bits().resize(data.size() - 4);
    std::memcpy(filter->bits().data(), data.data() + 4, data.size() - 4);
    
    return filter;
}

void BloomFilter::AddKey(const Slice& key) {
    auto [h1, h2] = HashKey(key);
    
    size_t num_bits = bits_.size() * 8;
    
    for (size_t i = 0; i < num_hashes_; ++i) {
        size_t bit_pos = (h1 + i * h2) % num_bits;
        bits_[bit_pos / 8] |= (1 << (bit_pos % 8));
    }
}

bool BloomFilter::MayContain(const Slice& key) const {
    auto [h1, h2] = HashKey(key);
    
    size_t num_bits = bits_.size() * 8;
    
    for (size_t i = 0; i < num_hashes_; ++i) {
        size_t bit_pos = (h1 + i * h2) % num_bits;
        if (!(bits_[bit_pos / 8] & (1 << (bit_pos % 8)))) {
            return false;
        }
    }
    
    return true;
}

std::string BloomFilter::Serialize() const {
    std::string result;
    result.reserve(4 + bits_.size());
    
    for (int i = 0; i < 4; ++i) {
        result.push_back(static_cast<char>((num_hashes_ >> (i * 8)) & 0xFF));
    }
    
    result.append(reinterpret_cast<const char*>(bits_.data()), bits_.size());
    
    return result;
}

double BloomFilter::FalsePositiveRate() const {
    if (num_keys_ == 0) return 0.0;
    
    double k = static_cast<double>(num_hashes_);
    double n = static_cast<double>(num_keys_);
    double m = static_cast<double>(bits_.size() * 8);
    
    return std::pow(1.0 - std::exp(-k * n / m), k);
}

std::pair<uint32_t, uint32_t> BloomFilter::HashKey(const Slice& key) const {
    uint32_t h1 = MurmurHash3_32(key.data(), key.size(), 0);
    uint32_t h2 = MurmurHash3_32(key.data(), key.size(), h1);
    return {h1, h2};
}

}