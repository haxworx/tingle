/*
Copyright (c) 2017, Al Poole <netstar@gmail.com>
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/* Visit: http://haxlab.org */
/* Build : cc -lm (file) -o (output) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/ioctl.h>

#if defined(__OpenBSD__) || defined(__NetBSD__)
# include <sys/swap.h>
# include <sys/mount.h>
# include <sys/sensors.h>
# include <sys/audioio.h>
#endif

#if defined(__FreeBSD__) || defined(__DragonFly__)
# include <sys/soundcard.h>
# include <vm/vm_param.h>
#endif

#if defined(__OpenBSD__) || defined(__NetBSD__)
# define CPU_STATES 6
#endif

#if defined(__FreeBSD__) || defined(__DragonFly__)
# define CPU_STATES 5
#endif

/* Filer requests and results */
#define RESULTS_CPU 0x01
#define RESULTS_MEM 0x02
#define RESULTS_PWR 0x04
#define RESULTS_TMP 0x08
#define RESULTS_AUD 0x10
#define RESULTS_ALL 0x1f

/* Refine results */
#define RESULTS_CPU_CORES 0x80
#define RESULTS_MEM_MB 0x20
#define RESULTS_MEM_GB 0x40

typedef struct {
    float percent;
    unsigned long total;
    unsigned long idle;
} cpu_core_t;

typedef struct {
    unsigned long total;
    unsigned long used;
    unsigned long cached;
    unsigned long buffered;
    unsigned long shared;
    unsigned long swap_total;
    unsigned long swap_used;
} meminfo_t;

#define MAX_BATTERIES 5
typedef struct {
    int *bat_mibs[MAX_BATTERIES];
    int ac_mibs[5];
    bool have_ac;
    int battery_index;
    uint8_t percent;
    double last_full_charge;
    double current_charge;
} power_t;

typedef struct {
    bool enabled;
    uint8_t volume_left;
    uint8_t volume_right;
} mixer_t;

#define INVALID_TEMP -999
typedef struct results_t results_t;
struct results_t {
    cpu_core_t **cores;
    int cpu_count;
    meminfo_t memory;
    power_t power;
    mixer_t mixer;
    int temperature;
};

static int cpu_count(void)
{
    int cores = 0;
#if defined(__FreeBSD__) || defined(__DragonFly__) || defined(__OpenBSD__) || defined(__NetBSD__)
    size_t len;
    int mib[2] = { CTL_HW, HW_NCPU };

    len = sizeof(cores);
    if (sysctl(mib, 2, &cores, &len, NULL, 0) < 0)
        return 0;
#endif
    return cores;
}

#if defined(__FreeBSD__) || defined(__DragonFly__)
static long int
_sysctlfromname(const char *name, void *mib, int depth, size_t * len)
{
    long int result;

    if (sysctlnametomib(name, mib, len) < 0)
        return -1;

    *len = sizeof(result);
    if (sysctl(mib, depth, &result, len, NULL, 0) < 0)
        return -1;

    return result;
}
#endif

static void _memsize_bytes_to_kb(unsigned long *bytes)
{
    *bytes = (unsigned int) *bytes >> 10;
}

#define _memsize_kb_to_mb _memsize_bytes_to_kb

static void _memsize_kb_to_gb(unsigned long *bytes)
{
    *bytes = (unsigned int) *bytes >> 20;
}

