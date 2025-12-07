/* src/commander.c */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/select.h>
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

    // 1. Operator
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

    // 2. Drony
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
    printf("[Commander] Commands:\n");
    printf("  '1' + Enter -> Signal 1: Increase Population (2*N)\n");
    printf("  '2' + Enter -> Signal 2: Decrease Population (50%%)\n");
    printf("  '3' + Enter -> Signal 3: SUICIDE ATTACK on specific Drone\n");
    printf("  Ctrl+C      -> Exit\n");

    while (!stop_requested) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        
        struct timeval tv = {1, 0};
        
        int ret = select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);
        
        if (ret > 0 && FD_ISSET(STDIN_FILENO, &fds)) {
            char buffer[128];
            int n = read(STDIN_FILENO, buffer, sizeof(buffer));
            if (n > 0) {
                if (buffer[0] == '1') {
                    printf("[Commander] Sending SIGUSR1 (Grow) to Operator...\n");
                    kill(op_pid, SIGUSR1);
                } 
                else if (buffer[0] == '2') {
                    printf("[Commander] Sending SIGUSR2 (Shrink) to Operator...\n");
                    kill(op_pid, SIGUSR2);
                }
                else if (buffer[0] == '3') {
                    // --- LOGIKA ATAKU ---
                    printf("\n[Commander] ENTER TARGET DRONE ID (0-%d): ", N-1);
                    int target_id = -1;
                    // U¿ywamy scanf, ¿eby pobraæ liczbê. 
                    // To chwilowo zablokuje pêtlê, ale w trybie dowodzenia to OK.
                    if (scanf("%d", &target_id) == 1) {
                        if (target_id >= 0 && target_id < N_val) {
                            pid_t target_pid = drone_pids[target_id];
                            if (target_pid > 0) {
                                printf("[Commander] Sending SIGUSR1 (SUICIDE) to Drone %d (PID %d)...\n", target_id, target_pid);
                                kill(target_pid, SIGUSR1);
                            } else {
                                printf("[Commander] Drone %d is already dead or replaced (unknown PID).\n", target_id);
                            }
                        } else {
                            printf("[Commander] Invalid ID. Only initial drones (0-%d) are targetable.\n", N_val-1);
                        }
                    }
                    // Czyœcimy bufor wejœcia po scanf
                    while (getchar() != '\n');
                }
            }
        }

        int status;
        pid_t res = waitpid(-1, &status, WNOHANG);
        if (res > 0) {
            if (res == op_pid) {
                printf("[Commander] Operator died unexpectedly! Exiting.\n");
                stop_requested = 1;
            } else {
                for(int i=0; i<N_val; i++) {
                    if (drone_pids[i] == res) {
                        drone_pids[i] = 0; 
                        break;
                    }
                }
            }
        }
    }

    printf("\n[Commander] Simulation stopping...\n");
    if (op_pid > 0) kill(op_pid, SIGINT);
    for (int i = 0; i < N_val; ++i) {
        if (drone_pids[i] > 0) kill(drone_pids[i], SIGINT);
    }
    waitpid(op_pid, NULL, 0);
    free(drone_pids);
    printf("[Commander] Shutdown complete.\n");
    return 0;
}