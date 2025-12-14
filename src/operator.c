/* src/operator.c */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <stdarg.h>
#include <sys/wait.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/sem.h>
#include <sys/shm.h>
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
static int shmid = -1;
static struct SharedState *shared_mem = NULL;
static volatile sig_atomic_t keep_running = 1;

static volatile sig_atomic_t flag_sig1 = 0; 
static volatile sig_atomic_t flag_sig2 = 0; 

static int current_P = 0;        
static int pending_removal = 0;  
static int signal1_used = 0;     

static volatile int target_N = 0;
static int current_active = 0;   
static int next_drone_id = 0;    

void olog(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);

    FILE *f = fopen("operator.txt", "a");
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

void sigusr1_handler(int sig) { (void)sig; flag_sig1 = 1; }
void sigusr2_handler(int sig) { (void)sig; flag_sig2 = 1; }
void sigchld_handler(int sig) {
    (void)sig;
    int saved_errno = errno;
    while (waitpid(-1, NULL, WNOHANG) > 0);
    errno = saved_errno;
}
void cleanup(int sig) { (void)sig; keep_running = 0; }

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
            if (waitq[t][i] == id) waitq[t][i] = -1;
            i = (i + 1) % WAITQ_CAP;
        }
    }
}

int reserve_hangar_spot() {
    struct sembuf op = {0, -1, IPC_NOWAIT};
    return (semop(semid, &op, 1) != -1);
}

void free_hangar_spot() {
    if (pending_removal > 0) {
        pending_removal--;
        olog(C_MAGENTA "[Operator] Platform dismantled after departure. Pending: %d" C_RESET "\n", pending_removal);
    } else {
        struct sembuf op = {0, 1, 0};
        semop(semid, &op, 1);
    }
}

int get_hangar_free_slots() {
    return semctl(semid, 0, GETVAL);
}

void increase_base_capacity() {
    if (signal1_used) {
        olog(C_YELLOW "[Operator] Signal 1 IGNORED (One-time use only)." C_RESET "\n");
        return;
    }
    
    int added_slots = current_P; 
    current_P *= 2;
    target_N *= 2;
    signal1_used = 1;

    struct sembuf op = {0, added_slots, 0};
    if (semop(semid, &op, 1) == -1) {
        perror("semop increase");
    }
    
    olog(C_BLUE "[Operator] !!! BASE EXPANDED !!! New P=%d, New Target N=%d" C_RESET "\n", current_P, target_N);
}

void decrease_base_capacity() {
    if (current_P <= 1) {
        olog(C_YELLOW "[Operator] Signal 2 IGNORED (Minimum P=1 reached)." C_RESET "\n");
        return;
    }

    int remove_cnt = current_P / 2;
    current_P -= remove_cnt;
    
    target_N /= 2;
    if (target_N < 1) target_N = 1;

    int free_slots = get_hangar_free_slots();
    int immediate_remove = (free_slots >= remove_cnt) ? remove_cnt : free_slots;
    int deferred_remove = remove_cnt - immediate_remove;
    
    if (immediate_remove > 0) {
        struct sembuf op = {0, -immediate_remove, IPC_NOWAIT};
        semop(semid, &op, 1);
    }
    
    pending_removal += deferred_remove;

    olog(C_MAGENTA "[Operator] !!! BASE SHRINKING !!! New P=%d, Target N=%d. Removed now: %d, Pending: %d" C_RESET "\n", 
         current_P, target_N, immediate_remove, pending_removal);
}

int find_available_channel(int needed_dir) {
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
    int cid_out = find_available_channel(DIR_OUT);
    if (cid_out != -1) {
        int id = dequeue(1);
        if (id != -1) {
            chan_dir[cid_out] = DIR_OUT; chan_users[cid_out]++; send_grant(id, cid_out);
            olog(C_GREEN "[Operator] GRANT TAKEOFF drone %d via Channel %d" C_RESET "\n", id, cid_out);
        }
    }
    if (current_active > target_N) return; 

    if (get_hangar_free_slots() > 0) {
        int cid_in = find_available_channel(DIR_IN);
        if (cid_in != -1) {
            int id = dequeue(0);
            if (id != -1) {
                if (reserve_hangar_spot()) {
                    chan_dir[cid_in] = DIR_IN; chan_users[cid_in]++; send_grant(id, cid_in);
                    olog(C_GREEN "[Operator] GRANT LAND %d via Ch %d" C_RESET "\n", id, cid_in);
                } else enqueue(0, id);
            }
        }
    }
}

void spawn_new_drone() {
    if (!reserve_hangar_spot()) {
        olog(C_RED "[Operator] ERROR: Tried to spawn drone inside base, but reserve failed!" C_RESET "\n");
        return; 
    }

    int new_id = next_drone_id++;
    pid_t pid = fork();
    if (pid == 0) {
        char idstr[16];
        snprintf(idstr, sizeof(idstr), "%d", new_id);
        execl("./drone", "drone", idstr, "1", NULL);
        perror("[Operator] execl drone failed");
        exit(1);
    } else if (pid > 0) {
        olog(C_BLUE "[Operator] REPLENISH: Spawned drone %d INSIDE BASE (pid %d). Slot occupied." C_RESET "\n", new_id, pid);
        current_active++;
        if (shared_mem != NULL && new_id < MAX_DRONE_ID) {
            shared_mem->drone_pids[new_id] = pid;
        }
    }
}

