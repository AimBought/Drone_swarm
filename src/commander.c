/*
 commander.c
 - uruchamia operatora jako ./operator P
 - uruchamia N dronów (execl ./drone <id>)
 - trzyma listê pidów
 - obs³uguje SIGINT: przesy³a SIGINT do operatora i do wszystkich dronów,
   czeka na zakoñczenie i koñczy siê
*/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

static pid_t op_pid = -1;
static pid_t *drone_pids = NULL;
static int drone_count = 0;
static volatile sig_atomic_t stop_requested = 0;

void sigint_handler(int sig) {
    (void)sig;
    stop_requested = 1;
    printf("\n[Commander] SIGINT received: shutting down operator and drones...\n");
    if (op_pid > 0) kill(op_pid, SIGINT);
    for (int i = 0; i < drone_count; ++i) {
        if (drone_pids[i] > 0) kill(drone_pids[i], SIGINT);
    }
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <P> <N>\n", argv[0]);
        return 1;
    }

    int P = atoi(argv[1]);
    int N = atoi(argv[2]);
    if (P <= 0 || N <= 0) {
        fprintf(stderr, "P and N must be > 0\n");
        return 1;
    }

    drone_pids = calloc(N, sizeof(pid_t));
    drone_count = N;

    signal(SIGINT, sigint_handler);

    // uruchom operatora
    op_pid = fork();
    if (op_pid == 0) {
        // child
        char argP[16];
        snprintf(argP, sizeof(argP), "%d", P);
        execl("./operator", "operator", argP, NULL);
        perror("[Commander] execl operator");
        exit(1);
    }

    // daj operatorowi chwilê na ustawienie IPC
    sleep(1);

    // uruchom drony
    for (int i = 0; i < N; ++i) {
        pid_t pid = fork();
        if (pid == 0) {
            // child -> uruchom drone z ID = i
            char idstr[16];
            snprintf(idstr, sizeof(idstr), "%d", i);
            execl("./drone", "drone", idstr, NULL);
            perror("[Commander] execl drone");
            exit(1);
        } else if (pid > 0) {
            drone_pids[i] = pid;
            printf("[Commander] Launched drone %d (pid=%d)\n", i, pid);
        } else {
            perror("[Commander] fork drone");
        }
    }

    printf("[Commander] All processes launched. Press Ctrl+C to stop the simulation.\n");

    // czekamy a¿ u¿ytkownik przerwie
    while (!stop_requested) {
        pause();
    }

    // poczekaj na zakoñczenie dzieci
    for (int i = 0; i < N; ++i) {
        if (drone_pids[i] > 0) waitpid(drone_pids[i], NULL, 0);
    }
    if (op_pid > 0) waitpid(op_pid, NULL, 0);

    free(drone_pids);
    printf("[Commander] Shutdown complete.\n");
    return 0;
}
