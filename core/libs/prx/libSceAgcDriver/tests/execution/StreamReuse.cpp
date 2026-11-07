#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics/StreamReuse.hpp"
#include "StreamReuseTest.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

using namespace AgcDriver;

// One frame of three submissions; `order` permutes them and `changed` flips one dword of the
// submission with that index (-1: none). Submission 1 needs a capture, the others do not.
void Frame(std::array<int, 3> order, int changed) {
    for (const int index : order) {
        StreamReuseBeginSubmission(0x50);
        std::array<std::uint32_t, 3> packet{0xc0001500u, 0x100u + static_cast<std::uint32_t>(index), 0xabcu};
        if (index == changed) packet[2] ^= 1u;
        StreamReusePacket(packet);
        StreamReuseCommand(false);
        if (index == 1) StreamReuseNoteTranslation();
        StreamReuseEndSubmission(1.0 + index);
    }
    StreamReusePresent();
}

bool Expect(const char* what, std::initializer_list<const char*> fields) {
    StreamReuseFlushForTest();
    const auto report = StreamReuseReportForTest();
    for (const char* field : fields) {
        if (report.find(field) != std::string::npos) continue;
        std::fprintf(stderr, "%s: expected %s in %s", what, field, report.c_str());
        return false;
    }
    std::printf("%s: %s", what, report.c_str());
    return true;
}

}

int main() {
    setenv("APS5_STREAM_REUSE_STATS", "1", 1);
    if (!StreamReuseEnabled()) {
        std::fprintf(stderr, "APS5_STREAM_REUSE_STATS did not enable the statistics\n");
        return 1;
    }
    bool ok = true;
    Frame({0, 1, 2}, -1);
    Frame({0, 1, 2}, -1);
    ok &= Expect("identical", {"presents=1 ", "submissions=3 ", "ordinal_match=100.0%", "anywhere_match=100.0%",
        "time_ordinal=100.0%", "reused_translation=66.7%", "reused_in_matched=66.7%"});
    Frame({0, 1, 2}, 2);
    ok &= Expect("one dword changed", {"ordinal_match=66.7%", "anywhere_match=66.7%", "time_ordinal=50.0%"});
    Frame({0, 1, 2}, -1);
    StreamReuseFlushForTest();
    Frame({2, 1, 0}, -1);
    ok &= Expect("permuted", {"ordinal_match=33.3%", "anywhere_match=100.0%", "time_anywhere=100.0%"});
    return ok ? 0 : 1;
}
