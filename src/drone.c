/* src/drone.c */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <sys/ipc.h>
#include <sys/msg.h>

#include "common.h"

// Parametry
#define BATTERY_FULL 100
#define BATTERY_CRITICAL 20
#define BATTERY_DEAD 0
#define TICK_US 100000 
#define CONST_CHARGE_TIME 5
#define CROSSING_TIME 1      // Czas przelotu przez tunel

static volatile sig_atomic_t keep_running = 1;

void sigint_handler(int sig) {
    (void)sig;
    keep_running = 0;
}

typedef struct {
    int id;
    double current_battery;
    int T1; 
    int T2; 
    double drain_rate_per_sec; 
} DroneState;

void init_drone_params(DroneState *d, int id) {
    d->id = id;
    d->current_battery = BATTERY_FULL;
    d->T1 = CONST_CHARGE_TIME; 
    d->T2 = (int)(2.5 * d->T1);
    d->drain_rate_per_sec = 80.0 / (double)d->T2;
    
    printf("[Drone %d] Init: Flight=%ds, Charge=%ds, Drain=%.2f/s\n", 
           d->id, d->T2, d->T1, d->drain_rate_per_sec);
}

// Funkcja wysy³aj¹ca komunikaty
int send_msg(int msqid, long type, int drone_id) {
    struct msg_req req;
    req.mtype = type;
    req.drone_id = drone_id;
    return msgsnd(msqid, &req, sizeof(req) - sizeof(long), 0);
}

int main(int argc, char *argv[]) {
    if (argc < 2) return 1;
    int id = atoi(argv[1]);
    
    signal(SIGINT, sigint_handler);

    int msqid = msgget(MSGQ_KEY, 0666);
    if (msqid == -1) {
        perror("[Drone] msgget");
        return 1;
    }

    DroneState drone;
    init_drone_params(&drone, id);
    struct msg_resp resp;

    printf("[Drone %d] Ready.\n", id);

    while (keep_running) {
        // --- 1. LOT ---
        printf("[Drone %d] Flying... (Bat: %.1f%%)\n", id, drone.current_battery);
        while (drone.current_battery > BATTERY_CRITICAL && keep_running) {
            usleep(TICK_US);
            drone.current_battery -= (drone.drain_rate_per_sec * (TICK_US / 1000000.0));
            if (drone.current_battery <= BATTERY_DEAD) {
                drone.current_battery = 0.0;
                goto drone_death;
            }
        }
        if (!keep_running) break;
        printf("[Drone %d] Requesting LANDING (Bat: %.1f%%)\n", id, drone.current_battery);

        // --- 2. PROŒBA O L¥DOWANIE ---
        if (send_msg(msqid, MSG_REQ_LAND, id) == -1) break;

        // Oczekiwanie na przydzia³ tunelu
        int channel = -1;
        int granted = 0;
        while (!granted && keep_running) {
            ssize_t r = msgrcv(msqid, &resp, sizeof(resp) - sizeof(long), RESPONSE_BASE + id, IPC_NOWAIT);
            if (r != -1) {
                granted = 1;
                channel = resp.channel_id;
            } else {
                if (errno == ENOMSG) {
                    usleep(TICK_US);
                    drone.current_battery -= (drone.drain_rate_per_sec * (TICK_US / 1000000.0));
                    if (drone.current_battery <= BATTERY_DEAD) {
                        printf("[Drone %d] Died waiting for landing.\n", id);
                        goto drone_death;
                    }
                } else break;
            }
        }
        if (!keep_running) break;

        // --- 3. PRZELOT PRZEZ TUNEL (WLOT) ---
        printf("[Drone %d] Crossing channel %d IN...\n", id, channel);
        sleep(CROSSING_TIME); 
        
        // Potwierdzenie wlotu
        send_msg(msqid, MSG_LANDED, id);

        // --- 4. £ADOWANIE ---
        printf("[Drone %d] Charging...\n", id);
        sleep(drone.T1);
        drone.current_battery = BATTERY_FULL;

        // --- 5. PROŒBA O START ---
        printf("[Drone %d] Charged. Requesting TAKEOFF.\n", id);
        if (send_msg(msqid, MSG_REQ_TAKEOFF, id) == -1) break;

        // Oczekiwanie na tunel wyjœciowy (tu bateria nie spada, bo jesteœmy w bazie)
        if (msgrcv(msqid, &resp, sizeof(resp) - sizeof(long), RESPONSE_BASE + id, 0) == -1) break;
        channel = resp.channel_id;

        // --- 6. PRZELOT PRZEZ TUNEL (WYLOT) ---
        printf("[Drone %d] Crossing channel %d OUT...\n", id, channel);
        sleep(CROSSING_TIME);

        // Potwierdzenie wylotu
        send_msg(msqid, MSG_DEPARTED, id);
        printf("[Drone %d] Back in the air.\n", id);
    }

    return 0;

drone_death:
    send_msg(msqid, MSG_DEAD, id);
    printf("[Drone %d] RIP.\n", id);
    return 0;
}