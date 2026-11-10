/*
 * Functional and concurrency tests for /dev/mychar.
 * Build: gcc -O2 -Wall -Wextra -static -pthread -o test_mychar test_mychar.c
 * Usage: test_mychar [stress_seconds]   (default 20, 0 skips the stress test)
 *
 * Every test starts from a known state, so the binary can be run repeatedly
 * against a device that has already been used.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "mychar.h"

#define DEV_PATH "/dev/mychar"
#define BASE_SIZE 4096u
#define STRESS_THREADS 4

static int checks, failures;

#define CHECK(c) do {							\
	checks++;							\
	if (!(c)) {							\
		failures++;						\
		printf("FAIL %s:%d: %s (errno=%d)\n", __func__, __LINE__, #c, errno); \
	}								\
} while (0)

#define CHECK_ERR(call, e) CHECK((call) == -1 && errno == (e))

/*
 * Raw syscalls for the deliberately invalid calls below: glibc fortification
 * would otherwise reject bad pointers and oversized counts at build time.
 */
static long raw_read(int fd, void *p, size_t n)
{
	return syscall(SYS_read, fd, p, n);
}

static long raw_write(int fd, const void *p, size_t n)
{
	return syscall(SYS_write, fd, p, n);
}

static int resize(int fd, uint64_t size)
{
	return ioctl(fd, MYCHAR_IOC_RESIZE, &size);
}

static int xfer(int fd, unsigned long cmd, uint64_t off, uint64_t len, void *p)
{
	struct mychar_xfer x = { .offset = off, .len = len,
				 .user_ptr = (uintptr_t)p };

	return ioctl(fd, cmd, &x);
}

static struct mychar_stat get_stat(int fd)
{
	struct mychar_stat st;

	memset(&st, 0xff, sizeof(st));
	CHECK(ioctl(fd, MYCHAR_IOC_STAT, &st) == 0);
	return st;
}

static void reset_state(int fd)
{
	CHECK(resize(fd, BASE_SIZE) == 0);
	CHECK(ioctl(fd, MYCHAR_IOC_RESET) == 0);
	CHECK(lseek(fd, 0, SEEK_SET) == 0);
}

static void test_file_position(int fd)
{
	char buf[16];

	reset_state(fd);
	CHECK(write(fd, "hello", 5) == 5);
	CHECK(lseek(fd, 0, SEEK_CUR) == 5);
	CHECK(lseek(fd, 0, SEEK_SET) == 0);
	CHECK(read(fd, buf, 5) == 5 && memcmp(buf, "hello", 5) == 0);
}

static void test_end_of_buffer(int fd)
{
	char buf[16] = "0123456789abcde";

	reset_state(fd);
	CHECK(lseek(fd, BASE_SIZE - 6, SEEK_SET) == BASE_SIZE - 6);
	CHECK(write(fd, buf, 16) == 6);		/* clamped to the end */
	CHECK(lseek(fd, BASE_SIZE - 6, SEEK_SET) == BASE_SIZE - 6);
	CHECK(read(fd, buf, 16) == 6);
	CHECK(read(fd, buf, 16) == 0);		/* at the end: EOF */
	CHECK_ERR(write(fd, buf, 1), ENOSPC);
	CHECK(lseek(fd, 100000, SEEK_SET) == 100000);
	CHECK(read(fd, buf, 16) == 0);		/* past the end */
	CHECK_ERR(write(fd, buf, 1), ENOSPC);
	CHECK(lseek(fd, 0, SEEK_SET) == 0);
	CHECK(read(fd, buf, 0) == 0);
	CHECK(write(fd, buf, 0) == 0);
}

/* Only the bytes that fit may be touched, however large the count is. */
static void test_oversized_count(int fd)
{
	static char page[BASE_SIZE];

	reset_state(fd);
	CHECK(raw_write(fd, page, 1u << 21) == (long)BASE_SIZE);
	CHECK(lseek(fd, 0, SEEK_SET) == 0);
	CHECK(raw_read(fd, page, 1u << 21) == (long)BASE_SIZE);
}

