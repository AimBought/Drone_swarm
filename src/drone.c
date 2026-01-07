/* src/drone.c
 *
 * Modu³ Drona (Symulacja procesu potomnego).
 * Realizuje cykl ¿ycia: Lot -> Kolejka -> L¹dowanie -> £adowanie -> Start.
 * Obs³uguje: Zu¿ycie baterii, Starzenie siê (cykle), Sygna³ Kamikadze.
 */

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

// --- PARAMETRY SYMULACJI ---
#define BATTERY_FULL 100
#define BATTERY_CRITICAL 20
#define BATTERY_DEAD 0
#define TICK_US 100000      // Krok symulacji (100ms)
#define CONST_CHARGE_TIME 20 // Czas ³adowania (s)
#define CROSSING_TIME 1     // Czas przelotu przez tunel (s)
#define LIFE_LIMIT 3        // Po ilu cyklach dron idzie na z³om

// --- STANY LOKALIZACJI ---
#define ST_OUTSIDE 0 // Dron w powietrzu lub w kolejce przed baz¹
#define ST_INSIDE  1 // Dron w bazie lub w tunelu wylotowym

// --- ZMIENNE GLOBALNE ---
static int msqid = -1;
static volatile sig_atomic_t keep_running = 1;
static char log_filename[64]; 

// Stan wewnêtrzny drona
typedef struct {
    int id;
    double current_battery;
    int T1; // Czas w bazie
    int T2; // Max czas lotu
    double drain_rate_per_sec;
    int cycles_flown;
    int max_cycles;
    
    volatile int location;         // Gdzie jestem?
    volatile int kamikaze_pending; // Flaga opóŸnionej œmierci
} DroneState;

static DroneState drone; 

// --- FUNKCJE POMOCNICZE ---

