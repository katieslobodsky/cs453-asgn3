// dine.c
#define _POSIX_C_SOURCE 200809L

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
#define DAWDLEFACTOR 1000
#endif

// possible states of a philosopher
typedef enum {
    ST_CHANGING=0, 
    ST_EATING, 
    ST_THINKING
}
state_t;

// information for each philosopher thread
typedef struct {
    int id; // 0..N-1
    int left_fork; // fork index (same as id)
    int right_fork; // (id+1)%N
    int cycles; // remaining eat/think cycles
}
phil_arg_t;

// global variables
static sem_t forks[NUM_PHILOSOPHERS];
static pthread_t tids[NUM_PHILOSOPHERS];
static phil_arg_t args[NUM_PHILOSOPHERS];
static pthread_mutex_t print_mtx = PTHREAD_MUTEX_INITIALIZER;

// for status display
static state_t g_state[NUM_PHILOSOPHERS];
static int g_hold_left[NUM_PHILOSOPHERS];
static int g_hold_right[NUM_PHILOSOPHERS];

// ----- utils -----
static void die_errno(const char *msg, int err) {
    if (err == 0) return;
    fprintf(stderr, "%s: %s\n", msg, strerror(err));
    exit(1);
}

// causes the philosopher to pause for a random
// amount of time between 0 and DAWDLEFACTOR milliseconds
static void dawdle(void) {
    // sleep for 0..DAWDLEFACTOR ms
    struct timespec tv;
    long ms = random() % (DAWDLEFACTOR + 1);
    tv.tv_sec = ms / 1000;
    tv.tv_nsec = (ms % 1000) * 1000000L;
    if (nanosleep(&tv, NULL) == -1) {
        perror("nanosleep");
    }
}

static char label_for(int i) {
    // start at 'A' and continue up the ASCII table
    return (char)('A' + i);
}

// build a forks string per column of length NUM_PHILOSOPHERS,
// marking only the forks held by this philosopher with their index digit
// and '-' elsewhere
static void build_fork_str(int pid, char *buf, size_t buflen) {
    // display dashes '-'
    for (int i = 0; i < NUM_PHILOSOPHERS && (size_t)i < buflen - 1; i++) {
        buf[i] = '-';
    }
    // null terminate
    if (NUM_PHILOSOPHERS < (int)buflen - 1) {
        buf[NUM_PHILOSOPHERS] = '\0';
    }
    else {
        buf[buflen - 1] = '\0';
    }

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

// print header once at start
static void print_header(void) {
    pthread_mutex_lock(&print_mtx);

    // print top border line
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

    // initial line: philosophers changing, holding nothing
    char fbuf[NUM_PHILOSOPHERS + 1];
    printf("| ");
    for (int i = 0; i < NUM_PHILOSOPHERS; i++) {
        build_fork_str(i, fbuf, sizeof fbuf);
        printf("%-5s%-7s| ", fbuf, "");
    }
    printf("\n");
    pthread_mutex_unlock(&print_mtx);
}

// print a single change line reflecting current global state
static void print_status_one_change(void) {
    pthread_mutex_lock(&print_mtx);

    char fbuf[NUM_PHILOSOPHERS + 1];
    printf("| ");
    for (int i = 0; i < NUM_PHILOSOPHERS; i++) {
        build_fork_str(i, fbuf, sizeof fbuf);
        const char *suf = state_suffix(g_state[i]);
        // show fork string + " Eat"/" Think", blank for changing
        // align columns for neatness
        printf("%-5s%-7s| ", fbuf, suf);
    }
    printf("\n");
    pthread_mutex_unlock(&print_mtx);
}

// ----- philosopher functions ------

// picks up the philosopher's first fork based on the specified order
static void pick_first_fork(int pid, int first_is_left) {
    int fork_idx;
    if (first_is_left) {
        fork_idx = args[pid].left_fork;
    } else {
        fork_idx = args[pid].right_fork;
    }

    // wait
    while (sem_wait(&forks[fork_idx]) == -1 && errno == EINTR) {}

    if (first_is_left) {
        g_hold_left[pid] = 1;
    } else {
        g_hold_right[pid] = 1;
    }

    print_status_one_change();
}

// picks up the philosopher's second fork
static void pick_second_fork(int pid, int first_is_left) {
    int fork_idx;
    if (first_is_left) {
        fork_idx = args[pid].right_fork;
    } else {
        fork_idx = args[pid].left_fork;
    }

    while (sem_wait(&forks[fork_idx]) == -1 && errno == EINTR) {}

    if (first_is_left) {
        g_hold_right[pid] = 1;
    } else {
        g_hold_left[pid] = 1;
    }

    print_status_one_change();
}

// releases one of the philosopher's forks
static void put_down_one_fork(int pid, int left) {
    int fork_idx;
    if (left) {
        fork_idx = args[pid].left_fork;
        g_hold_left[pid] = 0;
    } else {
        fork_idx = args[pid].right_fork;
        g_hold_right[pid] = 0;
    }

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

    // start hungry (changing) then attempt to eat first
    g_state[id] = ST_CHANGING;
    print_status_one_change();

    const int even = (id % 2 == 0);

    // odd/even strategy to avoid deadlock:
    // even picks RIGHT first, odd picks LEFT first
    // even -> right first; odd -> left first
    while (p->cycles > 0) {
        // ---- acquire forks (changing) ----
        g_state[id] = ST_CHANGING;
        print_status_one_change();

        pick_first_fork(id, !even);
        pick_second_fork(id, !even);

        // ---- eat ----
        g_state[id] = ST_EATING;
        print_status_one_change();
        dawdle();

        // ---- transition to set forks down ----
        g_state[id] = ST_CHANGING;
        print_status_one_change();

        // put down one at a time
        put_down_one_fork(id, !even);
        put_down_one_fork(id, even);

        // think
        g_state[id] = ST_THINKING;
        print_status_one_change();
        dawdle();

        // prepare next cycle
        p->cycles--;
    }

    // transition from thinking to terminated counts as changing
    g_state[id] = ST_CHANGING;
    print_status_one_change();
    return NULL;
}

// ----- main -----
int main(int argc, char **argv) {
    struct timeval tv;
    if (gettimeofday(&tv, NULL) == -1) {
        perror("gettimeofday");
        return 1;
    }
    srandom((unsigned)(tv.tv_sec ^ tv.tv_usec));

    // parse optional cycles argument
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

    // join threads
    for (int i = 0; i < NUM_PHILOSOPHERS; i++) {
        int rc = pthread_join(tids[i], NULL);
        if (rc != 0) die_errno("pthread_join", rc);
    }

    // bottom border
    pthread_mutex_lock(&print_mtx);
    printf("|");
    for (int i = 0; i < NUM_PHILOSOPHERS; i++) printf("=============|");
    printf("\n");
    pthread_mutex_unlock(&print_mtx);

    // destroy semaphores
    for (int i = 0; i < NUM_PHILOSOPHERS; i++) {
        if (sem_destroy(&forks[i]) == -1) {
            perror("sem_destroy");
        }
    }

    return 0;
}