static void _bsd_cpuinfo(cpu_core_t ** cores, int ncpu)
{
    size_t size;
    int diff_total, diff_idle;
    int i, j;
    double ratio, percent;
    unsigned long total, idle, used;
    cpu_core_t *core;
#if defined(__FreeBSD__) || defined(__DragonFly__)
    if (!ncpu)
        return;
    size = sizeof(unsigned long) * (CPU_STATES * ncpu);
    unsigned long cpu_times[ncpu][CPU_STATES];

    if (sysctlbyname("kern.cp_times", cpu_times, &size, NULL, 0) < 0)
        return;

    for (i = 0; i < ncpu; i++) {
        core = cores[i];
        unsigned long *cpu = cpu_times[i];

        total = 0;
        for (j = 0; j < CPU_STATES; j++)
            total += cpu[j];

        idle = cpu[4];

        diff_total = total - core->total;
        diff_idle = idle - core->idle;

        if (diff_total == 0)
            diff_total = 1;

        ratio = diff_total / 100.0;
        used = diff_total - diff_idle;

        percent = used / ratio;
        if (percent > 100)
            percent = 100;
        else if (percent < 0)
            percent = 0;

        core->percent = percent;
        core->total = total;
        core->idle = idle;
    }
#elif defined(__OpenBSD__)
    unsigned long cpu_times[CPU_STATES];
    if (!ncpu)
        return;
    if (ncpu == 1) {
        core = cores[0];
        int cpu_time_mib[] = { CTL_KERN, KERN_CPTIME };
        size = CPU_STATES * sizeof(unsigned long);
        if (sysctl(cpu_time_mib, 2, &cpu_times, &size, NULL, 0) < 0)
            return;

        total = 0;
        for (j = 0; j < CPU_STATES - 1; j++)
            total += cpu_times[j];

        idle = cpu_times[4];

        diff_total = total - core->total;
        diff_idle = idle - core->idle;
        if (diff_total == 0)
            diff_total = 1;

        ratio = diff_total / 100.0;
        used = diff_total - diff_idle;
        percent = used / ratio;
        if (percent > 100)
            percent = 100;
        else if (percent < 0)
            percent = 0;

        core->percent = percent;
        core->total = total;
        core->idle = idle;
    } else if (ncpu > 1) {
        for (i = 0; i < ncpu; i++) {
            core = cores[i];
            int cpu_time_mib[] = { CTL_KERN, KERN_CPTIME2, 0 };
            size = CPU_STATES * sizeof(unsigned long);
            cpu_time_mib[2] = i;
            if (sysctl(cpu_time_mib, 3, &cpu_times, &size, NULL, 0) < 0)
                return;

            total = 0;
            for (j = 0; j < CPU_STATES - 1; j++)
                total += cpu_times[j];

            idle = cpu_times[4];

            diff_total = total - core->total;
            if (diff_total == 0)
                diff_total = 1;

            diff_idle = idle - core->idle;

            ratio = diff_total / 100.0;
            used = diff_total - diff_idle;
            percent = used / ratio;

            if (percent > 100)
                percent = 100;
            else if (percent < 0)
                percent = 0;

            core->percent = percent;
            core->total = total;
            core->idle = idle;
        }
    }
#endif
}

static cpu_core_t **bsd_generic_cpuinfo(int *ncpu)
{
    cpu_core_t **cores;
    int i;

    *ncpu = cpu_count();

    cores = malloc((*ncpu) * sizeof(cpu_core_t *));

    for (i = 0; i < *ncpu; i++)
        cores[i] = calloc(1, sizeof(cpu_core_t));

    _bsd_cpuinfo(cores, *ncpu);
    usleep(1000000);
    _bsd_cpuinfo(cores, *ncpu);

    return (cores);
}