static void test_xfer_roundtrip(int fd)
{
	char in[8] = "world", out[8] = { 0 };

	reset_state(fd);
	CHECK(xfer(fd, MYCHAR_IOC_WRITE_AT, 100, 5, in) == 0);
	CHECK(xfer(fd, MYCHAR_IOC_READ_AT, 100, 5, out) == 0);
	CHECK(memcmp(out, "world", 5) == 0);
	CHECK(xfer(fd, MYCHAR_IOC_READ_AT, BASE_SIZE - 1, 1, out) == 0);
}

static void test_xfer_bounds(int fd)
{
	char buf[8] = { 0 };
	static const unsigned long cmds[] = {
		MYCHAR_IOC_READ_AT, MYCHAR_IOC_WRITE_AT,
	};

	reset_state(fd);
	for (unsigned int i = 0; i < 2; i++) {
		unsigned long c = cmds[i];

		CHECK(xfer(fd, c, BASE_SIZE, 0, buf) == 0);
		CHECK_ERR(xfer(fd, c, BASE_SIZE + 1, 0, buf), EINVAL);
		CHECK_ERR(xfer(fd, c, BASE_SIZE, 1, buf), EINVAL);
		CHECK_ERR(xfer(fd, c, BASE_SIZE - 1, 2, buf), EINVAL);
		/* offset + len wraps around 2^64 */
		CHECK_ERR(xfer(fd, c, UINT64_MAX, 2, buf), EINVAL);
		CHECK_ERR(xfer(fd, c, 1, UINT64_MAX, buf), EINVAL);
		CHECK_ERR(xfer(fd, c, 0, MYCHAR_MAX_SIZE + 1ull, buf), EINVAL);
	}
}

static void test_resize(int fd)
{
	char buf[16] = "abcdefgh", out[16];
	struct mychar_stat st;

	reset_state(fd);
	CHECK(xfer(fd, MYCHAR_IOC_WRITE_AT, 0, 8, buf) == 0);

	/* grow keeps the data and zeroes the new area */
	CHECK(resize(fd, 8192) == 0);
	CHECK(xfer(fd, MYCHAR_IOC_READ_AT, 0, 8, out) == 0);
	CHECK(memcmp(out, "abcdefgh", 8) == 0);
	CHECK(xfer(fd, MYCHAR_IOC_READ_AT, 8191, 1, out) == 0 && out[0] == 0);

	/* shrink keeps the prefix and enforces the new bound */
	CHECK(resize(fd, 4) == 0);
	CHECK(xfer(fd, MYCHAR_IOC_READ_AT, 0, 4, out) == 0);
	CHECK(memcmp(out, "abcd", 4) == 0);
	CHECK_ERR(xfer(fd, MYCHAR_IOC_READ_AT, 4, 1, out), EINVAL);

	/* limits */
	CHECK(resize(fd, MYCHAR_MAX_SIZE) == 0);
	CHECK_ERR(resize(fd, MYCHAR_MAX_SIZE + 1ull), EINVAL);
	CHECK_ERR(resize(fd, UINT64_MAX), EINVAL);
	CHECK_ERR(resize(fd, 1ull << 32), EINVAL);
	st = get_stat(fd);
	CHECK(st.size == MYCHAR_MAX_SIZE);
}

static void test_resize_to_zero(int fd)
{
	char buf[8] = { 0 };

	reset_state(fd);
	CHECK(resize(fd, 0) == 0);
	CHECK(get_stat(fd).size == 0);
	CHECK(lseek(fd, 0, SEEK_SET) == 0);
	CHECK(read(fd, buf, 8) == 0);
	CHECK_ERR(write(fd, buf, 8), ENOSPC);
	CHECK(xfer(fd, MYCHAR_IOC_READ_AT, 0, 0, buf) == 0);
	CHECK_ERR(xfer(fd, MYCHAR_IOC_READ_AT, 0, 1, buf), EINVAL);
	CHECK(ioctl(fd, MYCHAR_IOC_RESET) == 0);
	CHECK(resize(fd, 0) == 0);		/* free of nothing */
	CHECK(resize(fd, 16) == 0);		/* usable again */
	CHECK(xfer(fd, MYCHAR_IOC_READ_AT, 0, 16, (char[16]){ 0 }) == 0);
}

