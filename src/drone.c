/*
 drone.c
 - bierze ID z argv[1]
 - nieskoñczona pêtla:
   - 'lot' (sleep 3)
   - wysy³a MSG_REQUEST z drone_id
   - czeka na wiadomoœæ RESPONSE_BASE + drone_id (blocking)
   - po otrzymaniu GRANT: wchodzi do bazy (sleep 3)
   - wysy³a MSG_LEAVING
 - obs³uguje SIGINT (wychodzi)
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>

#include <sys/ipc.h>
#include <sys/msg.h>

#include "common.h"

static volatile sig_atomic_t keep_running = 1;
void sigint_handler(int sig) { (void)sig; keep_running = 0; }

struct msg_req {
    long mtype;
    int drone_id;
};

struct msg_resp {
    long mtype;
    char text[RESP_TEXT_SIZE];
};

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <drone_id>\n", argv[0]);
        return 1;
    }

    int id = atoi(argv[1]);
    signal(SIGINT, sigint_handler);

    // po³¹czenie do kolejki
    int msqid = msgget(MSGQ_KEY, 0666);
    if (msqid == -1) {
        perror("[Drone] msgget");
        return 1;
    }

    printf("[Drone %d] Ready. Entering flight loop.\n", id);

    while (keep_running) {
        // lot
        printf("[Drone %d] Taking off (3s)...\n", id);
        sleep(3);

        // powrót i proœba o wejœcie
        printf("[Drone %d] Requesting entry to base...\n", id);
        struct msg_req req;
        req.mtype = MSG_REQUEST;
        req.drone_id = id;
        if (msgsnd(msqid, &req, sizeof(req) - sizeof(long), 0) == -1) {
            perror("[Drone] msgsnd(request)");
            break;
        }

        // oczekiwanie na grant (typ = RESPONSE_BASE + id)
        struct msg_resp resp;
        ssize_t r = msgrcv(msqid, &resp, sizeof(resp.text), RESPONSE_BASE + id, 0);
        if (r == -1) {
            if (errno == EINTR) break;
            perror("[Drone] msgrcv(resp)");
            break;
        }

        printf("[Drone %d] Received grant: %s. Entering base (3s)...\n", id, resp.text);
        sleep(3); // pobyt w bazie

        // opuszczenie bazy
        struct msg_req leave;
        leave.mtype = MSG_LEAVING;
        leave.drone_id = id;
        if (msgsnd(msqid, &leave, sizeof(leave) - sizeof(long), 0) == -1) {
            perror("[Drone] msgsnd(leave)");
            break;
        }
        printf("[Drone %d] Left base. Preparing next flight.\n", id);
    }

    printf("[Drone %d] Shutting down.\n", id);
    return 0;
}