// Wrapper do logowania (Ekran + Plik)
void dlog(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vprintf(format, args); // Na ekran
    va_end(args);

    FILE *f = fopen(log_filename, "a"); // Do pliku
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

// Wys³anie komunikatu do Operatora
int send_msg(long type, int drone_id) {
    struct msg_req req;
    req.mtype = type;
    req.drone_id = drone_id;
    if (msgsnd(msqid, &req, sizeof(req) - sizeof(long), 0) == -1) {
        perror("[Drone] msgsnd failed");
        return -1;
    }
    return 0;
}

// Procedura œmierci (koniec procesu)
void drone_die() {
    send_msg(MSG_DEAD, drone.id);
    dlog(C_RED "[Drone %d] RIP (Self-destruct/Battery/Age)." C_RESET "\n", drone.id);
    exit(0);
}

// --- OBS£UGA SYGNA£ÓW ---

void sigint_handler(int sig) { (void)sig; keep_running = 0; }

// Handler Sygna³u 3 (Atak Kamikadze)
void sigusr1_handler(int sig) {
    (void)sig;
    
    // 1. Ochrona: Jeœli bateria niska, ignoruj rozkaz
    if (drone.current_battery < BATTERY_CRITICAL) {
        dlog(C_YELLOW "\n[Drone %d] Kamikaze order IGNORED. Battery too low (%.1f%%)." C_RESET "\n", drone.id, drone.current_battery);
        return;
    }

    dlog(C_RED "\n[Drone %d] !!! KAMIKAZE ORDER RECEIVED !!! Battery: %.1f%%" C_RESET "\n", drone.id, drone.current_battery);

    // 2. Decyzja w zale¿noœci od lokalizacji
    if (drone.location == ST_OUTSIDE) {
        dlog(C_RED "[Drone %d] Location: OUTSIDE. Dying immediately." C_RESET "\n", drone.id);
        drone_die();
    } else {
        dlog(C_RED "[Drone %d] Location: INSIDE BASE. Will die after exit." C_RESET "\n", drone.id);
        drone.kamikaze_pending = 1;
    }
}

// Inicjalizacja parametrów
void init_drone_params(DroneState *d, int id, int start_mode) {
    // Inicjalizacja generatora losowego (unikalna dla ka¿dego procesu)
    srand(time(NULL) ^ getpid());

    d->id = id;

    if (start_mode == 1) {
        // Nowy dron z fabryki (lub po respawnie) ma zawsze pe³n¹ bateriê
        d->current_battery = BATTERY_FULL;
    } else {
        // Dron startuj¹cy "w powietrzu" ma losow¹ bateriê od 50% do 100%
        // To zapobiega sytuacji, gdzie wszystkie drony l¹duj¹ w tej samej sekundzie.
        d->current_battery = 50.0 + (rand() % 51);
    }

    d->T1 = CONST_CHARGE_TIME; 
    d->T2 = (int)(2.5 * d->T1);
    d->drain_rate_per_sec = 80.0 / (double)d->T2;
    d->cycles_flown = 0;
    d->max_cycles = LIFE_LIMIT; 
    d->location = ST_OUTSIDE;
    d->kamikaze_pending = 0;
    
    dlog("[Drone %d] Init: Flight=%ds, Charge=%ds, Life=%d cycles, Bat=%.1f%%\n", 
           d->id, d->T2, d->T1, d->max_cycles, d->current_battery);
}

// --- MAIN LOOP ---

int main(int argc, char *argv[]) {
    if (argc < 3) return 1;
    int id = atoi(argv[1]);
    int start_mode = atoi(argv[2]); // 0=Powietrze, 1=Baza
    
    // Log per proces
    snprintf(log_filename, sizeof(log_filename), "drone_%d.txt", getpid());
    FILE *f = fopen(log_filename, "w"); if (f) fclose(f);

    signal(SIGINT, sigint_handler);
    signal(SIGUSR1, sigusr1_handler);

    msqid = msgget(MSGQ_KEY, 0600);
    if (msqid == -1) { perror("msgget"); return 1; }

    // Przekazujemy start_mode do funkcji init, ¿eby wylosowaæ bateriê
    init_drone_params(&drone, id, start_mode);
    
    struct msg_resp resp;

    // Ustawienie pocz¹tkowego stanu
    if (start_mode == 1) drone.location = ST_INSIDE;
    else drone.location = ST_OUTSIDE;

    dlog(C_GREEN "[Drone %d] Ready (PID %d). Mode: %s. Battery: %.1f%%" C_RESET "\n", 
         id, getpid(), start_mode ? "BASE" : "AIR", drone.current_battery);

    // Jeœli dron rodzi siê w bazie, pomijamy fazê lotu
    if (start_mode == 1) {
        dlog(C_BLUE "[Drone %d] Created inside BASE. Preparing for immediate TAKEOFF." C_RESET "\n", id);
        goto start_from_base;
    }

    while (keep_running) {
        // --- ETAP 1: LOT SWOBODNY ---
        drone.location = ST_OUTSIDE; 
        dlog(C_CYAN "[Drone %d] Flying... (Bat: %.1f%%)" C_RESET "\n", id, drone.current_battery);
        
        // Symulacja roz³adowywania
        while (drone.current_battery > BATTERY_CRITICAL && keep_running) {
            usleep(TICK_US);
            drone.current_battery -= (drone.drain_rate_per_sec * (TICK_US / 1000000.0));
            if (drone.current_battery <= BATTERY_DEAD) {
                drone.current_battery = 0.0;
                drone_die();
            }
        }
        if (!keep_running) break;
        
        // --- ETAP 2: OCZEKIWANIE NA L¥DOWANIE ---
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
                } else {
                    if (errno != EINTR) perror("[Drone] msgrcv failed"); 
                    break;
                }
            }
        }
        if (!keep_running) break;

        // --- ETAP 3: WLOT DO BAZY ---
        dlog(C_CYAN "[Drone %d] Crossing channel %d IN..." C_RESET "\n", id, channel);
        sleep(CROSSING_TIME); 
        
        drone.location = ST_INSIDE;
        send_msg(MSG_LANDED, id);

        // --- ETAP 4: £ADOWANIE ---
        dlog(C_GREEN "[Drone %d] Charging..." C_RESET "\n", id);

        // Obliczamy ile ticków trwa ³adowanie (np. 2s / 0.1s = 20 ticków)
        int charge_ticks = (drone.T1 * 1000000) / TICK_US;
        // Obliczamy ile brakuje do pe³na
	double missing_charge = 100.0 - drone.current_battery;

	// Rozk³adamy ten brakuj¹cy ³adunek na ca³y czas T1
	double charge_per_tick = (missing_charge / (double)drone.T1) * (TICK_US / 1000000.0);
        for (int i = 0; i < charge_ticks && keep_running; i++) {
            if (drone.kamikaze_pending) {
                dlog(C_RED "[Drone %d] Charging ABORTED due to KAMIKAZE order." C_RESET "\n", id);
                break;
            }
            
            usleep(TICK_US); // Czekaj 0.1s
            
            drone.current_battery += charge_per_tick;
            if (drone.current_battery > 100.0) drone.current_battery = 100.0;
            
            //Opcjonalnie: Loguj postêp co 1 sekundê (co 10 ticków), ¿eby nie zaœmiecaæ logów
            if (i % 10 == 0) dlog("[Drone %d] Charging: %.1f%%\n", id, drone.current_battery);
        }
        
        if (!drone.kamikaze_pending) drone.current_battery = BATTERY_FULL;
        drone.cycles_flown++;
        dlog("[Drone %d] Maintenance Log: Cycle %d/%d completed.\n", 
               id, drone.cycles_flown, drone.max_cycles);

start_from_base:
        // --- ETAP 5: START ---
        drone.location = ST_INSIDE;
        dlog("[Drone %d] Requesting TAKEOFF.\n", id);
        if (send_msg(MSG_REQ_TAKEOFF, id) == -1) break;

        if (msgrcv(msqid, &resp, sizeof(resp) - sizeof(long), RESPONSE_BASE + id, 0) == -1) {
             if (errno != EINTR) perror("[Drone] msgrcv blocking failed");
             break;
        }
        channel = resp.channel_id;

        // --- ETAP 6: WYLOT ---
        dlog(C_CYAN "[Drone %d] Crossing channel %d OUT..." C_RESET "\n", id, channel);
        sleep(CROSSING_TIME);

        send_msg(MSG_DEPARTED, id);
        drone.location = ST_OUTSIDE;
        
        dlog(C_CYAN "[Drone %d] Back in the air." C_RESET "\n", id);
        
        // --- ETAP 7: EWENTUALNY ZGON ---
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