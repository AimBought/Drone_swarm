// MUSI BYÆ PIERWSZE!
#define _GNU_SOURCE 

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/msg.h>
#include <limits.h>

#include "../include/common.h"
#include "../include/ipc_wrapper.h"

// --- Implementacja Wrapperów ---

int safe_semop(int semid, struct sembuf *sops, size_t nsops) {
    int res;
    do {
        res = semop(semid, sops, nsops);
    } while (res == -1 && errno == EINTR);
    return res;
}

ssize_t safe_msgrcv(int msqid, void *msgp, size_t msgsz, long msgtyp, int msgflg) {
    ssize_t res;
    do {
        res = msgrcv(msqid, msgp, msgsz, msgtyp, msgflg);
    } while (res == -1 && errno == EINTR);
    return res;
}

void custom_wait(int semid, double seconds) {
    struct timespec ts;
    ts.tv_sec = (time_t)seconds;
    ts.tv_nsec = (long)((seconds - ts.tv_sec) * 1000000000L);

    struct sembuf op = {SEM_TIMER, -1, 0};
    
    // U¿ywa semtimedop (dostêpne dziêki _GNU_SOURCE na górze pliku)
    semtimedop(semid, &op, 1, &ts);
}

int parse_int(const char *str, const char *name) {
    char *endptr;
    errno = 0;
    long val = strtol(str, &endptr, 10);

    if (errno != 0 || *endptr != '\0' || val <= 0 || val > INT_MAX) {
        fprintf(stderr, C_RED "Error: Invalid value for %s: '%s'. Must be positive int.\n" C_RESET, name, str);
        return -1;
    }
    return (int)val;
}