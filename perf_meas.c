/** 
 * Priority queue test harness.
 *
 *
 * Copyright (c) 2013-2018, Jonatan Linden
 *
 */

#define _GNU_SOURCE
#include <unistd.h>
#include <stdlib.h>
#include <inttypes.h>
#include <pthread.h>
#include <time.h>
#include <assert.h>
#include <math.h>

#include <limits.h>

#include "gc/gc.h"

#include "common.h"
#include "prioq.h"

/* check your cpu core numbering before pinning */
#define PIN

#define DEFAULT_SECS 10
#define DEFAULT_NTHREADS 1
#define DEFAULT_OFFSET 32
#define DEFAULT_SIZE 1<<15
#define EXPS 100000000

#define THREAD_ARGS_FOREACH(_iter) \
    for (int i = 0; i < nthreads && (_iter = &ts[i]); i++)


/* preload array with exponentially distanced integers for the
 * DES workload */
unsigned long *exps;
int exps_pos = 0;
void gen_exps(unsigned long *arr, unsigned short rng[3], int len, int intensity);

/* the workloads */
void work_exp (pq_t *pq);
void work_uni (pq_t *pq);

void *run (void *_args);


void (* work)(pq_t *pq);
thread_args_t *ts;
pq_t *pq;

volatile int wait_barrier  = 0;
volatile int loop  = 0;


static void
usage(FILE *out, const char *argv0)
{
    fprintf(out, "Usage: %s [OPTION]...\n"
	    "\n"
	    "Options:\n", argv0);

    fprintf(out, "\t-h\t\tDisplay usage.\n");
    fprintf(out, "\t-t SECS\t\tRun for SECS seconds. "
	    "Default: %i\n",
	    DEFAULT_SECS);
    fprintf(out, "\t-o OFFSET\tUse an offset of OFFSET nodes. Sensible "
	    "\n\t\t\tvalues could be 16 for 8 threads, 128 for 32 threads. "
	    "\n\t\t\tDefault: %i\n",
	    DEFAULT_OFFSET);
    fprintf(out, "\t-n NUM\t\tUse NUM threads. "
	    "Default: %i\n",
	    DEFAULT_NTHREADS);
    fprintf(out, "\t-s SIZE\t\tInitialize queue with SIZE elements. "
	    "Default: %i\n",
	    DEFAULT_SIZE);
}



static inline unsigned long
next_geometric (unsigned short seed[3], unsigned int p)
{
    /* inverse transform sampling */
    /* cf. https://en.wikipedia.org/wiki/Geometric_distribution */
    return floor(log(erand48(seed))/log(1 - p));
    /* uniformly distributed bits => geom. dist. level, p = 0.5 */
    //return __builtin_ctz(nrand48(seed) & (1LU << max) - 1) + 1;
}


