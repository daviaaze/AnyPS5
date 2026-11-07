#pragma once

#include <cstdint>
#include <span>

namespace AgcDriver {

// APS5_STREAM_REUSE_STATS=1: per queue worker, how much of each frame's PM4 stream repeats the
// previous frame's (by submission hash), and how many draws/dispatches needed no capture or
// materialization. Every 10 s one "[stream-reuse]" line per queue goes to stderr. Diagnostic only.
bool StreamReuseEnabled();
void StreamReuseBeginSubmission(std::uint32_t queue);
void StreamReusePacket(std::span<const std::uint32_t> packet);
// One draw or dispatch packet executed by the current submission.
void StreamReuseCommand(bool draw);
// A resource capture/materialization ran. On a queue worker it counts against the submission being
// executed; elsewhere (the queue-0 draw finisher) against the next queue-0 submission to end.
// Counted per stage, so reused commands are a lower bound.
void StreamReuseNoteTranslation();
void StreamReuseEndSubmission(double hostMs);
void StreamReusePresent();

}