static void test_stat_counters(int fd)
{
	char buf[16] = { 0 };
	struct mychar_stat before, st;

	reset_state(fd);
	before = get_stat(fd);
	CHECK(before.bytes_read == 0 && before.bytes_written == 0);
	CHECK(before.size == BASE_SIZE && before.pad == 0);

	CHECK(write(fd, buf, 5) == 5);
	CHECK(xfer(fd, MYCHAR_IOC_WRITE_AT, 0, 7, buf) == 0);
	CHECK(lseek(fd, 0, SEEK_SET) == 0);
	CHECK(read(fd, buf, 3) == 3);
	st = get_stat(fd);
	CHECK(st.bytes_written == 12 && st.bytes_read == 3);

	/* a resize to the current size is a no-op */
	CHECK(resize(fd, BASE_SIZE) == 0);
	CHECK(get_stat(fd).resize_count == before.resize_count);
	CHECK(resize(fd, BASE_SIZE * 2) == 0);
	CHECK(get_stat(fd).resize_count == before.resize_count + 1);

	/* a rejected resize does not count */
	CHECK_ERR(resize(fd, MYCHAR_MAX_SIZE + 1ull), EINVAL);
	CHECK(get_stat(fd).resize_count == before.resize_count + 1);

	/* RESET clears the byte counters only */
	CHECK(ioctl(fd, MYCHAR_IOC_RESET) == 0);
	st = get_stat(fd);
	CHECK(st.bytes_written == 0 && st.bytes_read == 0);
	CHECK(st.resize_count == before.resize_count + 1);
}

static void test_bad_user_pointers(int fd)
{
	void *bad = (void *)8;
	struct mychar_stat st;

	reset_state(fd);
	CHECK_ERR(raw_read(fd, bad, 8), EFAULT);
	CHECK(lseek(fd, 0, SEEK_CUR) == 0);	/* position not advanced */
	CHECK_ERR(raw_write(fd, bad, 8), EFAULT);
	CHECK(lseek(fd, 0, SEEK_CUR) == 0);
	CHECK_ERR(xfer(fd, MYCHAR_IOC_READ_AT, 0, 8, bad), EFAULT);
	CHECK_ERR(xfer(fd, MYCHAR_IOC_WRITE_AT, 0, 8, bad), EFAULT);
	CHECK_ERR(ioctl(fd, MYCHAR_IOC_RESIZE, bad), EFAULT);
	CHECK_ERR(ioctl(fd, MYCHAR_IOC_READ_AT, bad), EFAULT);
	CHECK_ERR(ioctl(fd, MYCHAR_IOC_WRITE_AT, bad), EFAULT);
	CHECK_ERR(ioctl(fd, MYCHAR_IOC_STAT, bad), EFAULT);

	/* failed transfers must not be counted */
	st = get_stat(fd);
	CHECK(st.bytes_read == 0 && st.bytes_written == 0);
}

static void test_unknown_ioctl(int fd)
{
	CHECK_ERR(ioctl(fd, _IO(MYCHAR_IOC_MAGIC, 99)), ENOTTY);
	CHECK_ERR(ioctl(fd, _IO('Z', 1)), ENOTTY);
}

/* Two descriptors have independent positions over the same buffer. */
static void test_two_descriptors(int fd)
{
	int fd2 = open(DEV_PATH, O_RDWR);
	char out[8] = { 0 };

	CHECK(fd2 >= 0);
	reset_state(fd);
	CHECK(write(fd, "ab", 2) == 2);
	CHECK(lseek(fd2, 0, SEEK_CUR) == 0);
	CHECK(read(fd2, out, 2) == 2 && memcmp(out, "ab", 2) == 0);
	close(fd2);
}

