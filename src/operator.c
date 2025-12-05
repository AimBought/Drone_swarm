/* src/operator.c*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/sem.h>
#include <sys/types.h>

#include "common.h"

#define CHANNELS 2
#define DIR_NONE 0
#define DIR_IN   1
#define DIR_OUT  2

#define CHECK_INTERVAL 5 

int chan_dir[CHANNELS];
int chan_users[CHANNELS];

#define WAITQ_CAP 1024
int waitq[2][WAITQ_CAP];
int q_head[2] = {0, 0};
int q_tail[2] = {0, 0};

static int msqid = -1;
static int semid = -1;
static volatile sig_atomic_t keep_running = 1;

// --- Zmienne populacji ---
static int initial_N = 0;        // Pocz¹tkowe N (zapamiêtane)
static volatile int target_N = 0;// Cel (mo¿e byæ zmieniany przez sygna³y!)
static int current_active = 0;   
static int next_drone_id = 0;    

// --- NOWOŒÆ: Obs³uga Sygna³u 1 (SIGUSR1) ---
void sigusr1_handler(int sig) {
    (void)sig;
    // Zwiêkszamy limit do 2 * pocz¹tkowe N
    int new_target = 2 * initial_N;
    
    if (target_N < new_target) {
        target_N = new_target;
        // Wypisujemy bezpoœrednio (uwaga: printf w handlerze to ryzyko, ale w prostym zadaniu OK)
        // W "sztywnym" kodzie produkcyjnym ustawialibyœmy tylko flagê.
        const char *msg = "\n[Operator] SIGNAL 1 RECEIVED: Target N increased to 2*N!\n";
        write(STDOUT_FILENO, msg, strlen(msg));
    }
}

// Obs³uga SIGCHLD
void sigchld_handler(int sig) {
    (void)sig;
    int saved_errno = errno;
    while (waitpid(-1, NULL, WNOHANG) > 0);
    errno = saved_errno;
}

void cleanup(int sig) { (void)sig; keep_running = 0; }

// --- Funkcje pomocnicze (bez zmian) ---
void enqueue(int type, int id) {
    int next = (q_tail[type] + 1) % WAITQ_CAP;
    if (next == q_head[type]) return;
    waitq[type][q_tail[type]] = id;
    q_tail[type] = next;
}
int dequeue(int type) {
    while (q_head[type] != q_tail[type]) {
        int id = waitq[type][q_head[type]];
        q_head[type] = (q_head[type] + 1) % WAITQ_CAP;
        if (id != -1) return id;
    }
    return -1;
}
void remove_dead(int id) {
    for (int t = 0; t < 2; t++) {
        int i = q_head[t];
        while (i != q_tail[t]) {
            if (waitq[t][i] == id) {
                waitq[t][i] = -1;
                printf("[Operator] Removed dead drone %d from queue %s\n", id, (t==0?"LAND":"TAKEOFF"));
            }
            i = (i + 1) % WAITQ_CAP;
        }
    }
}
int reserve_hangar_spot() {
    struct sembuf op = {0, -1, IPC_NOWAIT};
    return (semop(semid, &op, 1) != -1);
}
void free_hangar_spot() {
    struct sembuf op = {0, 1, 0};
    semop(semid, &op, 1);
}
int get_hangar_free_slots() {
    return semctl(semid, 0, GETVAL);
}
int find_available_channel(int needed_dir) {
    // Wersja zach³anna
    for (int i = 0; i < CHANNELS; i++) {
        if (chan_dir[i] == needed_dir) return i;
    }
    for (int i = 0; i < CHANNELS; i++) {
        if (chan_dir[i] == DIR_NONE) return i;
    }
    return -1; 
}
void send_grant(int id, int channel) {
    struct msg_resp resp;
    resp.mtype = RESPONSE_BASE + id;
    resp.channel_id = channel;
    msgsnd(msqid, &resp, sizeof(resp) - sizeof(long), 0);
}
void process_queues() {
    // 1. Wylot
    int cid_out = find_available_channel(DIR_OUT);
    if (cid_out != -1) {
        int id = dequeue(1);
        if (id != -1) {
            chan_dir[cid_out] = DIR_OUT;
            chan_users[cid_out]++;
            send_grant(id, cid_out);
            printf("[Operator] GRANT TAKEOFF drone %d via Channel %d\n", id, cid_out);
        }
    }
    // 2. Wlot
    if (get_hangar_free_slots() > 0) {
        int cid_in = find_available_channel(DIR_IN);
        if (cid_in != -1) {
            int id = dequeue(0);
            if (id != -1) {
                if (reserve_hangar_spot()) {
                    chan_dir[cid_in] = DIR_IN;
                    chan_users[cid_in]++;
                    send_grant(id, cid_in);
                    printf("[Operator] GRANT LANDING drone %d via Channel %d\n", id, cid_in);
                } else {
                    enqueue(0, id);
                }
            }
        }
    }
}
void spawn_new_drone() {
    int new_id = next_drone_id++;
    pid_t pid = fork();
    if (pid == 0) {
        char idstr[16];
        snprintf(idstr, sizeof(idstr), "%d", new_id);
        execl("./drone", "drone", idstr, NULL);
        perror("[Operator] execl drone failed");
        exit(1);
    } else if (pid > 0) {
        printf("[Operator] REPLENISH: Spawned new drone %d (pid %d)\n", new_id, pid);
        current_active++;
    }
}

int main(int argc, char *argv[]) {
    if (argc < 3) return 1;
    int P = atoi(argv[1]);
    target_N = atoi(argv[2]);
    initial_N = target_N; // Zapamiêtujemy stan pocz¹tkowy
    
    current_active = target_N;
    next_drone_id = target_N; 

    signal(SIGINT, cleanup);
    signal(SIGCHLD, sigchld_handler);
    
    // REJESTRACJA SYGNA£U 1
    signal(SIGUSR1, sigusr1_handler);

    msqid = msgget(MSGQ_KEY, IPC_CREAT | 0666);
    semid = semget(SEM_KEY, 1, IPC_CREAT | 0666);
    
    union semun { int val; struct semid_ds *buf; unsigned short *array; } arg;
    arg.val = P;
    semctl(semid, 0, SETVAL, arg);

    for(int i=0; i<CHANNELS; i++) { chan_dir[i]=DIR_NONE; chan_users[i]=0; }

    printf("[Operator] Ready. P=%d, Target N=%d.\n", P, target_N);

    time_t last_check = time(NULL);

    while (keep_running) {
        time_t now = time(NULL);
        if (now - last_check >= CHECK_INTERVAL) {
            last_check = now;
            
            // Fix na martwe semafory
            if (current_active == 0 && get_hangar_free_slots() < P) {
                printf("[Operator] CRITICAL: Zero drones active but Hangar not empty! Resetting semaphore.\n");
                union semun arg;
                arg.val = P;
                semctl(semid, 0, SETVAL, arg);
            }

            // --- TUTAJ OPERATOR ZOBACZY ZMIANÊ target_N ---
            if (current_active < target_N) {
                int needed = target_N - current_active;
                int free_slots = get_hangar_free_slots();
                
                if (free_slots > 0) {
                    printf("[Operator] CHECK: Population %d/%d. Base slots %d. Spawning...\n", 
                           current_active, target_N, free_slots);
                    int to_spawn = (needed < free_slots) ? needed : free_slots;
                    for (int k=0; k<to_spawn; k++) spawn_new_drone();
                } else {
                    printf("[Operator] CHECK: Low population (%d/%d) but Base FULL. Waiting.\n", 
                           current_active, target_N);
                }
            }
        }

        struct msg_req req;
        ssize_t r = msgrcv(msqid, &req, sizeof(req) - sizeof(long), -MSG_DEAD, IPC_NOWAIT);
        
        if (r == -1) {
            if (errno == ENOMSG) {
                usleep(50000); 
                continue;
            }
            if (errno == EINTR) continue;
            break;
        }

        int did = req.drone_id;

        switch (req.mtype) {
            case MSG_REQ_LAND:
                if (get_hangar_free_slots() > 0) {
                    int ch = find_available_channel(DIR_IN);
                    if (ch != -1 && reserve_hangar_spot()) {
                        chan_dir[ch] = DIR_IN;
                        chan_users[ch]++;
                        send_grant(did, ch);
                        printf("[Operator] GRANT LANDING drone %d via Ch %d\n", did, ch);
                    } else enqueue(0, did);
                } else enqueue(0, did);
                break;

            case MSG_REQ_TAKEOFF:
                {
                    int ch = find_available_channel(DIR_OUT);
                    if (ch != -1) {
                        chan_dir[ch] = DIR_OUT;
                        chan_users[ch]++;
                        send_grant(did, ch);
                        printf("[Operator] GRANT TAKEOFF drone %d via Ch %d\n", did, ch);
                    } else enqueue(1, did);
                }
                break;

            case MSG_LANDED:
                {
                    int found = 0;
                    for(int i=0; i<CHANNELS; i++) {
                        if (chan_dir[i] == DIR_IN && chan_users[i] > 0) {
                            chan_users[i]--;
                            if (chan_users[i] == 0) chan_dir[i] = DIR_NONE;
                            printf("[Operator] Drone %d entered base. Ch %d free users=%d\n", did, i, chan_users[i]);
                            found = 1;
                            break;
                        }
                    }
                    // POPRAWKA: U¿ywamy zmiennej found do logowania b³êdu
                    if (!found) {
                        printf("[Operator] WARNING: Got MSG_LANDED from %d but no channel was IN!\n", did);
                    }
                    
                    process_queues();
                }
                break;

            case MSG_DEPARTED:
                {
                    int found = 0;
                    for(int i=0; i<CHANNELS; i++) {
                        if (chan_dir[i] == DIR_OUT && chan_users[i] > 0) {
                            chan_users[i]--;
                            if (chan_users[i] == 0) chan_dir[i] = DIR_NONE;
                            printf("[Operator] Drone %d left system. Ch %d free users=%d\n", did, i, chan_users[i]);
                            found = 1;
                            break;
                        }
                    }
                    if (!found) printf("[Operator] ERROR: Got MSG_DEPARTED but no channel active OUT!\n");
                    free_hangar_spot();
                    process_queues();
                }
                break;

            case MSG_DEAD:
                printf("[Operator] RIP drone %d.\n", did);
                remove_dead(did);
                current_active--; 
                printf("[Operator] Active drones: %d/%d\n", current_active, target_N);
                break;
        }
    }

    if (msqid != -1) msgctl(msqid, IPC_RMID, NULL);
    if (semid != -1) semctl(semid, 0, IPC_RMID);
    return 0;
}