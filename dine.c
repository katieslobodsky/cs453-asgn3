// dine.c: Dining Philosophers with pthreads + POSIX semaphores

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>
#include <time.h>
#include <sys/time.h>

#ifndef NUM_PHILOSOPHERS
#define NUM_PHILOSOPHERS 5
#endif

#ifndef DAWDLEFACTOR
#define DAWDLEFACTOR 1000  // up to 1000ms
#endif

typedef enum { ST_CHANGING=0, ST_EATING, ST_THINKING } state_t;

typedef struct {
    int id;                 // 0..N-1
    int left_fork;          // fork index (same as id)
    int right_fork;         // (id+1)%N
    int cycles;             // remaining eat→think cycles
} phil_arg_t;

// Globals
static sem_t forks[NUM_PHILOSOPHERS];     // one semaphore per fork
static pthread_t tids[NUM_PHILOSOPHERS];
static phil_arg_t args[NUM_PHILOSOPHERS];

static pthread_mutex_t print_mtx = PTHREAD_MUTEX_INITIALIZER;

// for status display
static state_t g_state[NUM_PHILOSOPHERS];
static int g_hold_left[NUM_PHILOSOPHERS];   // bool
static int g_hold_right[NUM_PHILOSOPHERS];  // bool

// ---------- Utils ----------
static void die_errno(const char *msg, int err) {
    if (err == 0) return;
    fprintf(stderr, "%s: %s\n", msg, strerror(err));
    exit(1);
}

static void dawdle(void) {
    // sleep for 0..DAWDLEFACTOR ms (pseudo-random)
    struct timespec tv;
    long ms = random() % (DAWDLEFACTOR + 1);
    tv.tv_sec = ms / 1000;
    tv.tv_nsec = (ms % 1000) * 1000000L;
    if (nanosleep(&tv, NULL) == -1) {
        perror("nanosleep");
        // keep going; this is non-fatal for the assignment
    }
}

static char label_for(int i) {
    // Spec says: start at 'A' and continue up the ASCII table if > 26
    return (char)('A' + i);
}

// Build a per-column forks string of length NUM_PHILOSOPHERS,
// marking ONLY the forks held by THIS philosopher with their index digit
// and '-' elsewhere, to mimic the sample output idea.
static void build_fork_str(int pid, char *buf, size_t buflen) {
    // buflen expected >= NUM_PHILOSOPHERS + 1
    for (int i = 0; i < NUM_PHILOSOPHERS && (size_t)i < buflen-1; i++) {
        buf[i] = '-';
    }
    buf[(NUM_PHILOSOPHERS < (int)buflen-1) ? NUM_PHILOSOPHERS : (int)buflen-1] = '\0';

    int lf = args[pid].left_fork;
    int rf = args[pid].right_fork;

    if (g_hold_left[pid]) {
        buf[lf] = (char)('0' + (lf % 10));
    }
    if (g_hold_right[pid]) {
        buf[rf] = (char)('0' + (rf % 10));
    }
}

static const char* state_suffix(state_t s) {
    switch (s) {
        case ST_EATING:   return " Eat";
        case ST_THINKING: return " Think";
        default:          return " ";
    }
}

// Print header once at start
static void print_header(void) {
    pthread_mutex_lock(&print_mtx);

    // Top border line (per sample, 5 columns shown; generalize by N)
    printf("|");
    for (int i = 0; i < NUM_PHILOSOPHERS; i++) {
        printf("=============|");
    }
    printf("\n| ");
    for (int i = 0; i < NUM_PHILOSOPHERS; i++) {
        printf("%c           | ", label_for(i));
    }
    printf("\n|");
    for (int i = 0; i < NUM_PHILOSOPHERS; i++) {
        printf("=============|");
    }
    printf("\n");

    // Initial line (everyone changing, holding nothing)
    // We still print one status line to match the sample's style
    char fbuf[NUM_PHILOSOPHERS + 1];
    printf("| ");
    for (int i = 0; i < NUM_PHILOSOPHERS; i++) {
        build_fork_str(i, fbuf, sizeof fbuf);
        // Column is loosely fixed-width; keep simple formatting
        printf("%-5s%-7s| ", fbuf, "");  // changing prints no suffix
    }
    printf("\n");

    pthread_mutex_unlock(&print_mtx);
}

// Print a single change line reflecting current global state
static void print_status_one_change(void) {
    pthread_mutex_lock(&print_mtx);

    char fbuf[NUM_PHILOSOPHERS + 1];
    printf("| ");
    for (int i = 0; i < NUM_PHILOSOPHERS; i++) {
        build_fork_str(i, fbuf, sizeof fbuf);
        const char *suf = state_suffix(g_state[i]);
        // Show fork string + (maybe) " Eat"/" Think", blank for changing
        // pad so columns line up somewhat consistently
        // 5 chars forks + up to 6 chars state = ~11 chars, then "| "
        printf("%-5s%-7s| ", fbuf, suf);
    }
    printf("\n");

    pthread_mutex_unlock(&print_mtx);
}