int
main (int argc, char **argv) 
{
    int opt;
    unsigned short rng[3];
    struct timespec time;
    struct timespec start, end;
    thread_args_t *t;
    unsigned long elem;
    
    extern char *optarg;
    extern int optind, optopt;
    int nthreads	= DEFAULT_NTHREADS;
    int offset		= DEFAULT_OFFSET;
    int secs		= DEFAULT_SECS;
    int exp		= 0;
    int init_size	= DEFAULT_SIZE;
    int concise         = 0;
    work		= work_uni;
    
    while ((opt = getopt(argc, argv, "t:n:o:s:hex")) >= 0) {
        switch (opt) {
        case 'n': nthreads	= atoi(optarg); break;
        case 't': secs		= atoi(optarg); break;
        case 'o': offset	= atoi(optarg); break;
        case 's': init_size	= atoi(optarg); break;
        case 'x': concise       = 1; break;
        case 'e': exp		= 1; work = work_exp; break;
        case 'h': usage(stdout, argv[0]); exit(EXIT_SUCCESS); break;
        }
    }

#ifndef PIN
    printf("Running without threads pinned to cores.\n");
#endif

    E_NULL(ts = malloc(nthreads*sizeof(thread_args_t)));
    memset(ts, 0, nthreads*sizeof(thread_args_t));

    // finally available in macos 10.12 as well!
    clock_gettime(CLOCK_REALTIME, &time);

    /* initialize seed */
    rng[0] = time.tv_nsec;
    rng[1] = time.tv_nsec >> 16;
    rng[2] = time.tv_nsec >> 32;

    /* initialize garbage collection */
    _init_gc_subsystem();
    pq = pq_init(offset);

    // if DES workload, pre-sample values/event times
    if (exp) {
        E_NULL(exps = (unsigned long *)malloc(sizeof(unsigned long) * EXPS));
        gen_exps(exps, rng, EXPS, 1000);
    }
    
    /* pre-fill priority queue with elements */
    for (int i = 0; i < init_size; i++) {
        if (exp) {
            elem = exps[exps_pos++];
            insert(pq, elem, (void *)elem);
        } else {
            elem = nrand48(rng);
            insert(pq, elem, (void *)elem);
        }
    }


    /* initialize threads */
    THREAD_ARGS_FOREACH(t) {
        t->id = i;
        rng_init(t->rng);
        E_en(pthread_create(&t->thread, NULL, run, t));
    }

    /* RUN BENCHMARK */

    /* wait for all threads to call in */
    while (wait_barrier != nthreads) ;
    IRMB();
    gettime(&start);
    loop = 1;
    IWMB();
    /* Process might sleep longer than specified,
     * but this will be accounted for. */
    usleep( 1000000 * secs );
    loop = 0; /* halt all threads */
    IWMB();
    gettime(&end);

    /* END RUN BENCHMARK */

    THREAD_ARGS_FOREACH(t) {
        pthread_join(t->thread, NULL);
    }

    /* PRINT PERF. MEASURES */
    int sum = 0, min = INT_MAX, max =0;

    THREAD_ARGS_FOREACH(t) {
        sum += t->measure;
        min = min(min, t->measure);
        max = max(max, t->measure);
    }
    struct timespec elapsed = timediff(start, end);
    double dt = elapsed.tv_sec + (double)elapsed.tv_nsec / 1000000000.0;


    if (!concise) {
        printf("Total time:\t%1.8f s\n", dt);
        printf("Ops:\t\t%d\n", sum);
        printf("Ops/s:\t\t%.0f\n", (double) sum / dt);
        printf("Min ops/t:\t%d\n", min);
        printf("Max ops/t:\t%d\n", max);
    } else {
        printf("%li\n", lround((double) sum / dt));
        
    }
    
    /* CLEANUP */
    pq_destroy(pq);
    free (ts);
    _destroy_gc_subsystem();
}


__thread thread_args_t *args; 

/* uniform workload */
void
work_uni (pq_t *pq)  
{
    unsigned long elem;

    if (erand48(args->rng) < 0.5) {
        elem = (unsigned long)1 + nrand48(args->rng);
        insert(pq, elem, (void *)elem);
    } else 
        deletemin(pq);
}

/* DES workload */
void
work_exp (pq_t *pq)  
{
    int pos;
    unsigned long elem;
    deletemin(pq);
    pos = __sync_fetch_and_add(&exps_pos, 1);
    elem = exps[pos];
    insert(pq, elem, (void *)elem);
}


void *
run (void *_args)
{
    args = (thread_args_t *)_args;
    int cnt = 0;


#if defined(PIN) && defined(__linux__)
    /* Straight allocation on 32 core machine.
     * Check with your OS + machine.  */
    pin (gettid(), args->id/8 + 4*(args->id % 8));
#endif

    // call in to main thread
    __sync_fetch_and_add(&wait_barrier, 1);

    // wait until signaled by main thread
    while (!loop);
    /* start benchmark execution */
    do {
	work(pq);
        cnt++;
    } while (loop);
    /* end of measured execution */

    args->measure = cnt;
    return NULL;
}


/* generate array of exponentially distributed variables */
void
gen_exps(unsigned long *arr, unsigned short rng[3], int len, int intensity)
{
    int i = 0;
    arr[0] = 2;
    while (++i < len)
	arr[i] = arr[i-1] + 
	    next_geometric(rng, intensity);
}



