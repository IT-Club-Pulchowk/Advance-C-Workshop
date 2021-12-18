#define BMPMALLOC(size) bump(size)

#include "bump.c"
#include "bmp_lekhak.h"
#include "threading.h"

#define IM_WIDTH 1600
#define IM_HEIGHT 1600

#define MN_X_SCALE_MIN -2.00
#define MN_X_SCALE_MAX 0.47
#define MN_Y_SCALE_MIN -1.12
#define MN_Y_SCALE_MAX 1.12
#define MAX_ITER 1000

#if PF_LINUX
#include <unistd.h>
#include <time.h>
typedef struct timespec timer;

#define get_core_count() sysconf(_SC_NPROCESSORS_CONF)

void start_timing(timer *st) {
    clock_gettime(CLOCK_MONOTONIC, st);
}

uint32_t end_timing(timer st) {
    timer end;
    clock_gettime(CLOCK_MONOTONIC, &end);
    return end.tv_sec * 1000 - st.tv_sec * 1000 + (end.tv_nsec - st.tv_nsec) / 1000000;
}
#elif PF_WINDOWS
#define VC_EXTRALEAN
#include <Windows.h>
#include <sysinfoapi.h>
typedef LARGE_INTEGER timer;

long get_core_count() {
    SYSTEM_INFO tmp;
    GetSystemInfo(&tmp);
    return tmp.dwNumberOfProcessors;
}

void start_timing(timer *StartingTime) {
    QueryPerformanceCounter(StartingTime);
}

uint32_t end_timing (timer StartingTime) {
    LARGE_INTEGER Frequency, EndingTime, ElapsedMicroseconds;
    QueryPerformanceFrequency(&Frequency);
    QueryPerformanceCounter(&EndingTime);
    ElapsedMicroseconds.QuadPart = EndingTime.QuadPart - StartingTime.QuadPart;

    ElapsedMicroseconds.QuadPart *= 1000000;
    ElapsedMicroseconds.QuadPart /= Frequency.QuadPart;

    return ElapsedMicroseconds.QuadPart / 1000;
}
#endif

uint32_t rgb_to_hex(uint8_t r, uint8_t g, uint8_t b) {
    return r << 16 | g << 8 | b;
}

float map_range(float pmin, float pmax, float nmin, float nmax, float value) {
    float ret = (value - pmin) * (nmax - nmin) / (pmax - pmin) + nmin;
    return ret;
}

float lerp (float v0, float v1, float t) {
    return (1 - t) * v0 + t * v1;
}

uint32_t get_color(float t) {
    uint8_t r = 0, g = 0, b = 0;

    if (t >= 0.0 && t < 0.16) {
        t = (t - 0) / (0.16 - 0);
        r = lerp(211, 215, t);
        g = lerp(63 , 192, t);
        b = lerp(73 , 208, t);
    } else if (t >= 0.16 && t < 0.42) {
        t = (t - 0.16) / (0.42 - 0.16);
        r = lerp(215, 239, t);
        g = lerp(192, 240, t);
        b = lerp(208, 209, t);
    } else if (t >= 0.42 && t < 0.6425) {
        t = (t - 0.42) / (0.6425 - 0.42);
        r = lerp(239, 119, t);
        g = lerp(240, 186, t);
        b = lerp(209, 153, t);
    } else if (t >= 0.6425 && t < 0.8575) {
        t = (t - 0.6425) / (0.8575 - 0.6425);
        r = lerp(119, 38 , t);
        g = lerp(186, 39 , t);
        b = lerp(153, 48 , t);
    } else {
        t = (t - 0.8575) / (1 - 0.8575);
        r = lerp(38 , 0  , t);
        g = lerp(39 , 0  , t);
        b = lerp(48 , 0  , t);
    }

    return rgb_to_hex(r, g, b);
}

volatile uint32_t progress;
typedef struct {
    uint32_t *chunk;
    uint32_t start;
    uint32_t height;
} canvas;

int draw(void *pxl) {
    canvas *can = (canvas *) pxl;
    for (int Py = can->start; Py < can->start + can->height && Py <= IM_HEIGHT / 2; Py ++) {
        float y0 = map_range(0, IM_HEIGHT, MN_Y_SCALE_MIN, MN_Y_SCALE_MAX, Py);
        for(int Px = 0; Px < IM_WIDTH; Px ++) {
            float x0 = map_range(0, IM_WIDTH, MN_X_SCALE_MIN, MN_X_SCALE_MAX, Px);
            float x = 0, y = 0, x2 = 0, y2 = 0;
            int iteration = 0;
            while (x2 + y2 <= 4 && iteration < MAX_ITER) {
                y = 2 * x * y + y0;
                x = x2 - y2 + x0;
                x2 = x * x;
                y2 = y * y;
                iteration += 1;
            }
            uint32_t color = get_color((float)iteration / MAX_ITER);
            can->chunk[Py * IM_WIDTH + Px] = color;
            can->chunk[(IM_HEIGHT - Py - 1) * IM_WIDTH + Px] = color;
            interlocked_inc(&progress);
        }
    }
    return 0;
}

int main() {
    init_bump_context(megabytes(1024));
    timer tmr;
    FILE *outf = fopen("threadperf.csv", "wb");
    fprintf(outf, "Threads,Performance\n");
    for (uint32_t thread_count = 1; thread_count <= get_core_count() * 2; thread_count ++){
        progress = 0;
        printf("Thread Count: %10d\n", thread_count);
        start_timing(&tmr);
        BMP image = create_bmp(IM_WIDTH, IM_HEIGHT);

        thrd_t *rendering_threads = bump(thread_count * sizeof(*rendering_threads));
        uint32_t height_for_threads = IM_HEIGHT / 2 / thread_count;
        canvas *a = bump(thread_count * sizeof(*a));

        for (int i = 0; i < thread_count; i ++) {
            a[i].chunk  = image.pdata;
            a[i].start  = i * height_for_threads;
            a[i].height = height_for_threads;
        }

        if (a[thread_count - 1].start + a[thread_count - 1].height != IM_HEIGHT / 2) {
            a[thread_count - 1].height = IM_HEIGHT / 2 - a[thread_count - 1].start;
        }

        for (int i = 0; i < thread_count; i ++) {
            thrd_create(&rendering_threads[i], draw, &a[i]);
        }

        while (progress < IM_WIDTH * IM_HEIGHT / 2) {
            thrd_sleep_millisecs(20);
            printf("Progress: %10f%%\r", (float)progress / (IM_WIDTH * IM_HEIGHT / 2.0) * 100);
        }
        printf("\n");

        for (int i = 0; i < thread_count; i ++) {
            thrd_join(rendering_threads[i], NULL);
        }
        uint32_t diff = end_timing(tmr);
        printf("Time Elapsed: %10d ms\n", diff);
        printf("===========================\n");
        fprintf(outf, "%d,%d\n", thread_count, diff);
        reset_bump_context();
    }
    fclose(outf);
    end_bump_context();
}