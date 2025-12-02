/* src/commander.c*/
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <errno.h>

static pid_t op_pid = -1;
static pid_t *drone_pids = NULL;
static int drone_count = 0;
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

    drone_pids = calloc(N, sizeof(pid_t));
    drone_count = N;

    signal(SIGINT, sigint_handler);

    // 1. Uruchom operatora
    op_pid = fork();
    if (op_pid == 0) {
        char argP[16];
        snprintf(argP, sizeof(argP), "%d", P);
        
        execl("./operator", "operator", argP, NULL);
        
        perror("execl operator");
        exit(1);
    }
    
    sleep(1);

    // 2. Uruchom drony
    for (int i = 0; i < N; ++i) {
        pid_t pid = fork();
        if (pid == 0) {
            char idstr[16];
            snprintf(idstr, sizeof(idstr), "%d", i);
            
            // POPRAWKA: U¿ywamy ./drone zamiast bin/drone
            execl("./drone", "drone", idstr, NULL);
            
            perror("execl drone");
            exit(1);
        }
        drone_pids[i] = pid;
    }

    printf("[Commander] All processes launched. Monitoring...\n");

    int active_drones = N;
    
    // 3. Pêtla nadzorcza
    while (!stop_requested && active_drones > 0) {
        int status;
        // WNOHANG = nie czekaj, tylko sprawdŸ czy ktoœ skoñczy³
        pid_t res = waitpid(-1, &status, WNOHANG);
        
        if (res > 0) {
            if (res == op_pid) {
                printf("[Commander] Operator died unexpectedly!\n");
                stop_requested = 1;
            } else {
                for(int i=0; i<N; ++i) {
                    if (drone_pids[i] == res) {
                        printf("[Commander] Drone %d (pid %d) finished/died.\n", i, res);
                        drone_pids[i] = 0; 
                        active_drones--;
                        break;
                    }
                }
            }
        }
        
        sleep(1);
    }

    printf("\n[Commander] Simulation finished (Active drones: %d). Cleaning up.\n", active_drones);

    if (op_pid > 0) kill(op_pid, SIGINT);
    for (int i = 0; i < N; ++i) {
        if (drone_pids[i] > 0) kill(drone_pids[i], SIGINT);
    }
    
    waitpid(op_pid, NULL, 0);

    free(drone_pids);
    return 0;
} 