/* src/commander.c */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <sys/shm.h>
#include <string.h>
#include <errno.h>

#include "common.h" // Potrzebne dla SharedState i kluczy

static pid_t op_pid = -1;
static int N_val = 0;
static volatile sig_atomic_t stop_requested = 0;

// WskaŸnik do pamiêci dzielonej
static struct SharedState *shared_mem = NULL;
static int shmid = -1;

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

    // --- 1. Inicjalizacja Pamiêci Dzielonej ---
    // Tworzymy segment pamiêci o wielkoœci naszej struktury
    shmid = shmget(SHM_KEY, sizeof(struct SharedState), IPC_CREAT | 0666);
    if (shmid == -1) {
        perror("[Commander] shmget failed");
        return 1;
    }
    
    // Pod³¹czamy pamiêæ do naszego procesu
    shared_mem = (struct SharedState *)shmat(shmid, NULL, 0);
    if (shared_mem == (void *)-1) {
        perror("[Commander] shmat failed");
        return 1;
    }
    
    // Czyœcimy pamiêæ (same zera)
    memset(shared_mem, 0, sizeof(struct SharedState));
    printf("[Commander] Shared Memory created and attached.\n");
    // ------------------------------------------

    signal(SIGINT, sigint_handler);

    // Uruchomienie Operatora
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

    // Uruchomienie Dronów
    for (int i = 0; i < N; ++i) {
        pid_t pid = fork();
        if (pid == 0) {
            char idstr[16];
            snprintf(idstr, sizeof(idstr), "%d", i);
            execl("./drone", "drone", idstr, NULL);
            perror("execl drone");
            exit(1);
        }
        // ZAPISUJEMY PID DO PAMIÊCI DZIELONEJ
        if (i < MAX_DRONE_ID) {
            shared_mem->drone_pids[i] = pid;
        }
    }

    printf("[Commander] Launched P=%d, N=%d. Monitoring...\n", P, N);
    printf("[Commander] Commands: '1'=Grow, '2'=Shrink, '3'=Attack, Ctrl+C=Exit\n");

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
                    printf("[Commander] Sending SIGUSR1 (Grow)...\n");
                    kill(op_pid, SIGUSR1);
                } 
                else if (buffer[0] == '2') {
                    printf("[Commander] Sending SIGUSR2 (Shrink)...\n");
                    kill(op_pid, SIGUSR2);
                }
                else if (buffer[0] == '3') {
                    printf("\n[Commander] ENTER TARGET DRONE ID: ");
                    int target_id = -1;
                    if (scanf("%d", &target_id) == 1) {
                        if (target_id >= 0 && target_id < MAX_DRONE_ID) {
                            // CZYTAMY PID Z PAMIÊCI DZIELONEJ
                            pid_t target_pid = shared_mem->drone_pids[target_id];
                            
                            if (target_pid > 0) {
                                printf("[Commander] Targeting Drone %d (PID %d). Sending SIGUSR1...\n", target_id, target_pid);
                                // SprawdŸmy czy proces istnieje (kill 0 sprawdza)
                                if (kill(target_pid, 0) == 0) {
                                    kill(target_pid, SIGUSR1);
                                } else {
                                    printf("[Commander] Process %d not found (already dead?). Updating registry.\n", target_pid);
                                    shared_mem->drone_pids[target_id] = 0;
                                }
                            } else {
                                printf("[Commander] Drone %d is dead or not spawned yet.\n", target_id);
                            }
                        } else {
                            printf("[Commander] Invalid ID range.\n");
                        }
                    }
                    while (getchar() != '\n'); // flush input
                }
            }
        }

        int status;
        pid_t res = waitpid(-1, &status, WNOHANG);
        if (res > 0) {
            if (res == op_pid) {
                printf("[Commander] Operator died unexpectedly!\n");
                stop_requested = 1;
            }
            // Nie musimy tu czyœciæ Pamiêci Dzielonej, bo Operator to zrobi po otrzymaniu MSG_DEAD
            // Ale dla pewnoœci mo¿emy:
            // (Problem: Commander nie zna ID po PIDzie bez przeszukania tablicy. 
            // Zostawmy czyszczenie Operatorowi, on wie kto umar³ po ID).
        }
    }

    printf("\n[Commander] Stopping...\n");
    if (op_pid > 0) kill(op_pid, SIGINT);
    
    // Zabijamy wszystkich z listy (równie¿ tych nowych!)
    for(int i=0; i<MAX_DRONE_ID; i++) {
        if (shared_mem->drone_pids[i] > 0) {
             kill(shared_mem->drone_pids[i], SIGINT);
        }
    }
    
    waitpid(op_pid, NULL, 0);

    // Od³¹czenie i usuniêcie pamiêci dzielonej
    shmdt(shared_mem);
    shmctl(shmid, IPC_RMID, NULL);
    printf("[Commander] Shared Memory removed. Bye.\n");
    return 0;
}