/*
 operator.c
 - tworzy kolejkê komunikatów i semafor pojemnoœci (P)
 - odbiera REQUEST od dronów i albo przyznaje miejsce (jeœli jest),
   albo trzyma drony w lokalnej kolejce oczekuj¹cych
 - obs³uguje LEAVING (zwalnianie miejsca) i po zwolnieniu przyznaje
   miejsce pierwszemu oczekuj¹cemu (FIFO)
 - obs³uguje SIGINT i usuwa IPC (msgctl, semctl)
*/

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

/* struktury komunikatów - payload */
struct msg_req {
    long mtype;    // MSG_REQUEST or MSG_LEAVING
    int drone_id;
};

struct msg_resp {
    long mtype;              // RESPONSE_BASE + drone_id
    char text[RESP_TEXT_SIZE];
};

/* semun dla semctl (potrzebne) */
union semun {
    int val;
    struct semid_ds *buf;
    unsigned short *array;
};

/* prosta FIFO kolejka oczekuj¹cych dronów (id) */
#define WAITQ_CAP 1024
int waitq[WAITQ_CAP];
int waitq_head = 0, waitq_tail = 0;

static int msqid = -1;
static int semid = -1;
static volatile sig_atomic_t keep_running = 1;

void enqueue_wait(int id) {
    int next = (waitq_tail + 1) % WAITQ_CAP;
    if (next == waitq_head) {
        fprintf(stderr, "[Operator] Waiting queue full! Dropping request %d\n", id);
        return;
    }
    waitq[waitq_tail] = id;
    waitq_tail = next;
}

int dequeue_wait() {
    if (waitq_head == waitq_tail) return -1; // empty
    int id = waitq[waitq_head];
    waitq_head = (waitq_head + 1) % WAITQ_CAP;
    return id;
}

void cleanup(int sig) {
    (void)sig;
    keep_running = 0;
    printf("\n[Operator] SIGINT received. Cleaning up IPC resources...\n");

    if (msqid != -1) {
        if (msgctl(msqid, IPC_RMID, NULL) == -1)
            perror("[Operator] msgctl(IPC_RMID)");
        else
            printf("[Operator] Message queue removed.\n");
    }

    if (semid != -1) {
        if (semctl(semid, 0, IPC_RMID) == -1)
            perror("[Operator] semctl(IPC_RMID)");
        else
            printf("[Operator] Semaphores removed.\n");
    }
}

int sem_get_value() {
    int val = semctl(semid, 0, GETVAL);
    if (val == -1) perror("[Operator] semctl(GETVAL)");
    return val;
}

int sem_dec_nowait() {
    struct sembuf op;
    op.sem_num = 0;
    op.sem_op = -1; // decrement
    op.sem_flg = IPC_NOWAIT;
    if (semop(semid, &op, 1) == -1) {
        if (errno == EAGAIN || errno == EACCES) return 0;
        perror("[Operator] semop(-1)");
        return -1;
    }
    return 1;
}

int sem_inc() {
    struct sembuf op;
    op.sem_num = 0;
    op.sem_op = 1; // increment
    op.sem_flg = 0;
    if (semop(semid, &op, 1) == -1) {
        perror("[Operator] semop(+1)");
        return -1;
    }
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <P>\n", argv[0]);
        return 1;
    }

    int P = atoi(argv[1]);
    if (P <= 0) {
        fprintf(stderr, "[Operator] P must be > 0\n");
        return 1;
    }

    signal(SIGINT, cleanup);

    // 1) utworzenie kolejki komunikatów
    msqid = msgget(MSGQ_KEY, IPC_CREAT | 0666);
    if (msqid == -1) {
        perror("[Operator] msgget");
        return 1;
    }
    printf("[Operator] Message queue created (id=%d)\n", msqid);

    // 2) utworzenie semafora z wartoœci¹ P
    semid = semget(SEM_KEY, 1, IPC_CREAT | 0666);
    if (semid == -1) {
        perror("[Operator] semget");
        // cleanup msgqueue
        msgctl(msqid, IPC_RMID, NULL);
        return 1;
    }
    union semun arg;
    arg.val = P;
    if (semctl(semid, 0, SETVAL, arg) == -1) {
        perror("[Operator] semctl(SETVAL)");
        msgctl(msqid, IPC_RMID, NULL);
        semctl(semid, 0, IPC_RMID);
        return 1;
    }
    printf("[Operator] Semaphore created (id=%d) with P=%d\n", semid, P);

    printf("[Operator] Ready. Waiting for requests...\n");

    // g³ówna pêtla - odbieraj komunikaty od dronów
    while (keep_running) {
        struct msg_req req;
        ssize_t r = msgrcv(msqid, &req, sizeof(req) - sizeof(long), 0, 0); 
        // msgtyp = 0 => odbierz pierwsz¹ dostêpn¹ wiadomoœæ (kolejnoœæ kolejki)
        if (r == -1) {
            if (errno == EINTR) continue; // przerwane przez sygna³
            perror("[Operator] msgrcv");
            break;
        }

        if (req.mtype == MSG_REQUEST) {
            int did = req.drone_id;
            // sprawdŸ, czy jest miejsce
            int val = sem_get_value();
            if (val > 0) {
                // zarezerwuj miejsce - bez blokowania innych
                if (sem_dec_nowait() == 1) {
                    // send grant
                    struct msg_resp resp;
                    resp.mtype = RESPONSE_BASE + did;
                    snprintf(resp.text, RESP_TEXT_SIZE, "GRANT");
                    if (msgsnd(msqid, &resp, sizeof(resp.text), 0) == -1) {
                        perror("[Operator] msgsnd(grant)");
                    } else {
                        printf("[Operator] Granted entry to drone %d (slots left=%d)\n", did, sem_get_value());
                    }
                } else {
                    // nie uda³o siê zredukowaæ semafora - wrzuæ do lokalnej kolejki
                    enqueue_wait(did);
                    printf("[Operator] Could not decrement semaphore now; queued drone %d\n", did);
                }
            } else {
                // brak miejsca -> enqueue
                enqueue_wait(did);
                printf("[Operator] No free slots. Drone %d queued.\n", did);
            }
        } else if (req.mtype == MSG_LEAVING) {
            int did = req.drone_id;
            // zwolnij miejsce
            if (sem_inc() == 0) {
                printf("[Operator] Drone %d left. Slot freed (slots now=%d)\n", did, sem_get_value());
            }

            // jeœli ktoœ czeka, przyznaj miejsce pierwszemu w kolejce
            int next = dequeue_wait();
            if (next != -1) {
                // zarezerwuj za niego (semop -1), powinno siê udaæ bo w³aœnie zwolniliœmy
                if (sem_dec_nowait() == 1) {
                    // send grant
                    struct msg_resp resp;
                    resp.mtype = RESPONSE_BASE + next;
                    snprintf(resp.text, RESP_TEXT_SIZE, "GRANT");
                    if (msgsnd(msqid, &resp, sizeof(resp.text), 0) == -1) {
                        perror("[Operator] msgsnd(grant to queued)");
                    } else {
                        printf("[Operator] Granted entry to queued drone %d\n", next);
                    }
                } else {
                    // bardzo ma³o prawdopodobne: jeœli nie uda³o siê zarezerwowaæ, wstaw z powrotem
                    enqueue_wait(next);
                    printf("[Operator] Unexpected: couldn't reserve for queued drone %d, requeueing\n", next);
                }
            }
        } else {
            printf("[Operator] Unknown message type: %ld\n", req.mtype);
        }
    }

    // cleanup handled in handler
    printf("[Operator] Exiting\n");
    return 0;
}
