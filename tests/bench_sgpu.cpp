// SPDX-License-Identifier: GPL-2.0
// Submit-to-completion latency and batching throughput of the sgpu stack.
// The device is emulated, so these numbers describe driver and emulation
// overhead on this setup and do not predict real hardware.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

#include "sgpu.hpp"

using namespace sgpu;
using Clock = std::chrono::steady_clock;

static void latency(const Device &d, int rounds) {
    Buffer b = d.alloc(4096);
    std::vector<double> us;
    us.reserve(static_cast<std::size_t>(rounds));
    for (int i = 0; i < rounds; i++) {
        auto t0 = Clock::now();
        if (d.submit(CommandList().fill(b, 0, 64, 1)).wait() != 0) std::puts("job failed");
        us.push_back(std::chrono::duration<double, std::micro>(Clock::now() - t0).count());
    }
    std::sort(us.begin(), us.end());
    auto pct = [&](double p) { return us[static_cast<std::size_t>(p * (us.size() - 1))]; };
    std::printf("latency_us rounds=%d p50=%.0f p90=%.0f p99=%.0f max=%.0f\n", rounds, pct(0.5), pct(0.9),
                pct(0.99), us.back());
}

static void throughput(const Device &d, int batch, int seconds) {
    Buffer b = d.alloc(4096);
    CommandList list;
    for (int i = 0; i < batch; i++) list.fill(b, 0, 64, 2);
    unsigned long long cmds = 0;
    auto end = Clock::now() + std::chrono::seconds(seconds);
    while (Clock::now() < end) {
        if (d.submit(list).wait() != 0) std::puts("job failed");
        cmds += static_cast<unsigned long long>(batch);
    }
    std::printf("throughput batch=%d cmds_per_s=%.0f\n", batch, static_cast<double>(cmds) / seconds);
}

int main() {
    Device d;
    latency(d, 2000);
    for (int batch : {1, 8, 64}) throughput(d, batch, 5);
    return 0;
}
