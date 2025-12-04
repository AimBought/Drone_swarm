/* src/commander.c - ETAP 5: Commander przekazuje N i dzia³a ci¹gle */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <errno.h>

static pid_t op_pid = -1;
static pid_t *drone_pids = NULL;
static int N_val = 0;
static volatile sig_atomic_t stop_requested = 0;

void sigint_handler(int sig) {
    (void)sig;
    stop_requested = 1;
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <P> <N>\n", argv[0]);
        return 1;
    }

    int P = atoi(argv[1]);
    int N = atoi(argv[2]);
    N_val = N;

    drone_pids = calloc(N, sizeof(pid_t));

    signal(SIGINT, sigint_handler);

    // 1. Uruchom operatora
    // NOWOŒÆ: Przekazujemy P oraz N operatorowi!
    op_pid = fork();
    if (op_pid == 0) {
        char argP[16], argN[16];
        snprintf(argP, sizeof(argP), "%d", P);
        snprintf(argN, sizeof(argN), "%d", N);
        
        execl("./operator", "operator", argP, argN, NULL);
        perror("execl operator");
        exit(1);
    }
    
    sleep(1);

    // 2. Uruchom pocz¹tkowe drony
    for (int i = 0; i < N; ++i) {
        pid_t pid = fork();
        if (pid == 0) {
            char idstr[16];
            snprintf(idstr, sizeof(idstr), "%d", i);
            execl("./drone", "drone", idstr, NULL);
            perror("execl drone");
            exit(1);
        }
        drone_pids[i] = pid;
    }

    printf("[Commander] Launched P=%d, N=%d. Monitoring...\n", P, N);

    // 3. Pêtla nadzorcza
    // ZMIANA: Nie koñczymy, gdy active_drones == 0. Czekamy na Ctrl+C.
    // Operator bêdzie dba³ o podtrzymanie gatunku.
    while (!stop_requested) {
        int status;
        // Commander widzi tylko œmieræ swoich BEZPOŒREDNICH dzieci (pierwszej generacji)
        pid_t res = waitpid(-1, &status, WNOHANG);
        
        if (res > 0) {
            if (res == op_pid) {
                printf("[Commander] Operator died unexpectedly! Exiting.\n");
                stop_requested = 1;
            } else {
                // Logujemy tylko dla informacji
                for(int i=0; i<N_val; i++) {
                    if (drone_pids[i] == res) {
                        printf("[Commander] Initial Drone %d (pid %d) finished.\n", i, res);
                        drone_pids[i] = 0; 
                        break;
                    }
                }
            }
        }
        
        sleep(1);
    }

    printf("\n[Commander] Simulation stopping...\n");

    if (op_pid > 0) kill(op_pid, SIGINT);
    // Zabijamy pierwsz¹ generacjê (jeœli jeszcze ¿yje)
    for (int i = 0; i < N_val; ++i) {
        if (drone_pids[i] > 0) kill(drone_pids[i], SIGINT);
    }
    
    waitpid(op_pid, NULL, 0);
    free(drone_pids);
    
    printf("[Commander] Shutdown complete.\n");
    return 0;
}