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
#define CONST_CHARGE_TIME 2 // Skróci³em czas ³adowania dla szybszych testów
#define CROSSING_TIME 1

// LIMIT CYKLI (Dla testów ma³y, np. 3)
#define LIFE_LIMIT 3 

static int msqid = -1;
static volatile sig_atomic_t keep_running = 1;

typedef struct {
    int id;
    double current_battery;
    int T1; 
    int T2; 
    double drain_rate_per_sec;
    
    // NOWOŒÆ: Liczniki ¿ycia
    int cycles_flown;
    int max_cycles;
} DroneState;

static DroneState drone; 

void sigint_handler(int sig) {
    (void)sig;
    keep_running = 0;
}

int send_msg(long type, int drone_id) {
    struct msg_req req;
    req.mtype = type;
    req.drone_id = drone_id;
    return msgsnd(msqid, &req, sizeof(req) - sizeof(long), 0);
}

void sigusr1_handler(int sig) {
    (void)sig;
    if (drone.current_battery >= BATTERY_CRITICAL) {
        printf("\n[Drone %d] !!! KAMIKAZE ORDER RECEIVED !!! (Bat: %.1f%%)\n", drone.id, drone.current_battery);
        send_msg(MSG_DEAD, drone.id);
        exit(0);
    } else {
        printf("\n[Drone %d] Kamikaze order IGNORED. Battery low.\n", drone.id);
    }
}

void init_drone_params(DroneState *d, int id) {
    d->id = id;
    d->current_battery = BATTERY_FULL;
    d->T1 = CONST_CHARGE_TIME; 
    d->T2 = (int)(2.5 * d->T1);
    d->drain_rate_per_sec = 80.0 / (double)d->T2;
    
    // Inicjalizacja liczników
    d->cycles_flown = 0;
    d->max_cycles = LIFE_LIMIT; // Tu mo¿na dodaæ losowoœæ, np. 3 + rand()%3
    
    printf("[Drone %d] Init: Flight=%ds, Charge=%ds, Life=%d cycles\n", 
           d->id, d->T2, d->T1, d->max_cycles);
}

int main(int argc, char *argv[]) {
    if (argc < 2) return 1;
    int id = atoi(argv[1]);
    
    signal(SIGINT, sigint_handler);
    signal(SIGUSR1, sigusr1_handler);

    msqid = msgget(MSGQ_KEY, 0666);
    if (msqid == -1) {
        perror("[Drone] msgget");
        return 1;
    }

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
        if (send_msg(MSG_REQ_LAND, id) == -1) break;

        // Oczekiwanie
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

        // --- 3. WLOT ---
        printf("[Drone %d] Crossing channel %d IN...\n", id, channel);
        sleep(CROSSING_TIME); 
        send_msg(MSG_LANDED, id);

        // --- 4. £ADOWANIE ---
        printf("[Drone %d] Charging...\n", id);
        unsigned int left = drone.T1;
        while (left > 0 && keep_running) { left = sleep(left); }
        
        drone.current_battery = BATTERY_FULL;
        
        // ZWIÊKSZAMY LICZNIK CYKLI
        drone.cycles_flown++;
        printf("[Drone %d] Maintenance Log: Cycle %d/%d completed.\n", 
               id, drone.cycles_flown, drone.max_cycles);

        // --- 5. PROŒBA O START ---
        printf("[Drone %d] Charged. Requesting TAKEOFF.\n", id);
        if (send_msg(MSG_REQ_TAKEOFF, id) == -1) break;

        if (msgrcv(msqid, &resp, sizeof(resp) - sizeof(long), RESPONSE_BASE + id, 0) == -1) break;
        channel = resp.channel_id;

        // --- 6. WYLOT ---
        printf("[Drone %d] Crossing channel %d OUT...\n", id, channel);
        sleep(CROSSING_TIME);

        send_msg(MSG_DEPARTED, id);
        printf("[Drone %d] Back in the air.\n", id);
        
        // --- 7. SPRAWDZENIE ZU¯YCIA (UTYLIZACJA) ---
        // Sprawdzamy dopiero po zwolnieniu miejsca w bazie (po MSG_DEPARTED)
        if (drone.cycles_flown >= drone.max_cycles) {
            printf("[Drone %d] RETIRING: Wear limit reached (%d cycles). Goodbye.\n", 
                   id, drone.cycles_flown);
            goto drone_death; // Skocz do wys³ania MSG_DEAD i zakoñczenia
        }
    }

    return 0;

drone_death:
    send_msg(MSG_DEAD, id);
    // Tutaj proces siê koñczy. Operator otrzyma MSG_DEAD i:
    // 1. Zmniejszy current_active.
    // 2. Usunie PID z pamiêci dzielonej.
    // 3. W nastêpnym cyklu CHECK stworzy nowego drona (œwie¿ego, z licznikiem 0).
    return 0;
}