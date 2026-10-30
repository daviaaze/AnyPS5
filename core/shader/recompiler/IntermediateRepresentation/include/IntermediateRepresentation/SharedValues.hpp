#ifndef CORE_SHADER_RECOMPILIER_INTERMEDIATEREPRESENTATION_INCLUDE_INTERMEDIATEREPRESENTATION_SHAREDVALUES_HPP
#define CORE_SHADER_RECOMPILIER_INTERMEDIATEREPRESENTATION_INCLUDE_INTERMEDIATEREPRESENTATION_SHAREDVALUES_HPP

#include <algorithm>
#include <cstddef>
#include <atomic>
#include <cstdint>
#include <memory>
#include <ranges>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace ShaderRecompiler {

template <typename T>
class SharedValues {
public:
    SharedValues() = default;
    explicit SharedValues(std::vector<T> values) : block(values.empty() ? nullptr : std::make_shared<const Block>(std::move(values))) {}

    [[nodiscard]] std::size_t size() const { return block != nullptr ? block->values.size() : 0u; }
    [[nodiscard]] bool empty() const { return block == nullptr; }
    [[nodiscard]] const T* data() const { return block != nullptr ? block->values.data() : nullptr; }
    [[nodiscard]] const T* begin() const { return data(); }
    [[nodiscard]] const T* end() const { return size() != 0u ? data() + size() : data(); }
    [[nodiscard]] const T& operator[](std::size_t index) const { return block->values[index]; }
    [[nodiscard]] const T& at(std::size_t index) const {
        if (index >= size()) throw std::out_of_range("SharedValues::at");
        return block->values[index];
    }
    [[nodiscard]] const T& front() const { return at(0); }
    [[nodiscard]] const T& back() const { return at(size() - 1u); }
    [[nodiscard]] std::span<const T> Span() const { return {data(), size()}; }
    [[nodiscard]] std::vector<T> Copy() const { return block != nullptr ? block->values : std::vector<T>{}; }
    [[nodiscard]] bool Shares(const SharedValues& other) const { return block == other.block; }

    [[nodiscard]] std::uint64_t Hash() const {
        constexpr std::uint64_t Basis = 0xcbf29ce484222325ull;
        if (block == nullptr) return Basis;
        if (const auto known = block->hash.load(std::memory_order_relaxed); known != 0u) return known;
        std::uint64_t hash = Basis;
        const auto mix = [&](std::uint64_t value) { hash = (hash ^ value) * 0x100000001b3ull; };
        mix(block->values.size());
        for (const auto& value : block->values) {
            if constexpr (requires { value.dwordCount; value.availableDwordMask; value.dwords; }) {
                mix(value.dwordCount);
                mix(value.availableDwordMask);
                for (const auto dword : value.dwords) mix(dword);
            } else if constexpr (requires { value.bounded; value.readBegin; value.readEnd; value.writeBegin; value.writeEnd; }) {
                mix(value.bounded);
                mix(value.readBegin);
                mix(value.readEnd);
                mix(value.writeBegin);
                mix(value.writeEnd);
            } else if constexpr (std::is_integral_v<T>) {
                mix(value);
            } else {
                mix(value.first);
                mix(value.second);
            }
        }
        hash = hash != 0u ? hash : 1u;
        block->hash.store(hash, std::memory_order_relaxed);
        return hash;
    }

    friend bool operator==(const SharedValues& left, const SharedValues& right) { return left.block == right.block || std::ranges::equal(left.Span(), right.Span()); }
    friend bool operator==(const SharedValues& left, const std::vector<T>& right) { return std::ranges::equal(left.Span(), right); }

private:
    struct Block {
        explicit Block(std::vector<T> values) : values(std::move(values)) {}
        std::vector<T> values;
        mutable std::atomic<std::uint64_t> hash{0};
    };
    std::shared_ptr<const Block> block;
};

}

#endif