int main(int argc, char *argv[]) {
    if (argc < 3) return 1;
    int P = atoi(argv[1]);
    target_N = atoi(argv[2]);
    
    current_P = P;
    current_active = target_N;
    next_drone_id = target_N; 
    
    FILE *f = fopen("operator.txt", "w"); if(f) fclose(f);

    signal(SIGINT, cleanup);
    signal(SIGCHLD, sigchld_handler);
    signal(SIGUSR1, sigusr1_handler);
    signal(SIGUSR2, sigusr2_handler);

    msqid = msgget(MSGQ_KEY, IPC_CREAT | 0666);
    semid = semget(SEM_KEY, 1, IPC_CREAT | 0666);
    shmid = shmget(SHM_KEY, sizeof(struct SharedState), 0666);
    if (shmid != -1) {
        shared_mem = (struct SharedState *)shmat(shmid, NULL, 0);
        if (shared_mem == (void *)-1) shared_mem = NULL;
        else olog("[Operator] Attached to Shared Memory.\n");
    }
    
    union semun { int val; struct semid_ds *buf; unsigned short *array; } arg;
    arg.val = P;
    semctl(semid, 0, SETVAL, arg);

    for(int i=0; i<CHANNELS; i++) { chan_dir[i]=DIR_NONE; chan_users[i]=0; }

    olog(C_GREEN "[Operator] Ready. P=%d, Target N=%d." C_RESET "\n", P, target_N);
    time_t last_check = time(NULL);

    while (keep_running) {
        if (flag_sig1) { increase_base_capacity(); flag_sig1 = 0; }
        if (flag_sig2) { decrease_base_capacity(); flag_sig2 = 0; }

        time_t now = time(NULL);
        if (now - last_check >= CHECK_INTERVAL) {
            last_check = now;
            
            if (current_active == 0 && get_hangar_free_slots() < current_P && pending_removal == 0) {
                union semun arg; arg.val = current_P; semctl(semid, 0, SETVAL, arg);
                olog(C_YELLOW "[Operator] Reset semaphore to %d." C_RESET "\n", current_P);
            }
            if (current_active < target_N) {
                int needed = target_N - current_active;
                int free_slots = get_hangar_free_slots();
                if (free_slots > 0) {
                    olog(C_BLUE "[Operator] CHECK: Spawning inside base..." C_RESET "\n");
                    int to_spawn = (needed < free_slots) ? needed : free_slots;
                    for (int k=0; k<to_spawn; k++) spawn_new_drone();
                } 
            }
        }

        struct msg_req req;
        ssize_t r = msgrcv(msqid, &req, sizeof(req) - sizeof(long), -MSG_DEAD, IPC_NOWAIT);
        if (r == -1) {
            if (errno == ENOMSG) { usleep(50000); continue; }
            if (errno == EINTR) continue;
            break;
        }

        int did = req.drone_id;

        switch (req.mtype) {
            case MSG_REQ_LAND:
                if (current_active > target_N) { enqueue(0, did); olog(C_RED "[Operator] BLOCKED %d" C_RESET "\n", did); }
                else if (get_hangar_free_slots() > 0) {
                    int ch = find_available_channel(DIR_IN);
                    if (ch != -1 && reserve_hangar_spot()) {
                        chan_dir[ch] = DIR_IN; chan_users[ch]++; send_grant(did, ch);
                        olog(C_GREEN "[Operator] GRANT LAND %d via Ch %d" C_RESET "\n", did, ch);
                    } else enqueue(0, did);
                } else enqueue(0, did);
                break;
            case MSG_REQ_TAKEOFF:
                {
                    int ch = find_available_channel(DIR_OUT);
                    if (ch != -1) {
                        chan_dir[ch] = DIR_OUT; chan_users[ch]++; send_grant(did, ch);
                        olog(C_GREEN "[Operator] GRANT TAKEOFF %d via Ch %d" C_RESET "\n", did, ch);
                    } else enqueue(1, did);
                }
                break;
            case MSG_LANDED:
                {
                    int found = 0;
                    for(int i=0; i<CHANNELS; i++) {
                        if (chan_dir[i] == DIR_IN && chan_users[i] > 0) {
                            chan_users[i]--; if (chan_users[i] == 0) chan_dir[i] = DIR_NONE;
                            olog(C_CYAN "[Operator] Drone %d entered base." C_RESET "\n", did);
                            found = 1; break;
                        }
                    }
                    if (!found) olog(C_RED "[Operator] WARN: Unexpected LANDED from %d" C_RESET "\n", did);
                    process_queues();
                }
                break;
            case MSG_DEPARTED:
                {
                    int found = 0;
                    for(int i=0; i<CHANNELS; i++) {
                        if (chan_dir[i] == DIR_OUT && chan_users[i] > 0) {
                            chan_users[i]--; if (chan_users[i] == 0) chan_dir[i] = DIR_NONE;
                            olog(C_CYAN "[Operator] Drone %d left." C_RESET "\n", did);
                            found = 1; break;
                        }
                    }
                    if (!found) olog(C_RED "[Operator] ERROR: Got MSG_DEPARTED but no channel active OUT!" C_RESET "\n");
                    free_hangar_spot();
                    process_queues();
                }
                break;
            case MSG_DEAD:
                olog(C_RED "[Operator] RIP drone %d." C_RESET "\n", did);
                remove_dead(did);
                current_active--;
                if (shared_mem != NULL && did < MAX_DRONE_ID) shared_mem->drone_pids[did] = 0;
                olog(C_BLUE "[Operator] Active: %d/%d" C_RESET "\n", current_active, target_N);
                break;
        }
    }

    if (shared_mem) shmdt(shared_mem);
    if (msqid != -1) msgctl(msqid, IPC_RMID, NULL);
    if (semid != -1) semctl(semid, 0, IPC_RMID);
    return 0;
}