static void bsd_generic_meminfo(meminfo_t * memory)
{
    size_t len = 0;
    int i = 0;
    memset(memory, 0, sizeof(meminfo_t));
#if defined(__FreeBSD__) || defined(__DragonFly__)
    int total_pages = 0, free_pages = 0, inactive_pages = 0;
    long int result = 0;
    int page_size = getpagesize();
    int mib[4];

    mib[0] = CTL_HW;
    mib[1] = HW_PHYSMEM;
    len = sizeof(memory->total);
    if (sysctl(mib, 2, &memory->total, &len, NULL, 0) == -1)
        return;
    memory->total /= 1024;

    total_pages =
        _sysctlfromname("vm.stats.vm.v_page_count", mib, 4, &len);
    if (total_pages < 0)
        return;

    free_pages = _sysctlfromname("vm.stats.vm.v_free_count", mib, 4, &len);
    if (free_pages < 0)
        return;

    inactive_pages =
        _sysctlfromname("vm.stats.vm.v_inactive_count", mib, 4, &len);
    if (inactive_pages < 0)
        return;

    memory->used = (total_pages - free_pages - inactive_pages) * page_size;
    _memsize_bytes_to_kb(&memory->used);

    result = _sysctlfromname("vfs.bufspace", mib, 2, &len);
    if (result < 0)
        return;
    memory->buffered = (result);
    _memsize_bytes_to_kb(&memory->buffered);

    result = _sysctlfromname("vm.stats.vm.v_active_count", mib, 4, &len);
    if (result < 0)
        return;
    memory->cached = (result * page_size);
    _memsize_bytes_to_kb(&memory->cached);

    result = _sysctlfromname("vm.stats.vm.v_cache_count", mib, 4, &len);
    if (result < 0)
        return;
    memory->shared = (result * page_size);
    _memsize_bytes_to_kb(&memory->shared);

    result = _sysctlfromname("vm.swap_total", mib, 2, &len);
    if (result < 0)
        return;
    memory->swap_total = (result / 1024);

    struct xswdev xsw;
    /* previous mib is important for this one... */

    while (i++) {
        mib[2] = i;
        len = sizeof(xsw);
        if (sysctl(mib, 3, &xsw, &len, NULL, 0) == -1)
            break;

        memory->swap_used += xsw.xsw_used * page_size;
    }

    memory->swap_used >>= 10;

#elif defined(__OpenBSD__)
    static int mib[] = { CTL_HW, HW_PHYSMEM64 };
    static int bcstats_mib[] = { CTL_VFS, VFS_GENERIC, VFS_BCACHESTAT };
    struct bcachestats bcstats;
    static int uvmexp_mib[] = { CTL_VM, VM_UVMEXP };
    struct uvmexp uvmexp;
    int nswap, rnswap;
    struct swapent *swdev = NULL;

    len = sizeof(memory->total);
    if (sysctl(mib, 2, &memory->total, &len, NULL, 0) == -1)
        return;

    len = sizeof(uvmexp);
    if (sysctl(uvmexp_mib, 2, &uvmexp, &len, NULL, 0) == -1)
        return;

    len = sizeof(bcstats);
    if (sysctl(bcstats_mib, 3, &bcstats, &len, NULL, 0) == -1)
        return;

    /* Don't fail if there's not swap! */
    nswap = swapctl(SWAP_NSWAP, 0, 0);
    if (nswap == 0)
        goto swap_out;

    swdev = calloc(nswap, sizeof(*swdev));
    if (swdev == NULL)
        goto swap_out;

    rnswap = swapctl(SWAP_STATS, swdev, nswap);
    if (rnswap == -1)
        goto swap_out;

    for (i = 0; i < nswap; i++) {
        if (swdev[i].se_flags & SWF_ENABLE) {
            memory->swap_used += (swdev[i].se_inuse / (1024 / DEV_BSIZE));
            memory->swap_total += (swdev[i].se_nblks / (1024 / DEV_BSIZE));
        }
    }
  swap_out:
    if (swdev)
        free(swdev);

    memory->total /= 1024;

    memory->cached = (uvmexp.pagesize * bcstats.numbufpages);
    _memsize_bytes_to_kb(&memory->cached);

    memory->used = (uvmexp.active * uvmexp.pagesize);
    _memsize_bytes_to_kb(&memory->used);

    memory->buffered = (uvmexp.pagesize * (uvmexp.npages - uvmexp.free));
    _memsize_bytes_to_kb(&memory->buffered);

    memory->shared = (uvmexp.pagesize * uvmexp.wired);
    _memsize_bytes_to_kb(&memory->shared);
#endif
}

