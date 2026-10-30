#pragma once

#include "SharedValues.hpp"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace ShaderRecompiler {

// Resource keys serialize ordinary fields in order and represent image-table
// maps as shared payloads inserted at canonical ordinary-word offsets.
class ResourceKey {
public:
    struct MapPart {
        std::size_t ordinaryOffset;
        SharedValues<std::uint32_t> values;
        friend bool operator==(const MapPart& left, const MapPart& right) {
            return left.ordinaryOffset == right.ordinaryOffset && left.values == right.values;
        }
    };

    ResourceKey() = default;
    ResourceKey(std::initializer_list<std::uint32_t> words) : ordinaryWords(words), logicalSize(words.size()) {}
    explicit ResourceKey(std::span<const std::uint32_t> words) : ordinaryWords(words.begin(), words.end()), logicalSize(words.size()) {}

    void reserve(std::size_t count) { ordinaryWords.reserve(count); }
    void push_back(std::uint32_t word) {
        CheckLogicalGrowth(1);
        ordinaryWords.push_back(word);
        ++logicalSize;
    }
    void AppendWords(std::span<const std::uint32_t> words) {
        CheckLogicalGrowth(words.size());
        ordinaryWords.insert(ordinaryWords.end(), words.begin(), words.end());
        logicalSize += words.size();
    }
    void Append(const ResourceKey& other) {
        if (this == &other) {
            ResourceKey copy(other);
            Append(copy);
            return;
        }
        CheckLogicalGrowth(other.logicalSize);
        const auto ordinaryOffset = ordinaryWords.size();
        ordinaryWords.reserve(ordinaryWords.size() + other.ordinaryWords.size());
        maps.reserve(maps.size() + other.maps.size());
        ordinaryWords.insert(ordinaryWords.end(), other.ordinaryWords.begin(), other.ordinaryWords.end());
        for (const auto& map : other.maps) maps.push_back({ordinaryOffset + map.ordinaryOffset, map.values});
        logicalSize += other.logicalSize;
    }
    void AppendMap(const SharedValues<std::uint32_t>& values) {
        if (values.empty()) return;
        CheckLogicalGrowth(values.size());
        maps.push_back({ordinaryWords.size(), values});
        logicalSize += values.size();
    }

    [[nodiscard]] std::size_t size() const { return logicalSize; }
    [[nodiscard]] bool empty() const { return logicalSize == 0; }
    [[nodiscard]] std::uint32_t back() const { return (*this)[logicalSize - 1]; }
    [[nodiscard]] std::uint32_t operator[](std::size_t index) const {
        if (index >= logicalSize) throw std::out_of_range("ResourceKey index");
        std::size_t ordinaryOffset = 0;
        for (const auto& map : maps) {
            const auto ordinaryCount = map.ordinaryOffset - ordinaryOffset;
            if (index < ordinaryCount) return ordinaryWords[ordinaryOffset + index];
            index -= ordinaryCount;
            if (index < map.values.size()) return map.values[index];
            index -= map.values.size();
            ordinaryOffset = map.ordinaryOffset;
        }
        return ordinaryWords[ordinaryOffset + index];
    }
    void clear() {
        ordinaryWords.clear();
        maps.clear();
        logicalSize = 0;
    }
    [[nodiscard]] std::size_t MarkWord() const { return ordinaryWords.size(); }
    void SetWord(std::size_t mark, std::uint32_t value) {
        if (mark >= ordinaryWords.size()) throw std::out_of_range("ResourceKey ordinary word mark");
        ordinaryWords[mark] = value;
    }
    [[nodiscard]] std::span<const std::uint32_t> OrdinaryWords() const { return ordinaryWords; }
    [[nodiscard]] std::span<const MapPart> Maps() const { return maps; }

    [[nodiscard]] std::vector<std::uint32_t> Flatten() const {
        std::vector<std::uint32_t> result;
        result.reserve(logicalSize);
        std::size_t ordinaryOffset = 0;
        for (const auto& map : maps) {
            result.insert(result.end(), ordinaryWords.begin() + ordinaryOffset, ordinaryWords.begin() + map.ordinaryOffset);
            result.insert(result.end(), map.values.begin(), map.values.end());
            ordinaryOffset = map.ordinaryOffset;
        }
        result.insert(result.end(), ordinaryWords.begin() + ordinaryOffset, ordinaryWords.end());
        return result;
    }

    [[nodiscard]] std::size_t Hash() const noexcept {
        std::uint64_t hash = 0xcbf29ce484222325ull;
        const auto mix = [&hash](std::uint64_t value) { hash = (hash ^ value) * 0x100000001b3ull; };
        mix(ordinaryWords.size());
        for (const auto word : ordinaryWords) mix(word);
        mix(maps.size());
        for (const auto& map : maps) {
            mix(map.ordinaryOffset);
            mix(map.values.size());
            mix(map.values.Hash());
        }
        return static_cast<std::size_t>(hash);
    }
    friend bool operator==(const ResourceKey& left, const ResourceKey& right) {
        return left.logicalSize == right.logicalSize && left.ordinaryWords == right.ordinaryWords && left.maps == right.maps;
    }

private:
    void CheckLogicalGrowth(std::size_t growth) const {
        if (growth > std::numeric_limits<std::size_t>::max() - logicalSize)
            throw std::length_error("ResourceKey logical size overflow");
    }

    std::vector<std::uint32_t> ordinaryWords;
    std::vector<MapPart> maps;
    std::size_t logicalSize = 0;
};

}
