/* src/drone.c */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <stdarg.h> // Do obs³ugi zmiennej liczby argumentów (...)
#include <sys/ipc.h>
#include <sys/msg.h>

#include "common.h"

// Parametry
#define BATTERY_FULL 100
#define BATTERY_CRITICAL 20
#define BATTERY_DEAD 0
#define TICK_US 100000 
#define CONST_CHARGE_TIME 2 
#define CROSSING_TIME 1
#define LIFE_LIMIT 3 

static int msqid = -1;
static volatile sig_atomic_t keep_running = 1;
static char log_filename[64]; // Nazwa pliku loga

typedef struct {
    int id;
    double current_battery;
    int T1; 
    int T2; 
    double drain_rate_per_sec;
    int cycles_flown;
    int max_cycles;
} DroneState;

static DroneState drone; 

// --- FUNKCJA LOGUJ¥CA (Ekran + Plik) ---
void dlog(const char *format, ...) {
    va_list args;
    
    // 1. Wypisz na ekran
    va_start(args, format);
    vprintf(format, args);
    va_end(args);

    // 2. Zapisz do pliku
    FILE *f = fopen(log_filename, "a");
    if (f) {
        // Pobierz czas
        time_t now = time(NULL);
        struct tm *t = localtime(&now);
        char timebuf[32];
        strftime(timebuf, sizeof(timebuf), "%H:%M:%S", t);

        fprintf(f, "[%s] ", timebuf); // Znacznik czasu
        
        va_start(args, format);
        vfprintf(f, format, args);    // Treœæ komunikatu
        va_end(args);
        
        fclose(f);
    }
}
// ----------------------------------------

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
        dlog("\n[Drone %d] !!! KAMIKAZE ORDER RECEIVED !!! (Bat: %.1f%%)\n", drone.id, drone.current_battery);
        send_msg(MSG_DEAD, drone.id);
        exit(0);
    } else {
        dlog("\n[Drone %d] Kamikaze order IGNORED. Battery low.\n", drone.id);
    }
}

void init_drone_params(DroneState *d, int id) {
    d->id = id;
    d->current_battery = BATTERY_FULL;
    d->T1 = CONST_CHARGE_TIME; 
    d->T2 = (int)(2.5 * d->T1);
    d->drain_rate_per_sec = 80.0 / (double)d->T2;
    d->cycles_flown = 0;
    d->max_cycles = LIFE_LIMIT; 
    
    dlog("[Drone %d] Init: Flight=%ds, Charge=%ds, Life=%d cycles\n", 
           d->id, d->T2, d->T1, d->max_cycles);
}

int main(int argc, char *argv[]) {
    if (argc < 2) return 1;
    int id = atoi(argv[1]);
    
    // Ustalenie nazwy pliku loga: drone_PID.txt
    snprintf(log_filename, sizeof(log_filename), "drone_%d.txt", getpid());
    // Wyczyszczenie pliku przy starcie (nowy proces = nowy log)
    FILE *f = fopen(log_filename, "w");
    if (f) fclose(f);

    signal(SIGINT, sigint_handler);
    signal(SIGUSR1, sigusr1_handler);

    msqid = msgget(MSGQ_KEY, 0666);
    if (msqid == -1) {
        perror("[Drone] msgget"); // Perror zostawiamy na stderr
        return 1;
    }

    init_drone_params(&drone, id);
    struct msg_resp resp;

    dlog("[Drone %d] Ready (PID %d).\n", id, getpid());

    while (keep_running) {
        dlog("[Drone %d] Flying... (Bat: %.1f%%)\n", id, drone.current_battery);
        while (drone.current_battery > BATTERY_CRITICAL && keep_running) {
            usleep(TICK_US);
            drone.current_battery -= (drone.drain_rate_per_sec * (TICK_US / 1000000.0));
            if (drone.current_battery <= BATTERY_DEAD) {
                drone.current_battery = 0.0;
                goto drone_death;
            }
        }
        if (!keep_running) break;
        dlog("[Drone %d] Requesting LANDING (Bat: %.1f%%)\n", id, drone.current_battery);

        if (send_msg(MSG_REQ_LAND, id) == -1) break;

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
                        dlog("[Drone %d] Died waiting for landing.\n", id);
                        goto drone_death;
                    }
                } else break;
            }
        }
        if (!keep_running) break;

        dlog("[Drone %d] Crossing channel %d IN...\n", id, channel);
        sleep(CROSSING_TIME); 
        send_msg(MSG_LANDED, id);

        dlog("[Drone %d] Charging...\n", id);
        unsigned int left = drone.T1;
        while (left > 0 && keep_running) { left = sleep(left); }
        
        drone.current_battery = BATTERY_FULL;
        drone.cycles_flown++;
        dlog("[Drone %d] Maintenance Log: Cycle %d/%d completed.\n", 
               id, drone.cycles_flown, drone.max_cycles);

        dlog("[Drone %d] Charged. Requesting TAKEOFF.\n", id);
        if (send_msg(MSG_REQ_TAKEOFF, id) == -1) break;

        if (msgrcv(msqid, &resp, sizeof(resp) - sizeof(long), RESPONSE_BASE + id, 0) == -1) break;
        channel = resp.channel_id;

        dlog("[Drone %d] Crossing channel %d OUT...\n", id, channel);
        sleep(CROSSING_TIME);

        send_msg(MSG_DEPARTED, id);
        dlog("[Drone %d] Back in the air.\n", id);
        
        if (drone.cycles_flown >= drone.max_cycles) {
            dlog("[Drone %d] RETIRING: Wear limit reached (%d cycles). Goodbye.\n", 
                   id, drone.cycles_flown);
            goto drone_death; 
        }
    }
    return 0;

drone_death:
    send_msg(MSG_DEAD, id);
    dlog("[Drone %d] RIP.\n", id);
    return 0;
}