static atomic_ullong stress_ops;
static atomic_int stress_bad;
static atomic_int stress_stop;

static void *stress_worker(void *arg)
{
	unsigned int seed = (unsigned int)(uintptr_t)arg;
	int fd = open(DEV_PATH, O_RDWR);
	char *buf = malloc(16384);
	unsigned long long ops = 0;

	if (fd < 0 || !buf) {
		atomic_fetch_add(&stress_bad, 1);
		return NULL;
	}

	while (!atomic_load(&stress_stop)) {
		uint64_t off = (uint64_t)rand_r(&seed) % 16384;
		uint64_t len = (uint64_t)rand_r(&seed) % 16384;
		int op = rand_r(&seed) % 8;
		long r = 0;

		switch (op) {
		case 0:
			/* frees and reallocates under concurrent transfers */
			r = resize(fd, rand_r(&seed) % 4 ? len : 0);
			break;
		case 1:
			r = xfer(fd, MYCHAR_IOC_READ_AT, off, len, buf);
			break;
		case 2:
			r = xfer(fd, MYCHAR_IOC_WRITE_AT, off, len, buf);
			break;
		case 3:
			r = pread(fd, buf, len, (off_t)off);
			break;
		case 4:
			r = pwrite(fd, buf, len, (off_t)off);
			break;
		case 5:
			r = ioctl(fd, MYCHAR_IOC_RESET);
			break;
		case 6: {
			struct mychar_stat st;

			r = ioctl(fd, MYCHAR_IOC_STAT, &st);
			if (r == 0 && st.size > MYCHAR_MAX_SIZE)
				atomic_fetch_add(&stress_bad, 1);
			break;
		}
		default:
			r = lseek(fd, (off_t)off, SEEK_SET);
			break;
		}
		if (r < 0 && errno != EINVAL && errno != ENOSPC) {
			printf("stress: op %d failed with errno %d\n", op, errno);
			atomic_fetch_add(&stress_bad, 1);
		}
		ops++;
	}
	atomic_fetch_add(&stress_ops, ops);
	free(buf);
	close(fd);
	return NULL;
}

static void test_stress(unsigned int seconds)
{
	pthread_t t[STRESS_THREADS];

	for (uintptr_t i = 0; i < STRESS_THREADS; i++)
		CHECK(pthread_create(&t[i], NULL, stress_worker,
				     (void *)(i * 2654435761u + 1)) == 0);
	sleep(seconds);
	atomic_store(&stress_stop, 1);
	for (int i = 0; i < STRESS_THREADS; i++)
		pthread_join(t[i], NULL);

	CHECK(atomic_load(&stress_bad) == 0);
	printf("stress: threads=%d seconds=%u ops=%llu unexpected=%d\n",
	       STRESS_THREADS, seconds, atomic_load(&stress_ops),
	       atomic_load(&stress_bad));
}

int main(int argc, char **argv)
{
	unsigned int stress_seconds = argc > 1 ? (unsigned int)atoi(argv[1]) : 20;
	int fd = open(DEV_PATH, O_RDWR);

	if (fd < 0) {
		perror("open " DEV_PATH);
		return 2;
	}

	test_file_position(fd);
	test_end_of_buffer(fd);
	test_oversized_count(fd);
	test_xfer_roundtrip(fd);
	test_xfer_bounds(fd);
	test_resize(fd);
	test_resize_to_zero(fd);
	test_stat_counters(fd);
	test_bad_user_pointers(fd);
	test_unknown_ioctl(fd);
	test_two_descriptors(fd);
	if (stress_seconds)
		test_stress(stress_seconds);
	reset_state(fd);
	close(fd);

	printf("%s: %d checks, %d failures\n",
	       failures ? "FAILED" : "ALL TESTS PASSED", checks, failures);
	return failures ? 1 : 0;
}
