#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <algorithm>
#include <cmath>

namespace mydb {

class Similarity {
public:
    static int Levenshtein(std::string_view s1, std::string_view s2) {
        const size_t m = s1.size();
        const size_t n = s2.size();
        
        if (m == 0) return static_cast<int>(n);
        if (n == 0) return static_cast<int>(m);
        
        if (m > n) {
            return Levenshtein(s2, s1);
        }
        
        std::vector<int> prev_row(n + 1);
        std::vector<int> curr_row(n + 1);
        
        for (size_t j = 0; j <= n; ++j) {
            prev_row[j] = static_cast<int>(j);
        }
        
        for (size_t i = 1; i <= m; ++i) {
            curr_row[0] = static_cast<int>(i);
            
            for (size_t j = 1; j <= n; ++j) {
                int cost = (s1[i - 1] == s2[j - 1]) ? 0 : 1;
                
                curr_row[j] = std::min({
                    prev_row[j] + 1,
                    curr_row[j - 1] + 1,
                    prev_row[j - 1] + cost
                });
            }
            
            std::swap(prev_row, curr_row);
        }
        
        return prev_row[n];
    }

    static double NormalizedLevenshtein(std::string_view s1, std::string_view s2) {
        if (s1.empty() && s2.empty()) {
            return 1.0;
        }
        
        int distance = Levenshtein(s1, s2);
        size_t max_len = std::max(s1.size(), s2.size());
        
        return 1.0 - (static_cast<double>(distance) / static_cast<double>(max_len));
    }

    static double Jaro(std::string_view s1, std::string_view s2) {
        if (s1.empty() && s2.empty()) {
            return 1.0;
        }
        if (s1.empty() || s2.empty()) {
            return 0.0;
        }
        
        const size_t len1 = s1.size();
        const size_t len2 = s2.size();
        
        const size_t match_distance = (std::max(len1, len2) / 2) - 1;
        
        std::vector<bool> s1_matched(len1, false);
        std::vector<bool> s2_matched(len2, false);
        
        int matches = 0;
        int transpositions = 0;
        
        for (size_t i = 0; i < len1; ++i) {
            size_t start = (i > match_distance) ? i - match_distance : 0;
            size_t end = std::min(i + match_distance + 1, len2);
            
            for (size_t j = start; j < end; ++j) {
                if (s2_matched[j] || s1[i] != s2[j]) {
                    continue;
                }
                s1_matched[i] = true;
                s2_matched[j] = true;
                matches++;
                break;
            }
        }
        
        if (matches == 0) {
            return 0.0;
        }
        
        size_t k = 0;
        for (size_t i = 0; i < len1; ++i) {
            if (!s1_matched[i]) continue;
            
            while (!s2_matched[k]) {
                k++;
            }
            
            if (s1[i] != s2[k]) {
                transpositions++;
            }
            k++;
        }
        
        double m = static_cast<double>(matches);
        double t = static_cast<double>(transpositions) / 2.0;
        
        return (m / len1 + m / len2 + (m - t) / m) / 3.0;
    }

    static double JaroWinkler(std::string_view s1, std::string_view s2, 
                              double prefix_scale = 0.1) {
        double jaro_sim = Jaro(s1, s2);
        
        size_t prefix_len = 0;
        size_t max_prefix = std::min({s1.size(), s2.size(), size_t{4}});
        
        for (size_t i = 0; i < max_prefix; ++i) {
            if (s1[i] == s2[i]) {
                prefix_len++;
            } else {
                break;
            }
        }
        
        return jaro_sim + (prefix_len * prefix_scale * (1.0 - jaro_sim));
    }

    static int LevenshteinIgnoreCase(std::string_view s1, std::string_view s2) {
        std::string lower1 = ToLower(s1);
        std::string lower2 = ToLower(s2);
        return Levenshtein(lower1, lower2);
    }

    static double JaroWinklerIgnoreCase(std::string_view s1, std::string_view s2,
                                        double prefix_scale = 0.1) {
        std::string lower1 = ToLower(s1);
        std::string lower2 = ToLower(s2);
        return JaroWinkler(lower1, lower2, prefix_scale);
    }

    static bool IsSimilar(std::string_view s1, std::string_view s2,
                          double threshold = 0.8,
                          const std::string& algorithm = "jaro_winkler") {
        double sim;
        if (algorithm == "levenshtein") {
            sim = NormalizedLevenshtein(s1, s2);
        } else {
            sim = JaroWinkler(s1, s2);
        }
        return sim >= threshold;
    }

    static double GetSimilarity(std::string_view s1, std::string_view s2,
                                const std::string& algorithm = "jaro_winkler") {
        if (algorithm == "levenshtein") {
            return NormalizedLevenshtein(s1, s2);
        } else {
            return JaroWinkler(s1, s2);
        }
    }

private:
    static std::string ToLower(std::string_view s) {
        std::string result(s);
        std::transform(result.begin(), result.end(), result.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        return result;
    }
};

}