static int bsd_generic_audio_state_master(mixer_t * mixer)
{
#if defined(__OpenBSD__) || defined(__NetBSD__)
    int i, fd, devn;
    char name[64];
    mixer_devinfo_t dinfo;
    mixer_ctrl_t *values = NULL;
    mixer_devinfo_t *info = NULL;

    fd = open("/dev/mixer", O_RDONLY);
    if (fd < 0)
        return (0);

    for (devn = 0;; devn++) {
        dinfo.index = devn;
        if (ioctl(fd, AUDIO_MIXER_DEVINFO, &dinfo))
            break;
    }

    info = calloc(devn, sizeof(*info));
    if (!info)
        return (0);

    for (i = 0; i < devn; i++) {
        info[i].index = i;
        if (ioctl(fd, AUDIO_MIXER_DEVINFO, &info[i]) == -1) {
            --devn;
            --i;
            mixer->enabled = true;
            continue;
        }
    }

    values = calloc(devn, sizeof(*values));
    if (!values)
        return (0);

    for (i = 0; i < devn; i++) {
        values[i].dev = i;
        values[i].type = info[i].type;
        if (info[i].type != AUDIO_MIXER_CLASS) {
            values[i].un.value.num_channels = 2;
            if (ioctl(fd, AUDIO_MIXER_READ, &values[i]) == -1) {
                values[i].un.value.num_channels = 1;
                if (ioctl(fd, AUDIO_MIXER_READ, &values[i])
                    == -1)
                    return (0);
            }
        }
    }

    for (i = 0; i < devn; i++) {
        strlcpy(name, info[i].label.name, sizeof(name));
        if (!strcmp("master", name)) {
            mixer->volume_left = values[i].un.value.level[0];
            mixer->volume_right = values[i].un.value.level[1];
            mixer->enabled = true;
            break;
        }
    }

    close(fd);

    if (values)
        free(values);
    if (info)
        free(info);

#elif defined(__FreeBSD__) || defined(__DragonFly__)
    int bar;
    int fd = open("/dev/mixer", O_RDONLY);
    if (fd == -1)
        return (0);

    if ((ioctl(fd, MIXER_READ(0), &bar)) == -1) {
        return (0);
    }
    mixer->enabled = true;
    mixer->volume_left = bar & 0x7f;
    mixer->volume_right = (bar >> 8) & 0x7f;
    close(fd);
#endif
    return (mixer->enabled);
}

static void bsd_generic_temperature_state(int *temperature)
{
#if defined(__OpenBSD__) || defined(__NetBSD__)
    int mibs[5] = { CTL_HW, HW_SENSORS, 0, 0, 0 };
    int devn, numt;
    struct sensor snsr;
    size_t slen = sizeof(struct sensor);
    struct sensordev snsrdev;
    size_t sdlen = sizeof(struct sensordev);

    for (devn = 0;; devn++) {
        mibs[2] = devn;

        if (sysctl(mibs, 3, &snsrdev, &sdlen, NULL, 0) == -1) {
            if (errno == ENOENT)
                break;
            else
                continue;
        }
        if (!strcmp("cpu0", snsrdev.xname)) {
            //sensor_name = strdup("cpu0");
            break;
        } else if (!strcmp("km0", snsrdev.xname)) {
            //sensor_name = strdup("km0");
            break;
        }
    }

    for (numt = 0; numt < snsrdev.maxnumt[SENSOR_TEMP]; numt++) {
        mibs[4] = numt;

        if (sysctl(mibs, 5, &snsr, &slen, NULL, 0) == -1)
            continue;

        if (slen > 0 && (snsr.flags & SENSOR_FINVALID) == 0)
            break;
    }

    if (sysctl(mibs, 5, &snsr, &slen, NULL, 0)
        != -1) {
        *temperature = (snsr.value - 273150000) / 1000000.0;
    } else
        *temperature = INVALID_TEMP;
#elif defined(__FreeBSD__) || defined(__DragonFly__)
    unsigned int value;
    size_t len = sizeof(value);
    if ((sysctlbyname
         ("hw.acpi.thermal.tz0.temperature", &value, &len, NULL,
          0)) != -1) {
        *temperature = (value - 2732) / 10;
    } else
        *temperature = INVALID_TEMP;
#endif
}

