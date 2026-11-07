#pragma once

#include <string>

namespace AgcDriver {

// Test-only: close the calling thread's open frame, report it, and return the last report line.
void StreamReuseFlushForTest();
std::string StreamReuseReportForTest();

}
