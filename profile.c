#include <stdio.h>
#include <stdlib.h>
#include "monitor.h"
#include "trace_helpers.h"
#include "tep.h"

struct monitor profile;
struct monitor_ctx {
    int nr_cpus;
    uint64_t *counter;
    uint64_t *cycles;
    struct {
        uint64_t start_time;
        uint64_t num;
    }*stat;
    struct ksyms *ksyms;
    int tsc_khz;
    int vendor;
    struct env *env;
} ctx;

static int monitor_ctx_init(struct env *env)
{
    ctx.nr_cpus = get_possible_cpus();
    ctx.counter = calloc(ctx.nr_cpus, sizeof(uint64_t));
    if (!ctx.counter) {
        return -1;
    }
    ctx.cycles = calloc(ctx.nr_cpus, sizeof(uint64_t));
    if (!ctx.cycles) {
        free(ctx.counter);
        return -1;
    }
    ctx.stat = calloc(ctx.nr_cpus, sizeof(*ctx.stat));
    if (!ctx.stat) {
        free(ctx.counter);
        free(ctx.cycles);
        return -1;
    }
    if (env->callchain) {
        ctx.ksyms = ksyms__load();
    }
    ctx.tsc_khz = get_tsc_khz();
    ctx.vendor = get_cpu_vendor();
    ctx.env = env;
    return 0;
}

static void monitor_ctx_exit(void)
{
    free(ctx.counter);
    free(ctx.cycles);
    free(ctx.stat);
    ksyms__free(ctx.ksyms);
}

static int profile_init(struct perf_evlist *evlist, struct env *env)
{
    struct perf_event_attr attr = {
        .type          = PERF_TYPE_HARDWARE,
        .config        = PERF_COUNT_HW_CPU_CYCLES,
        .size          = sizeof(struct perf_event_attr),
        .sample_period = env->freq,
        .freq          = env->freq ? 1 : 0,
        .sample_type   = PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_CPU | PERF_SAMPLE_READ |
                         (env->callchain ? PERF_SAMPLE_CALLCHAIN : 0),
        .read_format   = 0,
        .pinned        = 1,
        .disabled      = 1,
        .exclude_callchain_user = 1,
        .exclude_user  = env->exclude_user,
        .exclude_kernel = env->exclude_kernel,
        .exclude_guest = env->exclude_guest,
        .exclude_host = env->guest,
        .wakeup_events = 1, //1个事件
    };
    struct perf_evsel *evsel;

    if (env->exclude_guest && env->guest)
        return -1;
    if (env->exclude_user && env->exclude_kernel)
        return -1;

    if (monitor_ctx_init(env) < 0)
        return -1;

    if (ctx.tsc_khz > 0 && env->freq > 0) {
        attr.freq = 0;
        attr.sample_period = ctx.tsc_khz * 1000ULL / env->freq;
    }
    if (ctx.vendor == X86_VENDOR_INTEL)
        attr.config = PERF_COUNT_HW_REF_CPU_CYCLES;

    if (env->callchain)
        profile.pages *= 2;

    if (!env->interval)
        profile.read = NULL;

    evsel = perf_evsel__new(&attr);
    if (!evsel) {
        return -1;
    }
    perf_evlist__add(evlist, evsel);
    return 0;
}

static void profile_exit(struct perf_evlist *evlist)
{
    monitor_ctx_exit();
}

static void profile_read(struct perf_counts_values *count, int cpu)
{
    uint64_t cycles = 0;
    const char *str_in[] = {"host,guest", "host", "guest", "error"};
    const char *str_mode[] = {"all", "usr", "sys", "error"};
    int in, mode;

    if (count->val > ctx.cycles[cpu]) {
        cycles = count->val - ctx.cycles[cpu];
        ctx.cycles[cpu] = count->val;
    }
    if (cycles) {
        in = (ctx.env->guest << 1) | ctx.env->exclude_guest;
        mode = (ctx.env->exclude_user << 1) | ctx.env->exclude_kernel;
        print_time(stdout);
        if (ctx.tsc_khz > 0 && ctx.vendor == X86_VENDOR_INTEL)
            printf("cpu %d [%s] %.2f%% [%s] %lu cycles\n", cpu, str_in[in],
                    (float)cycles * 100 / (ctx.tsc_khz * (__u64)ctx.env->interval),
                    str_mode[mode], cycles);
        else
            printf("cpu %d [%s] [%s] %lu cycles\n", cpu, str_in[in], str_mode[mode], cycles);
    }
}

static void profile_sample(union perf_event *event)
{
    // in linux/perf_event.h
    // PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_CPU | PERF_SAMPLE_READ | PERF_SAMPLE_CALLCHAIN
    struct sample_type_data {
        struct {
            __u32    pid;
            __u32    tid;
        }    tid_entry;
        __u64   time;
        struct {
            __u32    cpu;
            __u32    reserved;
        }    cpu_entry;
        __u64 counter;
        struct {
            __u64   nr;
	        __u64   ips[0];
        } callchain;
    } *data = (void *)event->sample.array;
    __u32 size = event->header.size - sizeof(struct perf_event_header);
    uint64_t counter = 0;
    int print = 1;

    if (size != sizeof(struct sample_type_data) +
            (ctx.env->callchain ? data->callchain.nr * sizeof(__u64) : -sizeof(__u64))) {
        fprintf(stderr, "size(%u) != sizeof sample_type_data\n", size);
    }

    if (data->counter > ctx.counter[data->cpu_entry.cpu]) {
        counter = data->counter - ctx.counter[data->cpu_entry.cpu];
        ctx.counter[data->cpu_entry.cpu] = data->counter;
    }

    if (ctx.env->greater_than) {
        __u32 cpu = data->cpu_entry.cpu;
        uint64_t time = ctx.stat[cpu].start_time;
        ctx.stat[cpu].num ++;
        if (data->time - time >= NSEC_PER_SEC) {
            print = 0;
            ctx.stat[cpu].start_time = data->time;
            ctx.stat[cpu].num = 1;
        } else {
            int x = (ctx.env->freq * ctx.env->greater_than + 99) / 100;
            if (ctx.stat[cpu].num < x)
                print = 0;
        }
    }

    if (print) {
        print_time(stdout);
        tep__update_comm(NULL, data->tid_entry.tid);
        printf("%16s %6u [%03d] %llu.%06llu: %lu cpu-cycles\n", tep__pid_to_comm(data->tid_entry.tid), data->tid_entry.tid,
                        data->cpu_entry.cpu, data->time / NSEC_PER_SEC, (data->time % NSEC_PER_SEC)/1000, counter);
        if (ctx.env->callchain && ctx.ksyms) {
            __u64 i;
            for (i = 0; i < data->callchain.nr; i++) {
                __u64 ip = data->callchain.ips[i];
                const struct ksym *ksym = ksyms__map_addr(ctx.ksyms, ip);
                printf("    %016llx %s+0x%llx\n", ip, ksym ? ksym->name : "Unknown", ip - ksym->addr);
            }
        }
    }
}

struct monitor profile = {
    .name = "profile",
    .pages = 2,
    .init = profile_init,
    .deinit = profile_exit,
    .read   = profile_read,
    .sample = profile_sample,
};
MONITOR_REGISTER(profile)

