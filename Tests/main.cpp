#include "TestFramework.h"
#include "Core/DeviceInputStream.h"

int main()
{
    // The drift loop places each pull within the device's block by reading a
    // clock. The tests drive push and pull back to back, thousands of times
    // faster than real time, so the wall clock would make every run differ.
    // A clock that reads zero disables that placement: the loop runs on the
    // raw ring level, as it did before, and a test that wants the placement
    // installs a simulated clock of its own.
    mma::DeviceInputStream::setClockForTesting ([]() -> int64_t { return 0; });
    return runAllTests();
}