static int bsd_generic_power_mibs_get(power_t * power)
{
    int result = 0;
#if defined(__OpenBSD__) || defined(__NetBSD__)
    struct sensordev snsrdev;
    size_t sdlen = sizeof(struct sensordev);
    int mib[5] = { CTL_HW, HW_SENSORS, 0, 0, 0 };
    int i, devn;
#elif defined(__FreeBSD__) || defined(__DragonFly__)
    size_t len;
#endif

#if defined(__OpenBSD__) || defined(__NetBSD__)
    for (devn = 0;; devn++) {
        mib[2] = devn;
        if (sysctl(mib, 3, &snsrdev, &sdlen, NULL, 0) == -1) {
            if (errno == ENXIO)
                continue;
            if (errno == ENOENT)
                break;
        }

        for (i = 0; i < MAX_BATTERIES; i++) {
            char buf[64];
            snprintf(buf, sizeof(buf), "acpibat%d", i);
            if (!strcmp(buf, snsrdev.xname)) {
                power->bat_mibs[power->battery_index] =
                    malloc(sizeof(int) * 5);
                int *tmp = power->bat_mibs[power->battery_index++];
                tmp[0] = mib[0];
                tmp[1] = mib[1];
                tmp[2] = mib[2];
            }
            result++;
        }

        if (!strcmp("acpiac0", snsrdev.xname)) {
            power->ac_mibs[0] = mib[0];
            power->ac_mibs[1] = mib[1];
            power->ac_mibs[2] = mib[2];
        }
    }
#elif defined(__FreeBSD__) || defined(__DragonFly__)
    if ((sysctlbyname("hw.acpi.battery.life", NULL, &len, NULL, 0)) != -1) {
        power->bat_mibs[power->battery_index] = malloc(sizeof(int) * 5);
        sysctlnametomib("hw.acpi.battery.life",
                        power->bat_mibs[power->battery_index], &len);
        power->battery_index = 1;
        result++;
    }

    if ((sysctlbyname("hw.acpi.acline", NULL, &len, NULL, 0)) != -1) {
        sysctlnametomib("hw.acpi.acline", power->ac_mibs, &len);
    }
#endif

    return (result);
}

static void _bsd_generic_battery_state_get(power_t * power, int *mib)
{
#if defined(__OpenBSD__) || defined(__NetBSD__)
    double last_full_charge = 0;
    double current_charge = 0;
    size_t slen = sizeof(struct sensor);
    struct sensor snsr;

    mib[3] = 7;
    mib[4] = 0;

    if (sysctl(mib, 5, &snsr, &slen, NULL, 0) != -1)
        last_full_charge = (double) snsr.value;

    mib[3] = 7;
    mib[4] = 3;

    if (sysctl(mib, 5, &snsr, &slen, NULL, 0) != -1)
        current_charge = (double) snsr.value;

    /* ACPI bug workaround... */
    if (current_charge == 0 || last_full_charge == 0) {
        mib[3] = 8;
        mib[4] = 0;

        if (sysctl(mib, 5, &snsr, &slen, NULL, 0) != -1)
            last_full_charge = (double) snsr.value;

        mib[3] = 8;
        mib[4] = 3;

        if (sysctl(mib, 5, &snsr, &slen, NULL, 0) != -1)
            current_charge = (double) snsr.value;
    }

    power->last_full_charge += last_full_charge;
    power->current_charge += current_charge;
#elif defined(__FreeBSD__) || defined(__DragonFly__)
    unsigned int value;
    size_t len = sizeof(value);
    if ((sysctl(mib, 4, &value, &len, NULL, 0)) != -1)
        power->percent = value;

#endif
}

static void bsd_generic_power_state(power_t * power)
{
    int i;
#if defined(__OpenBSD__) || defined(__NetBSD__)
    struct sensor snsr;
    int have_ac = 0;
    size_t slen = sizeof(struct sensor);
#elif defined(__FreeBSD__) || defined(__DragonFly__)
    unsigned int value;
    size_t len;
#endif

#if defined(__OpenBSD__) || defined(__NetBSD__)
    power->ac_mibs[3] = 9;
    power->ac_mibs[4] = 0;

    if (sysctl(power->ac_mibs, 5, &snsr, &slen, NULL, 0) != -1)
        have_ac = (int) snsr.value;
#elif defined(__FreeBSD__) || defined(__DragonFly__)
    len = sizeof(value);
    if ((sysctl(power->ac_mibs, 3, &value, &len, NULL, 0)) == -1) {
        return;
    }
    power->have_ac = value;
#endif

    for (i = 0; i < power->battery_index; i++)
        _bsd_generic_battery_state_get(power, power->bat_mibs[i]);

#if defined(__OpenBSD__) || defined(__NetBSD__)
    double percent =
        100 * (power->current_charge / power->last_full_charge);

    power->percent = (int) percent;
    power->have_ac = have_ac;
#elif defined(__FreeBSD__) || defined(__DragonFly__)
    len = sizeof(value);
    if ((sysctl(power->bat_mibs[0], 4, &value, &len, NULL, 0)) == -1) {
        return;
    }

    power->percent = value;

#endif
    for (i = 0; i < power->battery_index; i++)
        free(power->bat_mibs[i]);
}

