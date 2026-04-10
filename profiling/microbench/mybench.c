#define _GNU_SOURCE
#include <errno.h>
#include <getopt.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>
#include <linux/futex.h>
#include <time.h>
#include <unistd.h>

#ifndef FUTEX_PRIVATE_FLAG
#define FUTEX_PRIVATE_FLAG 128
#endif

#ifndef FUTEX_WAIT_PRIVATE
#define FUTEX_WAIT_PRIVATE (FUTEX_WAIT | FUTEX_PRIVATE_FLAG)
#endif

#ifndef FUTEX_WAKE_PRIVATE
#define FUTEX_WAKE_PRIVATE (FUTEX_WAKE | FUTEX_PRIVATE_FLAG)
#endif

typedef enum {
    MODE_CPU = 0,
    MODE_YIELD,
    MODE_SLEEPWAKE,
    MODE_FUTEX_PINGPONG,
} bench_mode_t;

typedef struct {
    int id;
    pthread_t tid;
    uint64_t ops;
    uint64_t wakeups;
    uint64_t loops;
} worker_t;

typedef struct {
    int threads;
    int duration_sec;
    int sleep_us;
    int futex_wake_batch;
    int pin_cpu_base;
    bool pin;
    bench_mode_t mode;
} config_t;

static config_t g_cfg = {
    .threads = 4,
    .duration_sec = 10,
    .sleep_us = 1000,
    .futex_wake_batch = 1,
    .pin_cpu_base = 0,
    .pin = false,
    .mode = MODE_CPU,
};

static worker_t *g_workers;
static atomic_int g_stop = 0;
static atomic_long g_barrier = 0;
static atomic_int g_futex_word = 0;
static atomic_int g_turn = 0;
static atomic_ulong g_total_ops = 0;
static atomic_ulong g_total_wakeups = 0;

static inline int futex_wait_private(atomic_int *uaddr, int expected)
{
    return syscall(SYS_futex, (int *)uaddr, FUTEX_WAIT_PRIVATE, expected, NULL, NULL, 0);
}

static inline int futex_wake_private(atomic_int *uaddr, int nr)
{
    return syscall(SYS_futex, (int *)uaddr, FUTEX_WAKE_PRIVATE, nr, NULL, NULL, 0);
}

static void die(const char *msg)
{
    perror(msg);
    exit(EXIT_FAILURE);
}

static const char *mode_name(bench_mode_t mode)
{
    switch (mode) {
    case MODE_CPU: return "cpu";
    case MODE_YIELD: return "yield";
    case MODE_SLEEPWAKE: return "sleepwake";
    case MODE_FUTEX_PINGPONG: return "futex_pingpong";
    default: return "unknown";
    }
}

static bench_mode_t parse_mode(const char *s)
{
    if (strcmp(s, "cpu") == 0) return MODE_CPU;
    if (strcmp(s, "yield") == 0) return MODE_YIELD;
    if (strcmp(s, "sleepwake") == 0) return MODE_SLEEPWAKE;
    if (strcmp(s, "futex_pingpong") == 0) return MODE_FUTEX_PINGPONG;

    fprintf(stderr, "Unknown mode: %s\n", s);
    exit(EXIT_FAILURE);
}

static void pin_to_cpu(int cpu)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0) {
        die("pthread_setaffinity_np");
    }
}

static void busy_work(unsigned long n)
{
    volatile unsigned long x = 0;
    for (unsigned long i = 0; i < n; ++i) {
        x += (i * 1103515245u + 12345u) ^ (x >> 3);
    }
    (void)x;
}

static void barrier_wait_all(int threads)
{
    atomic_fetch_add_explicit(&g_barrier, 1, memory_order_acq_rel);
    while (atomic_load_explicit(&g_barrier, memory_order_acquire) < threads) {
        sched_yield();
    }
}

static void *worker_cpu(void *arg)
{
    worker_t *w = (worker_t *)arg;
    if (g_cfg.pin) pin_to_cpu(g_cfg.pin_cpu_base + w->id);
    barrier_wait_all(g_cfg.threads);

    while (!atomic_load_explicit(&g_stop, memory_order_relaxed)) {
        busy_work(50000);
        w->ops++;
    }
    atomic_fetch_add(&g_total_ops, w->ops);
    return NULL;
}

static void *worker_yield(void *arg)
{
    worker_t *w = (worker_t *)arg;
    if (g_cfg.pin) pin_to_cpu(g_cfg.pin_cpu_base + w->id);
    barrier_wait_all(g_cfg.threads);

    while (!atomic_load_explicit(&g_stop, memory_order_relaxed)) {
        busy_work(2000);
        sched_yield();
        w->ops++;
    }
    atomic_fetch_add(&g_total_ops, w->ops);
    return NULL;
}

static void *worker_sleepwake(void *arg)
{
    worker_t *w = (worker_t *)arg;
    if (g_cfg.pin) pin_to_cpu(g_cfg.pin_cpu_base + w->id);
    barrier_wait_all(g_cfg.threads);

    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = g_cfg.sleep_us * 1000L;

    while (!atomic_load_explicit(&g_stop, memory_order_relaxed)) {
        busy_work(3000);
        nanosleep(&ts, NULL);
        w->ops++;
        w->wakeups++;
    }
    atomic_fetch_add(&g_total_ops, w->ops);
    atomic_fetch_add(&g_total_wakeups, w->wakeups);
    return NULL;
}

