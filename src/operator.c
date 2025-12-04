/* src/operator.c - ETAP 5: Reprodukcja */

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

// Co ile sekund Operator sprawdza stan roju
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

// --- Zmienne do zarz¹dzania populacj¹ ---
static int target_N = 0;         // Docelowa liczba dronów
static int current_active = 0;   // Aktualna liczba
static int next_drone_id = 0;    // ID dla nastêpnego nowego drona

// Obs³uga sygna³u SIGCHLD - sprz¹tanie po w³asnych dzieciach (nowych dronach)
void sigchld_handler(int sig) {
    (void)sig;
    int saved_errno = errno;
    while (waitpid(-1, NULL, WNOHANG) > 0);
    errno = saved_errno;
}

void cleanup(int sig) { (void)sig; keep_running = 0; }

// --- Funkcje kolejek i kana³ów (bez zmian logicznych) ---
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

// --- Zarz¹dzanie Kana³ami ---
// Zwraca ID kana³u (0 lub 1) jeœli dostêpny dla danego kierunku, inaczej -1
int find_available_channel(int needed_dir) {
    // Strategia: szukaj kana³u, który ju¿ ma ten kierunek, albo jest pusty
    // Priorytet dla kana³u, który ju¿ obs³uguje ten ruch (¿eby nie blokowaæ obu naraz zmian¹ kierunku)
    
    // 1. SprawdŸ czy jest kana³ ju¿ ustawiony na ten kierunek
    for (int i = 0; i < CHANNELS; i++) {
        if (chan_dir[i] == needed_dir) return i;
    }
    
    // 2. Jeœli nie, weŸ pusty kana³
    for (int i = 0; i < CHANNELS; i++) {
        if (chan_dir[i] == DIR_NONE) return i;
    }
    
    return -1; // Brak wolnych
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

// --- NOWA FUNKCJA: Tworzenie drona ---
void spawn_new_drone() {
    int new_id = next_drone_id++;
    pid_t pid = fork();
    if (pid == 0) {
        // Child
        char idstr[16];
        snprintf(idstr, sizeof(idstr), "%d", new_id);
        execl("./drone", "drone", idstr, NULL);
        perror("[Operator] execl drone failed");
        exit(1);
    } else if (pid > 0) {
        printf("[Operator] REPLENISH: Spawned new drone %d (pid %d)\n", new_id, pid);
        current_active++;
    } else {
        perror("[Operator] fork failed");
    }
}

int main(int argc, char *argv[]) {
    if (argc < 3) return 1; // Wymagamy teraz P i N
    int P = atoi(argv[1]);
    target_N = atoi(argv[2]); // Zapisujemy cel
    
    // Zak³adamy, ¿e Commander stworzy³ pocz¹tkowe N dronów
    current_active = target_N;
    next_drone_id = target_N; // Nowe bêd¹ mia³y ID od N w górê

    signal(SIGINT, cleanup);
    signal(SIGCHLD, sigchld_handler); // Sprz¹tanie zombie

    msqid = msgget(MSGQ_KEY, IPC_CREAT | 0666);
    semid = semget(SEM_KEY, 1, IPC_CREAT | 0666);
    
    union semun { int val; struct semid_ds *buf; unsigned short *array; } arg;
    arg.val = P;
    semctl(semid, 0, SETVAL, arg);

    for(int i=0; i<CHANNELS; i++) { chan_dir[i]=DIR_NONE; chan_users[i]=0; }

    printf("[Operator] Ready. P=%d, Target N=%d.\n", P, target_N);

    time_t last_check = time(NULL);

    while (keep_running) {
        // --- LOGIKA REPRODUKCJI (Co TK) ---
        time_t now = time(NULL);
        if (now - last_check >= CHECK_INTERVAL) {
            last_check = now;
            
            // Jeœli populacja wyginê³a (0)
            if (current_active == 0 && get_hangar_free_slots() < P) {
                printf("[Operator] CRITICAL: Zero drones active but Hangar not empty! Resetting semaphore.\n");
                union semun arg;
                arg.val = P; // Przywracamy pe³n¹ pojemnoœæ
                semctl(semid, 0, SETVAL, arg);
            }

            // Jeœli brakuje nam dronów
            if (current_active < target_N) {
                int needed = target_N - current_active;
                int free_slots = get_hangar_free_slots();
                
                // Warunek z zadania: "jeœli jest miejsce w bazie"
                // Interpretujemy to tak: nie spawnujemy, jeœli baza jest przepe³niona,
                // ¿eby nie robiæ jeszcze wiêkszego t³oku przed wejœciem.
                if (free_slots > 0) {
                    printf("[Operator] CHECK: Population %d/%d. Base slots %d. Spawning...\n", 
                           current_active, target_N, free_slots);
                    // Spawnujemy tyle ile trzeba, ale nie wiêcej ni¿ jest miejsc
                    // (zabezpieczenie, ¿eby nie zalaæ systemu)
                    int to_spawn = (needed < free_slots) ? needed : free_slots;
                    
                    for (int k=0; k<to_spawn; k++) {
                        spawn_new_drone();
                    }
                } else {
                    printf("[Operator] CHECK: Low population (%d/%d) but Base FULL. Waiting.\n", 
                           current_active, target_N);
                }
            }
        }

        // --- Standardowa obs³uga komunikatów ---
        struct msg_req req;
        // Odbieramy wiadomoœæ (z IPC_NOWAIT, ¿eby pêtla mog³a sprawdzaæ czas!)
        // Jeœli u¿yjemy blokuj¹cego msgrcv, to timer nie zadzia³a dopóki ktoœ nic nie przyœle.
        // Dlatego w pêtli z timerem czêsto stosuje siê NOWAIT + krótki sleep (jeœli pusto).
        
        ssize_t r = msgrcv(msqid, &req, sizeof(req) - sizeof(long), -MSG_DEAD, IPC_NOWAIT);
        
        if (r == -1) {
            if (errno == ENOMSG) {
                // Brak wiadomoœci - krótka drzemka, ¿eby nie zajechaæ CPU
                usleep(50000); // 50ms
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
                    } else {
                        enqueue(0, did);
                    }
                } else {
                    enqueue(0, did);
                }
                break;

            case MSG_REQ_TAKEOFF:
                {
                    int ch = find_available_channel(DIR_OUT);
                    if (ch != -1) {
                        chan_dir[ch] = DIR_OUT;
                        chan_users[ch]++;
                        send_grant(did, ch);
                        printf("[Operator] GRANT TAKEOFF drone %d via Ch %d\n", did, ch);
                    } else {
                        enqueue(1, did);
                    }
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
                    if (!found) printf("[Operator] ERROR: Got MSG_LANDED but no channel active IN!\n");
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
                current_active--; // Zmniejszamy licznik populacji!
                printf("[Operator] Active drones: %d/%d\n", current_active, target_N);
                break;
        }
    }

    if (msqid != -1) msgctl(msqid, IPC_RMID, NULL);
    if (semid != -1) semctl(semid, 0, IPC_RMID);
    return 0;
}