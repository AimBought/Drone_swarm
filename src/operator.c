/* src/operator.c */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/sem.h>
#include <sys/types.h>

#include "common.h"

// Definicje kana³ów
#define CHANNELS 2
#define DIR_NONE 0
#define DIR_IN   1
#define DIR_OUT  2

// Stan kana³ów
int chan_dir[CHANNELS];   // Kierunek: 0, 1 (IN), 2 (OUT)
int chan_users[CHANNELS]; // Liczba dronów w tunelu

// Kolejki oczekuj¹cych
#define WAITQ_CAP 1024
// Mamy dwie kolejki: 0 = L¹dowanie (IN), 1 = Start (OUT)
int waitq[2][WAITQ_CAP];
int q_head[2] = {0, 0};
int q_tail[2] = {0, 0};

static int msqid = -1;
static int semid = -1; // Semafor tylko do pilnowania limitu miejsc w bazie (P)
static volatile sig_atomic_t keep_running = 1;

// --- Obs³uga kolejek FIFO ---
void enqueue(int type, int id) {
    int next = (q_tail[type] + 1) % WAITQ_CAP;
    if (next == q_head[type]) return; // Full
    waitq[type][q_tail[type]] = id;
    q_tail[type] = next;
}

int dequeue(int type) {
    while (q_head[type] != q_tail[type]) {
        int id = waitq[type][q_head[type]];
        q_head[type] = (q_head[type] + 1) % WAITQ_CAP;
        if (id != -1) return id; // Zwróæ ¿ywego
    }
    return -1;
}

// Usuniêcie martwego drona z obu kolejek
void remove_dead(int id) {
    for (int t = 0; t < 2; t++) {
        int i = q_head[t];
        while (i != q_tail[t]) {
            if (waitq[t][i] == id) {
                waitq[t][i] = -1; // Oznacz jako martwy
                printf("[Operator] Removed dead drone %d from queue %s\n", 
                       id, (t==0 ? "LAND" : "TAKEOFF"));
            }
            i = (i + 1) % WAITQ_CAP;
        }
    }
}

// --- Obs³uga SEMAFORA (Pojemnoœæ bazy) ---
// Dec_nowait: zajmij miejsce w hangarze
int reserve_hangar_spot() {
    struct sembuf op = {0, -1, IPC_NOWAIT};
    if (semop(semid, &op, 1) == -1) return 0;
    return 1;
}
// Inc: zwolnij miejsce w hangarze
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

// Próbuje obs³u¿yæ oczekuj¹cych (zarówno l¹duj¹cych jak i startuj¹cych)
void process_queues() {
    // -- OBS£UGA KOLEJKI WYLOTOWEJ (TAKEOFF) --
    int cid_out = find_available_channel(DIR_OUT);
    if (cid_out != -1) {
        int id = dequeue(1); // 1 = TAKEOFF queue
        if (id != -1) {
            chan_dir[cid_out] = DIR_OUT;
            chan_users[cid_out]++;
            send_grant(id, cid_out);
            printf("[Operator] GRANT TAKEOFF drone %d via Channel %d\n", id, cid_out);
        }
    }

    // -- OBS£UGA KOLEJKI WLOTOWEJ (LAND) --
    int slots = get_hangar_free_slots();
    if (slots > 0) {
        int cid_in = find_available_channel(DIR_IN);
        if (cid_in != -1) {
            int id = dequeue(0); // 0 = LAND queue
            if (id != -1) {
                if (reserve_hangar_spot()) {
                    chan_dir[cid_in] = DIR_IN;
                    chan_users[cid_in]++;
                    send_grant(id, cid_in);
                    printf("[Operator] GRANT LANDING drone %d via Channel %d (Slots left: %d)\n", 
                           id, cid_in, get_hangar_free_slots());
                } else {
                    enqueue(0, id); // Wróæ do kolejki
                }
            }
        }
    }
}

void cleanup(int sig) { (void)sig; keep_running = 0; }

