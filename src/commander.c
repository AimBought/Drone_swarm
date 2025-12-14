/* src/commander.c
 *
 * Modu³ Dowódcy (G³ówny proces).
 * Odpowiada za:
 * - Walidacjê danych wejœciowych (P < N/2)
 * - Inicjalizacjê struktur IPC
 * - Uruchomienie Operatora i pocz¹tkowych Dronów
 * - Interfejs u¿ytkownika (komendy 1, 2, 3)
 * - Generowanie raportu koñcowego
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <sys/shm.h>
#include <sys/resource.h>
#include <string.h>
#include <stdarg.h> 
#include <time.h>   
#include <errno.h>

#include "common.h"

// --- ZMIENNE GLOBALNE ---
static pid_t op_pid = -1;
static int N_val = 0;
static volatile sig_atomic_t stop_requested = 0;
static struct SharedState *shared_mem = NULL;
static int shmid = -1;

void sigint_handler(int sig) {
    (void)sig;
    stop_requested = 1;
}

// Wrapper logowania
void cmd_log(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);

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

// Generowanie statystyk na podstawie logów operatora
void generate_report() {
    FILE *f = fopen("operator.txt", "r");
    if (!f) {
        cmd_log(C_RED "\n[Commander] Could not open operator.txt for reporting." C_RESET "\n");
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

    cmd_log(C_YELLOW "\n");
    cmd_log("========================================\n");
    cmd_log("       FINAL SIMULATION REPORT          \n");
    cmd_log("========================================\n");
    cmd_log(" Total Landings Granted:      %d\n", landings);
    cmd_log(" Total Takeoffs Granted:      %d\n", takeoffs);
    cmd_log(" Total Drone Deaths (RIP):    %d\n", deaths);
    cmd_log(" New Drones Spawned:          %d\n", spawns);
    cmd_log(" Entry Denials (Blocked):     %d\n", blocked);
    cmd_log("========================================" C_RESET "\n");
}

// Funkcja pomocnicza do bezpiecznej konwersji string -> int
int parse_int(const char *str, const char *name) {
    char *endptr;
    errno = 0;
    long val = strtol(str, &endptr, 10);

    if (errno != 0 || *endptr != '\0' || val <= 0) {
        fprintf(stderr, C_RED "Error: Invalid value for %s: '%s'. Must be a positive integer." C_RESET "\n", name, str);
        return -1;
    }
    return (int)val;
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <P> <N>\n", argv[0]);
        return 1;
    }

    // 1. Walidacja czy to w ogóle liczby (strtol)
    int P = parse_int(argv[1], "P");
    int N = parse_int(argv[2], "N");
    if (P == -1 || N == -1) return 1;

    // --- WALIDACJA DANYCH ---
    if (P <= 0 || N <= 0) {
        fprintf(stderr, "Error: P and N must be positive integers.\n");
        return 1;
    }
    if (2 * P >= N) {
        fprintf(stderr, "Error: Invalid parameters!\n");
        fprintf(stderr, "Condition P < N/2 is NOT met.\n");
        return 1;
    }

    // Walidacja limitów pamiêci wspó³dzielonej
    if (N > MAX_DRONE_ID) {
        fprintf(stderr, C_RED "Error: N exceeds MAX_DRONE_ID (%d).\n" C_RESET, MAX_DRONE_ID);
        return 1;
    }

    // Walidacja limitów systemowych (ulimit)
    struct rlimit limit;
    if (getrlimit(RLIMIT_NPROC, &limit) == 0) {
        // Liczymy potrzebne procesy: N (drony) + 1 (operator) + 1 (commander - my) + zapas na system
        int needed = N + 5; 
        if (needed > (int)limit.rlim_cur) {
            fprintf(stderr, C_RED "Error: Requested N=%d exceeds system process limit.\n", N);
            fprintf(stderr, "Your limit is %lu. Try a smaller N.\n" C_RESET, (unsigned long)limit.rlim_cur);
            return 1;
        }
    }

    N_val = N;
    
    FILE *f = fopen("commander.txt", "w"); if(f) fclose(f);

    // Utworzenie Pamiêci Dzielonej (do mapowania ID -> PID)
    shmid = shmget(SHM_KEY, sizeof(struct SharedState), IPC_CREAT | 0666);
    if (shmid == -1) { perror("shmget"); return 1; }
    
    shared_mem = (struct SharedState *)shmat(shmid, NULL, 0);
    if (shared_mem == (void *)-1) { perror("shmat"); return 1; }
    memset(shared_mem, 0, sizeof(struct SharedState));
    
    cmd_log(C_BLUE "[Commander] Shared Memory created." C_RESET "\n");

    signal(SIGINT, sigint_handler);

    // Uruchomienie Operatora
    op_pid = fork();
    if (op_pid == -1) { perror("fork operator"); return 1; }
    if (op_pid == 0) {
        char argP[16], argN[16];
        snprintf(argP, sizeof(argP), "%d", P);
        snprintf(argN, sizeof(argN), "%d", N);
        execl("./operator", "operator", argP, argN, NULL);
        perror("execl operator");
        exit(1);
    }
    
    sleep(1);

    // Uruchomienie pocz¹tkowych Dronów (Start z powietrza: arg "0")
    for (int i = 0; i < N; ++i) {
        pid_t pid = fork();
        if (pid == -1) { perror("fork drone"); break; }
        if (pid == 0) {
            char idstr[16];
            snprintf(idstr, sizeof(idstr), "%d", i);
            execl("./drone", "drone", idstr, "0", NULL);
            perror("execl drone");
            exit(1);
        }
        if (i < MAX_DRONE_ID) shared_mem->drone_pids[i] = pid;
    }

    cmd_log(C_GREEN "[Commander] Launched P=%d, N=%d. Monitoring..." C_RESET "\n", P, N);
    cmd_log(C_BLUE "[Commander] Commands: '1'=Grow, '2'=Shrink, '3'=Attack, Ctrl+C=Exit" C_RESET "\n");

    // G³ówna pêtla steruj¹ca (Non-blocking input)
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
                    cmd_log(C_MAGENTA "[Commander] Sending SIGUSR1 (Grow)..." C_RESET "\n");
                    if (kill(op_pid, SIGUSR1) == -1) perror("kill USR1"); // <-- DODANO
                } 
                else if (buffer[0] == '2') {
                    cmd_log(C_MAGENTA "[Commander] Sending SIGUSR2 (Shrink)..." C_RESET "\n");
                    if (kill(op_pid, SIGUSR2) == -1) perror("kill USR2"); // <-- DODANO
                }
                else if (buffer[0] == '3') {
                    printf("\n" C_RED "[Commander] ENTER TARGET DRONE ID: " C_RESET);
                    int target_id = -1;
                    if (scanf("%d", &target_id) == 1) {
                        if (target_id >= 0 && target_id < MAX_DRONE_ID) {
                            pid_t target_pid = shared_mem->drone_pids[target_id];
                            if (target_pid > 0) {
                                cmd_log(C_RED "[Commander] Targeting Drone %d (PID %d). Sending SIGUSR1..." C_RESET "\n", target_id, target_pid);
                                if (kill(target_pid, SIGUSR1) == -1) perror("kill USR1 drone"); // <-- DODANO
                            } else {
                                cmd_log("[Commander] Drone %d not active.\n", target_id);
                            }
                        }
                    }
                    while (getchar() != '\n'); 
                }
            }
        }

        // Sprawdzenie czy operator ¿yje
        int status;
        pid_t res = waitpid(-1, &status, WNOHANG);
        if (res > 0) {
            if (res == op_pid) {
                cmd_log(C_RED "[Commander] Operator died unexpectedly!" C_RESET "\n");
                stop_requested = 1;
            }
        }
    }

    // --- CLEANUP ---
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