static int percentage(int value, int max)
{
    double avg = (max / 100.0);
    double tmp = value / avg;

    return round(tmp);
}

static void statusbar(results_t * results, int *order, int count)
{
    int i, j, flags;
    double cpu_percent = 0;
    for (i = 0; i < count; i++) {
        flags = order[i];
        if (flags & RESULTS_CPU_CORES) {
            printf(" [CPUs]: ");
            for (j = 0; j < results->cpu_count; j++)
                printf("%.2f%% ", results->cores[j]->percent);
        } else if (flags & RESULTS_CPU) {
            for (j = 0; j < results->cpu_count; j++)
                cpu_percent += results->cores[j]->percent;

            printf(" [CPU]: %.2f%% ", cpu_percent / results->cpu_count);
        }

        if (flags & RESULTS_MEM) {
            _memsize_kb_to_mb(&results->memory.used);
            _memsize_kb_to_mb(&results->memory.total);

            printf(" [MEM]: %luM/%luM (used/total)", results->memory.used,
                   results->memory.total);
        }

        if (flags & RESULTS_PWR) {
            if (results->power.have_ac)
                printf(" [AC]: %d%%", results->power.percent);
            else
                printf(" [DC]: %d%%", results->power.percent);
        }

        if (flags & RESULTS_TMP) {
            if (results->temperature != INVALID_TEMP)
                printf(" [T]: %dC", results->temperature);
        }

        if (flags & RESULTS_AUD) {
            if (results->mixer.enabled) {
                uint8_t level =
                    results->mixer.volume_right >
                    results->mixer.volume_left ? results->
                    mixer.volume_right : results->mixer.volume_left;
#if defined(__OpenBSD__) || defined(__NetBSD__)
                int8_t perc = percentage(level, 255);
                printf(" [VOL]: %d%%", perc);
#elif defined(__FreeBSD__) || defined(__DragonFly__)
                uint8_t perc = percentage(level, 100);
                printf(" [VOL]: %d%%", perc);
#endif
            }
        }
    }
    printf(".\n");
}

static void results_cpu(cpu_core_t ** cores, int cpu_count)
{
    int i;
    for (i = 0; i < cpu_count; i++)
        printf("%.2f ", cores[i]->percent);

    printf("\n");
}

static void results_mem(meminfo_t * mem, int flags)
{
    unsigned long total, used, cached, buffered;
    unsigned long shared, swap_total, swap_used;

    total = mem->total;
    used = mem->used;
    cached = mem->cached;
    buffered = mem->buffered;
    shared = mem->shared;
    swap_total = mem->swap_total;
    swap_used = mem->swap_used;

    if (flags & RESULTS_MEM_MB) {
        _memsize_kb_to_mb(&total);
        _memsize_kb_to_mb(&used);
        _memsize_kb_to_mb(&cached);
        _memsize_kb_to_mb(&buffered);
        _memsize_kb_to_mb(&shared);
        _memsize_kb_to_mb(&swap_total);
        _memsize_kb_to_mb(&swap_used);
    } else if (flags & RESULTS_MEM_GB) {
        _memsize_kb_to_gb(&total);
        _memsize_kb_to_gb(&used);
        _memsize_kb_to_gb(&cached);
        _memsize_kb_to_gb(&buffered);
        _memsize_kb_to_gb(&shared);
        _memsize_kb_to_gb(&swap_total);
        _memsize_kb_to_gb(&swap_used);
    }

    printf("%lu %lu %lu %lu %lu %lu %lu\n",
           total, used, cached, buffered, shared, swap_total, swap_used);
}

static void results_power(power_t * power)
{
    printf("%d %d\n", power->have_ac, power->percent);
}

static void results_temperature(int temp)
{
    printf("%d\n", temp);
}