int main(int argc, char *argv[]) {
    if (argc < 2) return 1;
    int P = atoi(argv[1]);

    signal(SIGINT, cleanup);

    msqid = msgget(MSGQ_KEY, IPC_CREAT | 0666);
    semid = semget(SEM_KEY, 1, IPC_CREAT | 0666);
    
    union semun { int val; struct semid_ds *buf; unsigned short *array; } arg;
    arg.val = P;
    semctl(semid, 0, SETVAL, arg);

    // Init kana³ów
    for(int i=0; i<CHANNELS; i++) { chan_dir[i]=DIR_NONE; chan_users[i]=0; }

    printf("[Operator] Ready. P=%d. 2 Channels.\n", P);

    while (keep_running) {
        struct msg_req req;
        // Odbieramy wszystko <= MSG_DEAD (czyli 1,2,3,4,5). Omijamy odpowiedzi > 1000.
        ssize_t r = msgrcv(msqid, &req, sizeof(req) - sizeof(long), -MSG_DEAD, 0);
        
        if (r == -1) {
            if (errno == EINTR) continue;
            break;
        }

        int did = req.drone_id;

        switch (req.mtype) {
            case MSG_REQ_LAND:
                // SprawdŸ warunki natychmiast
                if (get_hangar_free_slots() > 0) {
                    int ch = find_available_channel(DIR_IN);
                    if (ch != -1 && reserve_hangar_spot()) {
                        chan_dir[ch] = DIR_IN;
                        chan_users[ch]++;
                        send_grant(did, ch);
                        printf("[Operator] GRANT LANDING drone %d via Ch %d\n", did, ch);
                    } else {
                        // Jest miejsce w bazie, ale brak tunelu -> czekaj
                        enqueue(0, did);
                        printf("[Operator] QUEUE LAND drone %d (Busy/WrongDir)\n", did);
                    }
                } else {
                    // Brak miejsca w bazie
                    enqueue(0, did);
                    printf("[Operator] QUEUE LAND drone %d (Base Full)\n", did);
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
                        enqueue(1, did); // Kolejka wylotowa
                        printf("[Operator] QUEUE TAKEOFF drone %d\n", did);
                    }
                }
                break;

            case MSG_LANDED:
                // Dron wyszed³ z tunelu do bazy
                // Szukamy którym tunelem lecia³? Uproszczenie: po prostu dekrementujemy 
                // kana³ ustawiony na IN, który ma users > 0.
                // W idealnym œwiecie message powinien zawieraæ ID kana³u, ale tu za³o¿ymy automat.
                {
                    int found = 0;
                    for(int i=0; i<CHANNELS; i++) {
                        if (chan_dir[i] == DIR_IN && chan_users[i] > 0) {
                            chan_users[i]--;
                            if (chan_users[i] == 0) chan_dir[i] = DIR_NONE; // Reset kierunku
                            printf("[Operator] Drone %d entered base. Ch %d free users=%d\n", did, i, chan_users[i]);
                            found = 1;
                            break;
                        }
                    }
                    if (!found) printf("[Operator] ERROR: Got MSG_LANDED but no channel active IN!\n");
                    process_queues(); // Zwolni³ siê kana³, mo¿e ktoœ wejdzie/wyjdzie
                }
                break;

            case MSG_DEPARTED:
                // Dron wyszed³ z tunelu na zewn¹trz
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
                    
                    // Zwolnij miejsce w bazie
                    free_hangar_spot();
                    printf("[Operator] Hangar spot freed. Slots: %d\n", get_hangar_free_slots());
                    
                    process_queues(); // Zwolni³o siê miejsce i kana³
                }
                break;

            case MSG_DEAD:
                printf("[Operator] RIP drone %d.\n", did);
                remove_dead(did);
                // Nie zwalniamy semafora bazy tutaj, bo nie wiemy gdzie umar³ (w powietrzu czy w bazie).
                // Uproszczenie symulacji: zak³adamy ¿e umar³ w powietrzu (w kolejce land), wiêc nie zajmowa³ miejsca.
                break;
        }
    }

    if (msqid != -1) msgctl(msqid, IPC_RMID, NULL);
    if (semid != -1) semctl(semid, 0, IPC_RMID);
    return 0;
}