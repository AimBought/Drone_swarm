/* src/drone.c */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <stdarg.h> 
#include <sys/ipc.h>
#include <sys/msg.h>

#include "common.h"

#define BATTERY_FULL 100
#define BATTERY_CRITICAL 20
#define BATTERY_DEAD 0
#define TICK_US 100000 
#define CONST_CHARGE_TIME 2 
#define CROSSING_TIME 1
#define LIFE_LIMIT 3 

#define ST_OUTSIDE 0
#define ST_INSIDE  1

static int msqid = -1;
static volatile sig_atomic_t keep_running = 1;
static char log_filename[64]; 

typedef struct {
    int id;
    double current_battery;
    int T1; 
    int T2; 
    double drain_rate_per_sec;
    int cycles_flown;
    int max_cycles;
    
    volatile int location;
    volatile int kamikaze_pending;
} DroneState;

static DroneState drone; 

void dlog(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);

    FILE *f = fopen(log_filename, "a");
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

int send_msg(long type, int drone_id) {
    struct msg_req req;
    req.mtype = type;
    req.drone_id = drone_id;
    return msgsnd(msqid, &req, sizeof(req) - sizeof(long), 0);
}

void drone_die() {
    send_msg(MSG_DEAD, drone.id);
    dlog(C_RED "[Drone %d] RIP (Self-destruct/Battery)." C_RESET "\n", drone.id);
    exit(0);
}

void sigint_handler(int sig) { (void)sig; keep_running = 0; }

void sigusr1_handler(int sig) {
    (void)sig;
    
    if (drone.current_battery < BATTERY_CRITICAL) {
        dlog(C_YELLOW "\n[Drone %d] Kamikaze order IGNORED. Battery too low (%.1f%%)." C_RESET "\n", drone.id, drone.current_battery);
        return;
    }

    dlog(C_RED "\n[Drone %d] !!! KAMIKAZE ORDER RECEIVED !!!" C_RESET "\n", drone.id);

    if (drone.location == ST_OUTSIDE) {
        dlog(C_RED "[Drone %d] Location: OUTSIDE. Dying immediately." C_RESET "\n", drone.id);
        drone_die();
    } else {
        dlog(C_RED "[Drone %d] Location: INSIDE BASE. Will die after exit." C_RESET "\n", drone.id);
        drone.kamikaze_pending = 1;
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
    d->location = ST_OUTSIDE;
    d->kamikaze_pending = 0;
    
    dlog("[Drone %d] Init: Flight=%ds, Charge=%ds, Life=%d cycles\n", 
           d->id, d->T2, d->T1, d->max_cycles);
}

int main(int argc, char *argv[]) {
    if (argc < 3) return 1;
    int id = atoi(argv[1]);
    int start_mode = atoi(argv[2]); 
    
    snprintf(log_filename, sizeof(log_filename), "drone_%d.txt", getpid());
    FILE *f = fopen(log_filename, "w"); if (f) fclose(f);

    signal(SIGINT, sigint_handler);
    signal(SIGUSR1, sigusr1_handler);

    msqid = msgget(MSGQ_KEY, 0666);
    if (msqid == -1) { perror("msgget"); return 1; }

    init_drone_params(&drone, id);
    struct msg_resp resp;

    if (start_mode == 1) drone.location = ST_INSIDE;
    else drone.location = ST_OUTSIDE;

    dlog(C_GREEN "[Drone %d] Ready (PID %d). Mode: %s" C_RESET "\n", id, getpid(), start_mode ? "BASE" : "AIR");

    if (start_mode == 1) {
        dlog(C_BLUE "[Drone %d] Created inside BASE. Preparing for immediate TAKEOFF." C_RESET "\n", id);
        goto start_from_base;
    }

    while (keep_running) {
        // --- 1. LOT ---
        drone.location = ST_OUTSIDE; 
        dlog(C_CYAN "[Drone %d] Flying... (Bat: %.1f%%)" C_RESET "\n", id, drone.current_battery);
        
        while (drone.current_battery > BATTERY_CRITICAL && keep_running) {
            usleep(TICK_US);
            drone.current_battery -= (drone.drain_rate_per_sec * (TICK_US / 1000000.0));
            if (drone.current_battery <= BATTERY_DEAD) {
                drone.current_battery = 0.0;
                drone_die();
            }
        }
        if (!keep_running) break;
        
        // --- 2. PROŒBA O L¥DOWANIE ---
        dlog(C_YELLOW "[Drone %d] Requesting LANDING (Bat: %.1f%%)" C_RESET "\n", id, drone.current_battery);
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
                        dlog(C_RED "[Drone %d] Died waiting for landing." C_RESET "\n", id);
                        drone_die();
                    }
                } else break;
            }
        }
        if (!keep_running) break;

        // --- 3. WLOT ---
        dlog(C_CYAN "[Drone %d] Crossing channel %d IN..." C_RESET "\n", id, channel);
        sleep(CROSSING_TIME); 
        
        drone.location = ST_INSIDE;
        send_msg(MSG_LANDED, id);

        // --- 4. £ADOWANIE ---
        dlog(C_GREEN "[Drone %d] Charging..." C_RESET "\n", id);
        unsigned int left = drone.T1;
        while (left > 0 && keep_running) { 
            if (drone.kamikaze_pending) {
                dlog(C_RED "[Drone %d] Charging ABORTED due to KAMIKAZE order." C_RESET "\n", id);
                break;
            }
            left = sleep(left); 
        }
        
        if (!drone.kamikaze_pending) drone.current_battery = BATTERY_FULL;
        drone.cycles_flown++;
        dlog("[Drone %d] Maintenance Log: Cycle %d/%d completed.\n", 
               id, drone.cycles_flown, drone.max_cycles);

start_from_base:
        // --- 5. PROŒBA O START ---
        drone.location = ST_INSIDE;
        dlog("[Drone %d] Requesting TAKEOFF.\n", id);
        if (send_msg(MSG_REQ_TAKEOFF, id) == -1) break;

        if (msgrcv(msqid, &resp, sizeof(resp) - sizeof(long), RESPONSE_BASE + id, 0) == -1) break;
        channel = resp.channel_id;

        // --- 6. WYLOT ---
        dlog(C_CYAN "[Drone %d] Crossing channel %d OUT..." C_RESET "\n", id, channel);
        sleep(CROSSING_TIME);

        send_msg(MSG_DEPARTED, id);
        drone.location = ST_OUTSIDE;
        
        dlog(C_CYAN "[Drone %d] Back in the air." C_RESET "\n", id);
        
        if (drone.kamikaze_pending) {
            dlog(C_RED "[Drone %d] Mission complete. Detonating outside base." C_RESET "\n", id);
            drone_die();
        }
        
        if (drone.cycles_flown >= drone.max_cycles) {
            dlog(C_YELLOW "[Drone %d] RETIRING: Wear limit reached (%d cycles). Goodbye." C_RESET "\n", id, drone.cycles_flown);
            drone_die();
        }
    }
    return 0;
}