static void results_mixer(mixer_t * mixer)
{
    if (!mixer->enabled)
        return;

    printf("%d %d\n", mixer->volume_left, mixer->volume_right);
}

static void display_results(results_t * results, int *order, int count)
{
    int i, flags;
    for (i = 0; i < count; i++) {
        flags = order[i];
        if (flags & RESULTS_CPU)
            results_cpu(results->cores, results->cpu_count);
        else if (flags & RESULTS_MEM)
            results_mem(&results->memory, flags);
        else if (flags & RESULTS_PWR)
            results_power(&results->power);
        else if (flags & RESULTS_TMP)
            results_temperature(results->temperature);
        else if (flags & RESULTS_AUD)
            results_mixer(&results->mixer);
    }
}

int main(int argc, char **argv)
{
    results_t results;
    bool have_battery;
    bool statusline = false;
    int i, j = 0;
    int flags = 0;
    int order[argc];

    memset(&order, 0, sizeof(int) * (argc));

    for (i = 1; i < argc; i++) {
        if ((!strcmp(argv[i], "-h")) ||
            (!strcmp(argv[i], "-help")) || (!strcmp(argv[i], "--help"))) {
            printf("Usage: tingle [OPTIONS]\n"
                   "   Where OPTIONS can be a combination of\n"
                   "      -c\n"
                   "        Show cpu average usage (percentages).\n"
                   "      -C\n"
                   "        Show all cpu cores and usage.\n"
                   "      -m (kb) -M (MB) -G (GB)\n"
                   "        Show memory usage (unit).\n"
                   "      -p\n"
                   "        Show power status (ac and battery percentage).\n"
                   "      -t\n"
                   "        Show temperature sensors (temperature in celcius).\n"
                   "      -a\n"
                   "        Display mixer values (system values).\n"
                   "      -s\n"
                   "        Show all in a nicely formatted status-bar format.\n"
                   "        This is the default behaviour with no arguments.\n"
                   "        With other flags specify which components to \n"
                   "        display in the status bar.\n"
                   "      -h | -help | --help\n" "        This help.\n");
            exit(0);
        }

        if (!strcmp(argv[i], "-c"))
            order[j] |= RESULTS_CPU;
        else if (!strcmp(argv[i], "-C"))
            order[j] |= RESULTS_CPU | RESULTS_CPU_CORES;
        else if (!strcmp(argv[i], "-m"))
            order[j] |= RESULTS_MEM;
        else if (!strcmp(argv[i], "-M"))
            order[j] |= RESULTS_MEM | RESULTS_MEM_MB;
        else if (!strcmp(argv[i], "-G"))
            order[j] |= RESULTS_MEM | RESULTS_MEM_GB;
        else if (!strcmp(argv[i], "-p"))
            order[j] |= RESULTS_PWR;
        else if (!strcmp(argv[i], "-t"))
            order[j] |= RESULTS_TMP;
        else if (!strcmp(argv[i], "-a"))
            order[j] |= RESULTS_AUD;
        else if (!strcmp(argv[i], "-s")) {
            statusline = true;
        }
        flags |= order[j++];
    }

    if (flags == 0) {
        flags |= RESULTS_ALL;
        order[0] |= RESULTS_ALL;
        statusline = true;
    }

    memset(&results, 0, sizeof(results_t));

    if (flags & RESULTS_CPU)
        results.cores = bsd_generic_cpuinfo(&results.cpu_count);

    if (flags & RESULTS_MEM)
        bsd_generic_meminfo(&results.memory);

    if (flags & RESULTS_PWR) {
        have_battery = bsd_generic_power_mibs_get(&results.power);
        if (have_battery)
            bsd_generic_power_state(&results.power);
    }

    if (flags & RESULTS_TMP)
        bsd_generic_temperature_state(&results.temperature);

    if (flags & RESULTS_AUD)
        bsd_generic_audio_state_master(&results.mixer);

    if (statusline)
        statusbar(&results, order, j ? j : 1);
    else
        display_results(&results, order, j);

    if (flags & RESULTS_CPU) {
        for (i = 0; i < results.cpu_count; i++)
            free(results.cores[i]);
        free(results.cores);
    }

    return (EXIT_SUCCESS);
}
