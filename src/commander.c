/* src/commander.c */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <sys/shm.h>
#include <string.h>
#include <stdarg.h> 
#include <time.h>   
#include <errno.h>

#include "common.h"

static pid_t op_pid = -1;
static int N_val = 0;
static volatile sig_atomic_t stop_requested = 0;
static struct SharedState *shared_mem = NULL;
static int shmid = -1;

void sigint_handler(int sig) {
    (void)sig;
    stop_requested = 1;
}

// Funkcja loguj¹ca
void cmd_log(const char *format, ...) {
    va_list args;
    
    // 1. Ekran
    va_start(args, format);
    vprintf(format, args);
    va_end(args);

    // 2. Plik
    FILE *f = fopen("commander.txt", "a");
    if (f) {
        time_t now = time(NULL);
        struct tm *t = localtime(&now);
        char timebuf[32];
        strftime(timebuf, sizeof(timebuf), "%H:%M:%S", t);
        fprintf(f, "[%s] ", timebuf);
        
        va_start(args, format);
        vfprintf(f, format, args);
        va_end(args);
        fclose(f);
    }
}

// Funkcja generuj¹ca raport statystyczny
void generate_report() {
    FILE *f = fopen("operator.txt", "r");
    if (!f) {
        cmd_log("\n[Commander] Could not open operator.txt for reporting.\n");
        return;
    }

    int landings = 0;
    int takeoffs = 0;
    int deaths = 0;
    int spawns = 0;
    int blocked = 0;
    
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "GRANT LAND")) landings++;
        if (strstr(line, "GRANT TAKEOFF")) takeoffs++;
        if (strstr(line, "RIP")) deaths++;
        if (strstr(line, "REPLENISH")) spawns++;
        if (strstr(line, "BLOCKED")) blocked++;
    }
    fclose(f);

    cmd_log("\n");
    cmd_log("========================================\n");
    cmd_log("       FINAL SIMULATION REPORT          \n");
    cmd_log("========================================\n");
    cmd_log(" Total Landings Granted:      %d\n", landings);
    cmd_log(" Total Takeoffs Granted:      %d\n", takeoffs);
    cmd_log(" Total Drone Deaths (RIP):    %d\n", deaths);
    cmd_log(" New Drones Spawned:          %d\n", spawns);
    cmd_log(" Entry Denials (Blocked):     %d\n", blocked);
    cmd_log("========================================\n");
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <P> <N>\n", argv[0]);
        return 1;
    }

    int P = atoi(argv[1]);
    int N = atoi(argv[2]);

    // --- WALIDACJA DANYCH WEJŒCIOWYCH ---
    if (P <= 0 || N <= 0) {
        fprintf(stderr, "Error: P and N must be positive integers.\n");
        return 1;
    }

    // Warunek: P < N/2  <=>  2*P < N
    if (2 * P >= N) {
        fprintf(stderr, "Error: Invalid parameters!\n");
        fprintf(stderr, "Condition P < N/2 is NOT met.\n");
        fprintf(stderr, "  P=%d, N=%d -> Max allowed P for N=%d is %d.\n", 
                P, N, N, (N % 2 == 0) ? (N/2 - 1) : (N/2));
        return 1;
    }
    // --------------------------------------------

    N_val = N;
    
    // Reset loga
    FILE *f = fopen("commander.txt", "w"); if(f) fclose(f);

    shmid = shmget(SHM_KEY, sizeof(struct SharedState), IPC_CREAT | 0666);
    if (shmid == -1) { perror("shmget"); return 1; }
    
    shared_mem = (struct SharedState *)shmat(shmid, NULL, 0);
    if (shared_mem == (void *)-1) { perror("shmat"); return 1; }
    memset(shared_mem, 0, sizeof(struct SharedState));
    
    cmd_log("[Commander] Shared Memory created.\n");

    signal(SIGINT, sigint_handler);

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

    for (int i = 0; i < N; ++i) {
        pid_t pid = fork();
        if (pid == 0) {
            char idstr[16];
            snprintf(idstr, sizeof(idstr), "%d", i);
            
            // Argument "0" oznacza start w powietrzu (standard)
            execl("./drone", "drone", idstr, "0", NULL);
            
            perror("execl drone");
            exit(1);
        }
        if (i < MAX_DRONE_ID) shared_mem->drone_pids[i] = pid;
    }

    cmd_log("[Commander] Launched P=%d, N=%d. Monitoring...\n", P, N);
    cmd_log("[Commander] Commands: '1'=Grow, '2'=Shrink, '3'=Attack, Ctrl+C=Exit\n");

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
                    cmd_log("[Commander] Sending SIGUSR1 (Grow)...\n");
                    kill(op_pid, SIGUSR1);
                } 
                else if (buffer[0] == '2') {
                    cmd_log("[Commander] Sending SIGUSR2 (Shrink)...\n");
                    kill(op_pid, SIGUSR2);
                }
                else if (buffer[0] == '3') {
                    printf("\n[Commander] ENTER TARGET DRONE ID: ");
                    int target_id = -1;
                    if (scanf("%d", &target_id) == 1) {
                        if (target_id >= 0 && target_id < MAX_DRONE_ID) {
                            pid_t target_pid = shared_mem->drone_pids[target_id];
                            if (target_pid > 0) {
                                cmd_log("[Commander] Targeting Drone %d (PID %d). Sending SIGUSR1...\n", target_id, target_pid);
                                kill(target_pid, SIGUSR1);
                            } else {
                                cmd_log("[Commander] Drone %d not active.\n", target_id);
                            }
                        }
                    }
                    while (getchar() != '\n'); 
                }
            }
        }

        int status;
        pid_t res = waitpid(-1, &status, WNOHANG);
        if (res > 0) {
            if (res == op_pid) {
                cmd_log("[Commander] Operator died unexpectedly!\n");
                stop_requested = 1;
            }
        }
    }

    cmd_log("\n[Commander] Stopping...\n");
    if (op_pid > 0) kill(op_pid, SIGINT);
    
    for(int i=0; i<MAX_DRONE_ID; i++) {
        if (shared_mem->drone_pids[i] > 0) kill(shared_mem->drone_pids[i], SIGINT);
    }
    
    waitpid(op_pid, NULL, 0);

    generate_report(); 

    shmdt(shared_mem);
    shmctl(shmid, IPC_RMID, NULL);
    cmd_log("[Commander] Cleanup complete. Bye.\n");
    return 0;
}