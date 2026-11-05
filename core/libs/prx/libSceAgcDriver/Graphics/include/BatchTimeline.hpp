#pragma once

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string_view>

namespace AgcDriver::BatchTimeline {
inline bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_FRAME_TIMELINE");
        return value != nullptr && std::string_view(value) == "1";
    }();
    return enabled;
}

inline std::uint64_t CpuNanos() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

struct Origin {
    std::uint64_t submission = 0;
    std::uint32_t queue = 0;
    friend bool operator==(const Origin&, const Origin&) = default;
};

inline thread_local Origin currentOrigin;

class Scope {
public:
    explicit Scope(Origin origin) : active(Enabled()) {
        if (active) {
            previous = currentOrigin;
            currentOrigin = origin;
        }
    }
    ~Scope() {
        if (active) currentOrigin = previous;
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
private:
    Origin previous;
    bool active;
};
}