static void *worker_futex_pingpong(void *arg)
{
    worker_t *w = (worker_t *)arg;
    if (g_cfg.pin) pin_to_cpu(g_cfg.pin_cpu_base + w->id);
    barrier_wait_all(g_cfg.threads);

    while (!atomic_load_explicit(&g_stop, memory_order_relaxed)) {
        int expected = w->id;
        while (!atomic_load_explicit(&g_stop, memory_order_relaxed) &&
               atomic_load_explicit(&g_turn, memory_order_acquire) != expected) {
            int fw = atomic_load_explicit(&g_futex_word, memory_order_acquire);
            futex_wait_private(&g_futex_word, fw);
        }
        if (atomic_load_explicit(&g_stop, memory_order_relaxed))
            break;

        busy_work(2000);
        w->ops++;

        int next = (w->id + 1) % g_cfg.threads;
        atomic_store_explicit(&g_turn, next, memory_order_release);
        atomic_fetch_add_explicit(&g_futex_word, 1, memory_order_acq_rel);
        futex_wake_private(&g_futex_word, g_cfg.futex_wake_batch);
        w->wakeups++;
    }

    atomic_fetch_add(&g_total_ops, w->ops);
    atomic_fetch_add(&g_total_wakeups, w->wakeups);
    return NULL;
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options]\n"
        "  -t, --threads N          number of threads (default: 4)\n"
        "  -d, --duration SEC       run duration in seconds (default: 10)\n"
        "  -m, --mode MODE          cpu|yield|sleepwake|futex_pingpong (default: cpu)\n"
        "  -s, --sleep-us US        sleep duration for sleepwake mode (default: 1000)\n"
        "  -w, --wake-batch N       futex wake batch size (default: 1)\n"
        "  -p, --pin BASECPU        pin thread i to cpu BASECPU+i\n"
        "  -h, --help               show this help\n",
        prog);
}

static void parse_args(int argc, char **argv)
{
    static const struct option long_opts[] = {
        {"threads", required_argument, 0, 't'},
        {"duration", required_argument, 0, 'd'},
        {"mode", required_argument, 0, 'm'},
        {"sleep-us", required_argument, 0, 's'},
        {"wake-batch", required_argument, 0, 'w'},
        {"pin", required_argument, 0, 'p'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "t:d:m:s:w:p:h", long_opts, NULL)) != -1) {
        switch (c) {
        case 't':
            g_cfg.threads = atoi(optarg);
            break;
        case 'd':
            g_cfg.duration_sec = atoi(optarg);
            break;
        case 'm':
            g_cfg.mode = parse_mode(optarg);
            break;
        case 's':
            g_cfg.sleep_us = atoi(optarg);
            break;
        case 'w':
            g_cfg.futex_wake_batch = atoi(optarg);
            break;
        case 'p':
            g_cfg.pin = true;
            g_cfg.pin_cpu_base = atoi(optarg);
            break;
        case 'h':
        default:
            usage(argv[0]);
            exit(c == 'h' ? EXIT_SUCCESS : EXIT_FAILURE);
        }
    }

    if (g_cfg.threads <= 0 || g_cfg.duration_sec <= 0 || g_cfg.sleep_us < 0 || g_cfg.futex_wake_batch <= 0) {
        fprintf(stderr, "Invalid arguments\n");
        exit(EXIT_FAILURE);
    }
}

static void print_result(void)
{
    unsigned long total_ops = atomic_load(&g_total_ops);
    unsigned long total_wakeups = atomic_load(&g_total_wakeups);

    printf("=== my_bench result ===\n");
    printf("mode          : %s\n", mode_name(g_cfg.mode));
    printf("threads       : %d\n", g_cfg.threads);
    printf("duration_sec  : %d\n", g_cfg.duration_sec);
    printf("sleep_us      : %d\n", g_cfg.sleep_us);
    printf("wake_batch    : %d\n", g_cfg.futex_wake_batch);
    printf("pin           : %s\n", g_cfg.pin ? "yes" : "no");
    printf("total_ops     : %lu\n", total_ops);
    printf("ops/sec       : %.2f\n", (double)total_ops / g_cfg.duration_sec);
    printf("total_wakeups : %lu\n", total_wakeups);
    printf("wakeups/sec   : %.2f\n", (double)total_wakeups / g_cfg.duration_sec);

    puts("per-thread:");
    for (int i = 0; i < g_cfg.threads; ++i) {
        printf("  thread[%d]: ops=%lu wakeups=%lu loops=%lu\n",
               g_workers[i].id,
               (unsigned long)g_workers[i].ops,
               (unsigned long)g_workers[i].wakeups,
               (unsigned long)g_workers[i].loops);
    }
}

int main(int argc, char **argv)
{
    parse_args(argc, argv);

    g_workers = calloc((size_t)g_cfg.threads, sizeof(*g_workers));
    if (!g_workers) die("calloc");

    void *(*fn)(void *) = NULL;
    switch (g_cfg.mode) {
    case MODE_CPU:
        fn = worker_cpu;
        break;
    case MODE_YIELD:
        fn = worker_yield;
        break;
    case MODE_SLEEPWAKE:
        fn = worker_sleepwake;
        break;
    case MODE_FUTEX_PINGPONG:
        fn = worker_futex_pingpong;
        break;
    default:
        fprintf(stderr, "Invalid mode\n");
        return EXIT_FAILURE;
    }

    for (int i = 0; i < g_cfg.threads; ++i) {
        g_workers[i].id = i;
        if (pthread_create(&g_workers[i].tid, NULL, fn, &g_workers[i]) != 0) {
            die("pthread_create");
        }
    }

    sleep(g_cfg.duration_sec);
    atomic_store(&g_stop, 1);
    atomic_fetch_add(&g_futex_word, 1);
    futex_wake_private(&g_futex_word, g_cfg.threads);

    for (int i = 0; i < g_cfg.threads; ++i) {
        pthread_join(g_workers[i].tid, NULL);
    }

    print_result();
    free(g_workers);
    return 0;
}
