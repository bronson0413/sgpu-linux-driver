// SPDX-License-Identifier: GPL-2.0
// Functional, isolation, recovery and concurrency tests for the sgpu driver.
// Usage:
//   test_sgpu [stress_seconds]     run the suite (default 20, 0 skips stress)
//   test_sgpu --unplug <pci-bdf>   hot-unplug the device with a context open
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "sgpu.hpp"

using namespace sgpu;
using Bytes = std::vector<std::uint8_t>;

static int g_checks, g_failures;

#define CHECK(c)                                                                     \
    do {                                                                             \
        ++g_checks;                                                                  \
        if (!(c)) {                                                                  \
            ++g_failures;                                                            \
            std::printf("FAIL %s:%d: %s (errno=%d)\n", __func__, __LINE__, #c, errno); \
        }                                                                            \
    } while (0)

#define CHECK_ERR(call, e) CHECK((call) == -1 && errno == (e))

static int raw_create(int fd, std::uint64_t size, std::uint32_t pad = 0) {
    sgpu_bo_create a{};
    a.size = size;
    a.pad = pad;
    return ioctl(fd, SGPU_IOC_BO_CREATE, &a);
}

static int raw_xfer(int fd, unsigned long cmd, std::uint32_t handle, std::uint64_t off, std::uint64_t len,
                    void *p, std::uint32_t pad = 0) {
    sgpu_bo_xfer a{};
    a.handle = handle;
    a.offset = off;
    a.len = len;
    a.user_ptr = reinterpret_cast<std::uintptr_t>(p);
    a.pad = pad;
    return ioctl(fd, cmd, &a);
}

static int raw_submit(int fd, const sgpu_cmd *cmds, std::uint32_t count, std::uint32_t pad = 0) {
    sgpu_submit a{};
    a.cmds_ptr = reinterpret_cast<std::uintptr_t>(cmds);
    a.count = count;
    a.pad = pad;
    return ioctl(fd, SGPU_IOC_SUBMIT, &a);
}

static int submit_one(int fd, std::uint32_t op, std::uint64_t src, std::uint64_t dst, std::uint32_t len) {
    sgpu_cmd c{};
    c.opcode = op;
    c.src = src;
    c.dst = dst;
    c.len = len;
    return raw_submit(fd, &c, 1);
}

static int raw_wait(int fd, std::uint64_t fence, std::uint32_t ms) {
    sgpu_wait a{};
    a.fence = fence;
    a.timeout_ms = ms;
    return ioctl(fd, SGPU_IOC_WAIT, &a);
}

static bool all_equal(const Bytes &b, std::uint8_t v) {
    return std::all_of(b.begin(), b.end(), [v](std::uint8_t x) { return x == v; });
}

static void test_info() {
    Device d;
    auto i = d.info();
    CHECK(i.version == 1 && i.num_contexts == 16 && i.max_bo_size == SGPU_MAX_BO_SIZE);
    CHECK(i.banned == 0 && i.context_id < 16);
    CHECK_ERR(ioctl(d.fd(), SGPU_IOC_INFO, 8), EFAULT);
    CHECK_ERR(ioctl(d.fd(), _IO('Z', 1)), ENOTTY);
}

static void test_bo_limits() {
    Device d;
    CHECK_ERR(raw_create(d.fd(), 0), EINVAL);
    CHECK_ERR(raw_create(d.fd(), SGPU_MAX_BO_SIZE + 1ull), EINVAL);
    CHECK_ERR(raw_create(d.fd(), UINT64_MAX), EINVAL);
    CHECK_ERR(raw_create(d.fd(), 4096, 1), EINVAL);
    CHECK_ERR(ioctl(d.fd(), SGPU_IOC_BO_CREATE, 8), EFAULT);

    sgpu_bo_destroy gone{};
    gone.handle = 12345;
    CHECK_ERR(ioctl(d.fd(), SGPU_IOC_BO_DESTROY, &gone), ENOENT);
    {
        Buffer b = d.alloc(4096);
        sgpu_bo_destroy bad_pad{};
        bad_pad.handle = b.handle();
        bad_pad.pad = 1;
        CHECK_ERR(ioctl(d.fd(), SGPU_IOC_BO_DESTROY, &bad_pad), EINVAL);

        sgpu_bo_destroy twice{};
        twice.handle = b.handle();
        CHECK(ioctl(d.fd(), SGPU_IOC_BO_DESTROY, &twice) == 0);
        CHECK_ERR(ioctl(d.fd(), SGPU_IOC_BO_DESTROY, &twice), ENOENT);
    }

    // The address space runs out cleanly and is reusable after freeing.
    std::vector<Buffer> all;
    try {
        for (;;) all.push_back(d.alloc(SGPU_MAX_BO_SIZE));
    } catch (const std::system_error &e) {
        CHECK(e.code().value() == ENOSPC);
    }
    CHECK(all.size() == 3);
    all.clear();
    CHECK(d.alloc(SGPU_MAX_BO_SIZE).size() == SGPU_MAX_BO_SIZE);
}

static void test_guard_pages() {
    Device d;
    Buffer a = d.alloc(4096), b = d.alloc(4096);
    CHECK(b.va() >= a.va() + 2 * 4096);	// an unmapped page between buffers
}

static void test_bo_xfer() {
    Device d;
    Buffer b = d.alloc(4096);
    Bytes in{1, 2, 3, 4}, out(4);
    b.write(100, in);
    b.read(100, out);
    CHECK(out == in);

    std::uint8_t c = 0;
    int fd = d.fd();
    std::uint32_t h = b.handle();
    CHECK(raw_xfer(fd, SGPU_IOC_BO_READ, h, 4095, 1, &c) == 0);
    CHECK_ERR(raw_xfer(fd, SGPU_IOC_BO_READ, h, 4096, 1, &c), EINVAL);
    CHECK_ERR(raw_xfer(fd, SGPU_IOC_BO_READ, h, 4095, 2, &c), EINVAL);
    CHECK_ERR(raw_xfer(fd, SGPU_IOC_BO_READ, h, UINT64_MAX, 2, &c), EINVAL);
    CHECK_ERR(raw_xfer(fd, SGPU_IOC_BO_READ, h, 1, UINT64_MAX, &c), EINVAL);
    CHECK_ERR(raw_xfer(fd, SGPU_IOC_BO_READ, h, 0, 0, &c), EINVAL);
    CHECK_ERR(raw_xfer(fd, SGPU_IOC_BO_READ, h, 0, 1, &c, 1), EINVAL);
    CHECK_ERR(raw_xfer(fd, SGPU_IOC_BO_READ, 999, 0, 1, &c), ENOENT);
    CHECK_ERR(raw_xfer(fd, SGPU_IOC_BO_READ, h, 0, 1, reinterpret_cast<void *>(8)), EFAULT);
    CHECK_ERR(raw_xfer(fd, SGPU_IOC_BO_WRITE, h, 0, 1, reinterpret_cast<void *>(8)), EFAULT);
}

static void test_fill_copy() {
    Device d;
    Buffer a = d.alloc(3 * 4096), b = d.alloc(3 * 4096);

    // Spans page boundaries on both source and destination.
    Fence f = d.submit(CommandList().fill(a, 4000, 8000, 0x5a).copy(a, 4000, b, 100, 8000));
    CHECK(f.wait() == 0);

    Bytes out(8000);
    b.read(100, out);
    CHECK(all_equal(out, 0x5a));
    Bytes edge(1);
    a.read(3999, edge);
    CHECK(edge[0] == 0);
    a.read(12000, edge);
    CHECK(edge[0] == 0);
}

static void test_ordering() {
    Device d;
    Buffer b = d.alloc(4096);
    CommandList list;
    for (int i = 1; i <= SGPU_MAX_CMDS; i++) list.fill(b, 0, 16, static_cast<std::uint8_t>(i));
    CHECK(d.submit(list).wait() == 0);
    Bytes out(16);
    b.read(0, out);
    CHECK(all_equal(out, SGPU_MAX_CMDS));

    std::optional<Fence> last;
    for (int i = 0; i < 300; i++) last = d.submit(CommandList().fill(b, 0, 1, 0x11));
    CHECK(last->wait() == 0);
    CHECK(d.info().banned == 0);
}

static void test_fences() {
    Device d, other;
    Buffer b = d.alloc(4096);
    Fence f = d.submit(CommandList().fill(b, 0, 16, 1));
    CHECK(f.wait() == 0);
    CHECK(f.wait(0) == 0);	// already signalled

    CHECK_ERR(raw_wait(d.fd(), 0, 10), EINVAL);
    CHECK_ERR(raw_wait(d.fd(), f.value() + 1000, 10), EINVAL);
    CHECK_ERR(raw_wait(d.fd(), f.value(), 3600000), EINVAL);
    // A context cannot wait on a fence that belongs to another context.
    CHECK_ERR(raw_wait(other.fd(), f.value(), 10), EINVAL);
    CHECK_ERR(ioctl(d.fd(), SGPU_IOC_WAIT, 8), EFAULT);
}

static void test_validator() {
    Device d;
    Buffer b = d.alloc(4096), c = d.alloc(4096);
    int fd = d.fd();
    const std::uint64_t va = b.va();

    CHECK_ERR(submit_one(fd, 0x55, 0, va, 1), EINVAL);		// unknown opcode
    CHECK_ERR(submit_one(fd, SGPU_OP_NOP, 1, 0, 0), EINVAL);	// stray fields
    CHECK_ERR(submit_one(fd, SGPU_OP_FILL, 0, va, 0), EINVAL);	// zero length
    CHECK_ERR(submit_one(fd, SGPU_OP_FILL, 0x100, va, 1), EINVAL);	// bad pattern
    CHECK_ERR(submit_one(fd, SGPU_OP_FILL, 0, va, SGPU_MAX_BO_SIZE + 1u), EINVAL);
    CHECK_ERR(submit_one(fd, SGPU_OP_FILL, 0, va + 4096, 1), EINVAL);	// guard page
    CHECK_ERR(submit_one(fd, SGPU_OP_FILL, 0, va + 4095, 2), EINVAL);	// past the end
    CHECK_ERR(submit_one(fd, SGPU_OP_FILL, 0, 1ull << 40, 1), EINVAL);	// out of range
    CHECK_ERR(submit_one(fd, SGPU_OP_FILL, 0, UINT64_MAX, 2), EINVAL);	// wraps
    CHECK_ERR(submit_one(fd, SGPU_OP_COPY, va, c.va() + 4096, 1), EINVAL);
    CHECK_ERR(submit_one(fd, SGPU_OP_COPY, va + 4096, c.va(), 1), EINVAL);

    sgpu_cmd cmd{};
    cmd.opcode = SGPU_OP_NOP;
    CHECK_ERR(raw_submit(fd, &cmd, 0), EINVAL);
    CHECK_ERR(raw_submit(fd, &cmd, SGPU_MAX_CMDS + 1), EINVAL);
    CHECK_ERR(raw_submit(fd, &cmd, 1, 1), EINVAL);
    CHECK_ERR(raw_submit(fd, reinterpret_cast<sgpu_cmd *>(8), 1), EFAULT);
    CHECK_ERR(ioctl(fd, SGPU_IOC_SUBMIT, 8), EFAULT);

    // One bad command rejects the whole batch before anything runs.
    sgpu_cmd batch[2]{};
    batch[0].opcode = SGPU_OP_FILL;
    batch[0].src = 0x77;
    batch[0].dst = va;
    batch[0].len = 16;
    batch[1].opcode = 0x55;
    CHECK_ERR(raw_submit(fd, batch, 2), EINVAL);
    CHECK(d.submit(CommandList().nop()).wait() == 0);
    Bytes out(16);
    b.read(0, out);
    CHECK(all_equal(out, 0));
}

static void test_isolation() {
    Device a, b;
    CHECK(a.info().context_id != b.info().context_id);
    Buffer secret = a.alloc(2 * 4096);
    Bytes data(16, 0xee);
    secret.write(0, data);

    // Context b has no such mapping, so a's addresses are rejected for b.
    CHECK_ERR(submit_one(b.fd(), SGPU_OP_FILL, 0, secret.va(), 1), EINVAL);
    Buffer mine = b.alloc(4096);
    CHECK_ERR(submit_one(b.fd(), SGPU_OP_COPY, secret.va(4096), mine.va(), 1), EINVAL);

    // Handles are per-context as well.
    std::uint8_t c = 0;
    Buffer high = a.alloc(4096);
    CHECK_ERR(raw_xfer(b.fd(), SGPU_IOC_BO_READ, high.handle(), 0, 1, &c), ENOENT);
    sgpu_bo_destroy destroy{};
    destroy.handle = high.handle();
    CHECK_ERR(ioctl(b.fd(), SGPU_IOC_BO_DESTROY, &destroy), ENOENT);

    Bytes out(16);
    secret.read(0, out);
    CHECK(out == data);
}

static void test_context_limit() {
    std::vector<int> fds;
    for (;;) {
        int fd = open("/dev/sgpu", O_RDWR);
        if (fd < 0) {
            CHECK(errno == EBUSY);
            break;
        }
        fds.push_back(fd);
    }
    CHECK(fds.size() == 16);
    for (int fd : fds) close(fd);
    Device d;
    CHECK(d.info().banned == 0);
}

// The handle goes away while the job may still be running on the device.
static void test_destroy_in_flight() {
    Device d;
    for (int i = 0; i < 200; i++) {
        Buffer a = d.alloc(SGPU_MAX_BO_SIZE / 4);
        Buffer b = d.alloc(SGPU_MAX_BO_SIZE / 4);
        Fence f = d.submit(CommandList()
                               .fill(a, 0, SGPU_MAX_BO_SIZE / 4, 0x33)
                               .copy(a, 0, b, 0, SGPU_MAX_BO_SIZE / 4));
        a.release();
        b.release();
        CHECK(f.wait() == 0);
    }
    CHECK(d.info().banned == 0);
}

// A destroyed buffer's address must be unusable at once, even while a queued job
// keeps its memory alive. Regression test for the bug in
// docs/case-study-fuzz-refcount-uaf.md. The hang keeps the job in flight.
static void test_destroy_unpublishes_va() {
    Device bad, d;
    Buffer b = d.alloc(4096);
    const auto va = b.va();
    Fence hung = bad.submit(CommandList().raw(SGPU_OP_TEST_HANG, 0, 0, 0));
    Fence queued = d.submit(CommandList().fill(b, 0, 4096, 0x55));
    b.release();
    CHECK_ERR(submit_one(d.fd(), SGPU_OP_FILL, 0x66, va, 16), EINVAL);
    CHECK(hung.wait() == -ETIMEDOUT);
    CHECK(queued.wait() == -ECANCELED);
    CHECK(d.submit(CommandList().nop()).wait() == 0);
}

// The file is closed while work is queued; the kernel must finish it safely.
static void test_close_in_flight() {
    for (int i = 0; i < 50; i++) {
        Device d;
        Buffer a = d.alloc(SGPU_MAX_BO_SIZE);
        CommandList list;
        for (int j = 0; j < SGPU_MAX_CMDS; j++) list.fill(a, 0, SGPU_MAX_BO_SIZE, 0x44);
        (void)d.submit(list);
    }
}

static void test_fault_recovery() {
    Device bad, good;
    Buffer mine = good.alloc(4096);
    auto before = good.info().resets;

    Fence f = bad.submit(CommandList().raw(SGPU_OP_TEST_FAULT, 0, 0, 0));
    CHECK(f.wait() == -EFAULT);
    CHECK(bad.info().banned == 1);
    CHECK(good.info().banned == 0);
    CHECK(good.info().resets == before + 1);

    CHECK_ERR(submit_one(bad.fd(), SGPU_OP_NOP, 0, 0, 0), EIO);
    CHECK(good.submit(CommandList().fill(mine, 0, 16, 9)).wait() == 0);
}

static void test_hang_recovery() {
    Device bad, good;
    Buffer mine = good.alloc(4096);
    auto before = good.info().resets;

    Fence hung = bad.submit(CommandList().raw(SGPU_OP_TEST_HANG, 0, 0, 0));
    Fence queued = good.submit(CommandList().fill(mine, 0, 16, 7));
    CHECK(hung.wait() == -ETIMEDOUT);
    CHECK(queued.wait() == -ECANCELED);	// innocent work is cancelled, not lost silently
    CHECK(bad.info().banned == 1);
    CHECK(good.info().banned == 0);
    CHECK(good.info().resets == before + 1);

    CHECK(good.submit(CommandList().fill(mine, 0, 16, 7)).wait() == 0);
    Bytes out(16);
    mine.read(0, out);
    CHECK(all_equal(out, 7));
}

static void test_test_ops_disabled() {
    Device d;
    CHECK_ERR(submit_one(d.fd(), SGPU_OP_TEST_HANG, 0, 0, 0), EINVAL);
    CHECK_ERR(submit_one(d.fd(), SGPU_OP_TEST_FAULT, 0, 0, 0), EINVAL);
}

static void test_stress(unsigned seconds) {
    std::atomic<bool> stop{false};
    std::atomic<unsigned long long> ops{0}, bad{0};

    auto worker = [&](unsigned id) {
        std::mt19937 rng(id * 2654435761u + 1);
        try {
            Device d;
            auto tag = static_cast<std::uint8_t>(0x10 + id);
            while (!stop.load()) {
                std::size_t size = 4096 * (1 + rng() % 8);
                auto len = static_cast<std::uint32_t>(1 + rng() % size);
                Buffer a = d.alloc(size), b = d.alloc(size);
                Fence f = d.submit(CommandList().fill(a, 0, len, tag).copy(a, 0, b, 0, len));
                if (rng() % 4 == 0) {
                    a.release();	// destroyed while possibly in flight
                    if (f.wait() != 0) bad.fetch_add(1);
                } else {
                    if (f.wait() != 0) bad.fetch_add(1);
                    Bytes out(len);
                    b.read(0, out);	// any cross-context leak shows up here
                    if (!all_equal(out, tag)) bad.fetch_add(1);
                }
                ops.fetch_add(1);
            }
        } catch (const std::exception &e) {
            std::printf("stress worker %u: %s\n", id, e.what());
            bad.fetch_add(1);
        }
    };

    std::vector<std::thread> threads;
    for (unsigned i = 0; i < 4; i++) threads.emplace_back(worker, i);
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    stop.store(true);
    for (auto &t : threads) t.join();

    CHECK(bad.load() == 0);
    std::printf("stress: threads=4 seconds=%u jobs=%llu unexpected=%llu\n", seconds, ops.load(), bad.load());
}

static const char *const kStatusPath = "/sys/kernel/debug/sgpu/status";

static std::string read_status() {
    // The file can vanish or return EIO while the device is being removed.
    try {
        std::ifstream in(kStatusPath);
        return std::string(std::istreambuf_iterator<char>(in), {});
    } catch (const std::ios_base::failure &) {
        return {};
    }
}

// Status reads race against contexts being created, used and closed.
static void test_status_concurrent() {
    CHECK(read_status().find("completed:") != std::string::npos);

    std::atomic<bool> stop{false};
    std::atomic<unsigned long long> reads{0};
    std::vector<std::thread> readers;
    for (int i = 0; i < 2; i++)
        readers.emplace_back([&] {
            while (!stop.load()) {
                (void)read_status();
                reads.fetch_add(1);
            }
        });

    auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    unsigned long long cycles = 0;
    while (std::chrono::steady_clock::now() < end) {
        Device d;
        Buffer b = d.alloc(4096);
        (void)d.submit(CommandList().fill(b, 0, 16, 1));
        ++cycles;	// closes with the job possibly still queued
    }
    stop.store(true);
    for (auto &t : readers) t.join();
    CHECK(reads.load() > 0 && cycles > 0);
    std::printf("status: reads=%llu context_cycles=%llu\n", reads.load(), cycles);
}

static int run_unplug(const std::string &bdf) {
    std::atomic<bool> stop_reader{false};
    std::thread reader([&] {
        while (!stop_reader.load()) (void)read_status();
    });
    Device d;
    Buffer b = d.alloc(SGPU_MAX_BO_SIZE);
    CommandList list;
    for (int i = 0; i < SGPU_MAX_CMDS; i++) list.fill(b, 0, SGPU_MAX_BO_SIZE, 0x66);
    Fence f = d.submit(list);

    {
        std::ofstream remove("/sys/bus/pci/devices/" + bdf + "/remove");
        remove << "1";
    }
    // The context stays open; every later call must fail cleanly.
    int status = f.wait(30000);
    CHECK(status == 0 || status == -ENODEV);
    CHECK_ERR(raw_create(d.fd(), 4096), ENODEV);
    CHECK_ERR(submit_one(d.fd(), SGPU_OP_NOP, 0, 0, 0), ENODEV);
    std::uint8_t c = 0;
    CHECK_ERR(raw_xfer(d.fd(), SGPU_IOC_BO_READ, b.handle(), 0, 1, &c), ENODEV);
    CHECK(d.info().banned == 0);
    stop_reader.store(true);
    reader.join();
    std::printf("unplug: job_status=%d checks=%d failures=%d\n", status, g_checks, g_failures);
    return g_failures ? 1 : 0;
}

int main(int argc, char **argv) {
    try {
        if (argc == 3 && std::strcmp(argv[1], "--unplug") == 0) return run_unplug(argv[2]);
        unsigned stress = argc > 1 ? static_cast<unsigned>(std::atoi(argv[1])) : 20;

        bool test_ops = Device().info().test_ops != 0;
        test_info();
        test_bo_limits();
        test_guard_pages();
        test_bo_xfer();
        test_fill_copy();
        test_ordering();
        test_fences();
        test_validator();
        test_isolation();
        test_context_limit();
        test_destroy_in_flight();
        test_close_in_flight();
        if (test_ops) {
            test_fault_recovery();
            test_hang_recovery();
            test_destroy_unpublishes_va();
        } else {
            test_test_ops_disabled();
        }
        if (!std::getenv("SGPU_SKIP_STATUS") && access(kStatusPath, R_OK) == 0) test_status_concurrent();
        if (stress) test_stress(stress);
    } catch (const std::exception &e) {
        std::printf("FAILED: uncaught exception: %s\n", e.what());
        return 2;
    }
    std::printf("%s: %d checks, %d failures\n", g_failures ? "FAILED" : "ALL TESTS PASSED", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
