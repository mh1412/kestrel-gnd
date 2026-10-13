#include "clock.hpp"
#include <ctime>
namespace kestrel_ui {
uint64_t monotonic_us() {
    timespec ts{}; clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}
}