// ---------- Philosopher Logic ----------
static void pick_first_fork(int pid, int first_is_left) {
    int fork_idx = first_is_left ? args[pid].left_fork : args[pid].right_fork;
    // wait
    while (sem_wait(&forks[fork_idx]) == -1 && errno == EINTR) {}
    if (first_is_left) g_hold_left[pid] = 1; else g_hold_right[pid] = 1;
    print_status_one_change();
}

static void pick_second_fork(int pid, int first_is_left) {
    int fork_idx = first_is_left ? args[pid].right_fork : args[pid].left_fork;
    while (sem_wait(&forks[fork_idx]) == -1 && errno == EINTR) {}
    if (first_is_left) g_hold_right[pid] = 1; else g_hold_left[pid] = 1;
    print_status_one_change();
}

static void put_down_one_fork(int pid, int left) {
    int fork_idx = left ? args[pid].left_fork : args[pid].right_fork;
    if (left) g_hold_left[pid] = 0; else g_hold_right[pid] = 0;
    // post
    if (sem_post(&forks[fork_idx]) == -1) {
        perror("sem_post");
        exit(1);
    }
    print_status_one_change();
}

static void *philosopher(void *vp) {
    phil_arg_t *p = (phil_arg_t*)vp;
    int id = p->id;

    // Start hungry, in "changing", then attempt to EAT first
    g_state[id] = ST_CHANGING;  // already the default
    print_status_one_change();

    const int even = (id % 2 == 0);

    // Odd/even strategy to avoid deadlock:
    // even picks RIGHT first, odd picks LEFT first (or vice versa; just be consistent)
    // We'll do: even -> right first; odd -> left first.
    while (p->cycles > 0) {
        // ---- Acquire forks (CHANGING) ----
        g_state[id] = ST_CHANGING;
        print_status_one_change();

        pick_first_fork(id, /*first_is_left=*/!even);
        pick_second_fork(id, /*first_is_left=*/!even);

        // ---- Eat ----
        g_state[id] = ST_EATING;
        print_status_one_change();
        dawdle();

        // ---- Transition to set forks down ----
        g_state[id] = ST_CHANGING;
        print_status_one_change();

        // Put down one at a time
        put_down_one_fork(id, /*left=*/!even); // reverse order of acquisition not required but fine
        put_down_one_fork(id, /*left=*/even);

        // ---- Think ----
        g_state[id] = ST_THINKING;
        print_status_one_change();
        dawdle();

        // Prepare next iteration
        p->cycles--;
    }

    // Transition from thinking to terminated counts as "changing"
    g_state[id] = ST_CHANGING;
    print_status_one_change();

    return NULL;
}

// ---------- main ----------
int main(int argc, char **argv) {
    // Seed PRNG with time of day (seconds + usec), per assignment hint
    struct timeval tv;
    if (gettimeofday(&tv, NULL) == -1) {
        perror("gettimeofday");
        return 1;
    }
    srandom((unsigned)(tv.tv_sec ^ tv.tv_usec));

    // Parse optional cycles argument
    long cycles = 1;
    if (argc >= 2) {
        char *end = NULL;
        errno = 0;
        long val = strtol(argv[1], &end, 10);
        if (errno || end == argv[1] || val <= 0 || val > INT_MAX) {
            fprintf(stderr, "Usage: %s [positive cycles]\n", argv[0]);
            return 1;
        }
        cycles = val;
    }

    // init shared state
    for (int i = 0; i < NUM_PHILOSOPHERS; i++) {
        g_state[i] = ST_CHANGING;
        g_hold_left[i] = 0;
        g_hold_right[i] = 0;
    }

    // init semaphores (forks)
    for (int i = 0; i < NUM_PHILOSOPHERS; i++) {
        if (sem_init(&forks[i], 0, 1) == -1) {
            perror("sem_init");
            return 1;
        }
    }

    print_header();

    // create threads
    for (int i = 0; i < NUM_PHILOSOPHERS; i++) {
        args[i].id = i;
        args[i].left_fork  = i;
        args[i].right_fork = (i + 1) % NUM_PHILOSOPHERS;
        args[i].cycles = (int)cycles;

        int rc = pthread_create(&tids[i], NULL, philosopher, &args[i]);
        if (rc != 0) die_errno("pthread_create", rc);
    }

    // join threads (note: join order fixed; OK per assignment)
    for (int i = 0; i < NUM_PHILOSOPHERS; i++) {
        int rc = pthread_join(tids[i], NULL);
        if (rc != 0) die_errno("pthread_join", rc);
    }

    // bottom border (to make output look tidy like sample)
    pthread_mutex_lock(&print_mtx);
    printf("|");
    for (int i = 0; i < NUM_PHILOSOPHERS; i++) printf("=============|");
    printf("\n");
    pthread_mutex_unlock(&print_mtx);

    // destroy semaphores
    for (int i = 0; i < NUM_PHILOSOPHERS; i++) {
        if (sem_destroy(&forks[i]) == -1) {
            perror("sem_destroy");
            // continue; exiting anyway
        }
    }

    return 0;
}
