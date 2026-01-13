/* src/drone.c
 *
 * Modu³ Drona (Symulacja procesu potomnego).
 * Realizuje cykl ¿ycia: Lot -> Kolejka -> L¹dowanie -> £adowanie -> Start.
 * Obs³uguje: Zu¿ycie baterii, Starzenie siê (cykle), Sygna³ Kamikadze.
 */

#include <stdio.h>      // Standardowe wejœcie/wyjœcie (printf, fopen)
#include <stdlib.h>     // Biblioteka standardowa (exit, atoi, rand, srand)
#include <unistd.h>     // Funkcje systemowe (usleep, sleep, getpid)
#include <signal.h>     // Obs³uga sygna³ów (signal, SIGUSR1)
#include <errno.h>      // Obs³uga b³êdów (zmienna errno, EINTR, ENOMSG)
#include <time.h>       // Funkcje czasu (time, localtime - do logów i seedowania random)
#include <stdarg.h>     // Obs³uga zmiennej liczby argumentów (va_list)
#include <sys/ipc.h>    // Flagi IPC
#include <sys/msg.h>    // Kolejki komunikatów (msgsnd, msgrcv)

#include "common.h"     // Wspólne definicje (klucze IPC, typy wiadomoœci)

// --- PARAMETRY SYMULACJI ---
#define BATTERY_FULL 100
#define BATTERY_CRITICAL 20 // Próg, poni¿ej którego dron prosi o l¹dowanie
#define BATTERY_DEAD 0      // Œmieræ baterii
#define TICK_US 100000      // Krok symulacji (100ms) - co tyle czasu aktualizujemy stan baterii
#define CONST_CHARGE_TIME 20 // Czas ³adowania (s) - sta³y czas spêdzony w hangarze
#define CROSSING_TIME 1     // Czas przelotu przez tunel (s) - symulacja fizycznego ruchu
#define LIFE_LIMIT 3        // Po ilu cyklach dron idzie na z³om (symulacja zu¿ycia sprzêtu)

// --- STANY LOKALIZACJI ---
// Potrzebne do handlera sygna³u Kamikadze, aby wiedzieæ czy mo¿na bezpiecznie umrzeæ
#define ST_OUTSIDE 0 // Dron w powietrzu lub w kolejce przed baz¹ (mo¿na wybuchn¹æ)
#define ST_INSIDE  1 // Dron w bazie lub w tunelu wylotowym (nie mo¿na wybuchn¹æ, bo zablokuje zasoby)

// --- ZMIENNE GLOBALNE ---
static int msqid = -1; // ID kolejki komunikatów
static volatile sig_atomic_t keep_running = 1; // Flaga pêtli g³ównej (reakcja na Ctrl+C)
static char log_filename[64]; // Nazwa pliku logów (unikalna dla PID)

// Stan wewnêtrzny drona - struktura trzymaj¹ca wszystkie parametry ¿yciowe
typedef struct {
    int id;                 // Logiczne ID drona (nadane przez Commandera/Operatora)
    double current_battery; // Bateria (typ double dla p³ynnego odejmowania u³amków %)
    int T1;                 // Czas w bazie (³adowanie)
    int T2;                 // Max czas lotu (pojemnoœæ baku)
    double drain_rate_per_sec; // Jak szybko spada bateria (wyliczane z T2)
    int cycles_flown;       // Licznik wykonanych przelotów (Start-L¹dowanie)
    int max_cycles;         // Limit cykli ¿ycia
    
    volatile int location;         // Gdzie jestem? (volatile, bo zmieniane w main, czytane w handlerze)
    volatile int kamikaze_pending; // Flaga opóŸnionej œmierci (ustawiana w handlerze)
} DroneState;

static DroneState drone; // Instancja stanu dla tego procesu

// --- FUNKCJE POMOCNICZE ---

// Wrapper do logowania (Ekran + Plik)
// Funkcja przyjmuje format jak printf (np. "Bateria: %d", val)
void dlog(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vprintf(format, args); // Wypisanie na ekran konsoli (stdout)
    va_end(args);

    FILE *f = fopen(log_filename, "a"); // Otwarcie pliku w trybie dopisywania
    if (f) {
        time_t now = time(NULL);
        struct tm *t = localtime(&now);
        char timebuf[32];
        strftime(timebuf, sizeof(timebuf), "%H:%M:%S", t); // Dodanie znacznika czasu
        fprintf(f, "[%s] ", timebuf);
        va_start(args, format);
        vfprintf(f, format, args); // Zapisanie treœci do pliku
        va_end(args);
        fclose(f);
    }
}

// Wys³anie komunikatu do Operatora
// U³atwia wysy³anie standardowej struktury msg_req
int send_msg(long type, int drone_id) {
    struct msg_req req;
    req.mtype = type;       // Typ wiadomoœci (REQ_LAND, REQ_TAKEOFF, DEAD, itd.)
    req.drone_id = drone_id; // ID nadawcy
    // msgsnd wysy³a wiadomoœæ do kolejki. Odejmujemy sizeof(long) od rozmiaru.
    if (msgsnd(msqid, &req, sizeof(req) - sizeof(long), 0) == -1) {
        perror("[Drone] msgsnd failed");
        return -1;
    }
    return 0;
}

// Procedura œmierci (koniec procesu)
// Wywo³ywana gdy bateria padnie, dron siê zu¿yje lub dostanie rozkaz Kamikadze
void drone_die() {
    // 1. Najpierw informujemy Operatora, ¿eby zwolni³ nasze zasoby (ID w pamiêci, sloty)
    send_msg(MSG_DEAD, drone.id);
    // 2. Logujemy ostatnie s³owa
    dlog(C_RED "[Drone %d] RIP (Self-destruct/Battery/Age)." C_RESET "\n", drone.id);
    // 3. Koñczymy proces systemowy. System operacyjny posprz¹ta pamiêæ RAM.
    exit(0);
}

// --- OBS£UGA SYGNA£ÓW ---

// Handler Ctrl+C (SIGINT) - bezpieczne wyjœcie z pêtli while
void sigint_handler(int sig) { (void)sig; keep_running = 0; }

// Handler Sygna³u 3 (SIGUSR1 - Atak Kamikadze / Snajper)
// Ten kod wykonuje siê asynchronicznie (przerywa main) w momencie otrzymania sygna³u
void sigusr1_handler(int sig) {
    (void)sig;
    
    // 1. Ochrona: Jeœli bateria niska, ignoruj rozkaz (symulacja awarii systemu)
    if (drone.current_battery < BATTERY_CRITICAL) {
        dlog(C_YELLOW "\n[Drone %d] Kamikaze order IGNORED. Battery too low (%.1f%%)." C_RESET "\n", drone.id, drone.current_battery);
        return;
    }

    dlog(C_RED "\n[Drone %d] !!! KAMIKAZE ORDER RECEIVED !!! Battery: %.1f%%" C_RESET "\n", drone.id, drone.current_battery);

    // 2. Decyzja w zale¿noœci od lokalizacji - KLUCZOWE DLA ZASOBÓW
    if (drone.location == ST_OUTSIDE) {
        // Jeœli jestem na zewn¹trz, mogê umrzeæ od razu, nikogo nie blokujê.
        dlog(C_RED "[Drone %d] Location: OUTSIDE. Dying immediately." C_RESET "\n", drone.id);
        drone_die(); // Wywo³uje exit(0)
    } else {
        // Jeœli jestem w œrodku (zajmujê semafor) lub w tunelu, NIE MOGÊ umrzeæ teraz.
        // Zablokowa³bym semafor na zawsze (deadlock).
        dlog(C_RED "[Drone %d] Location: INSIDE BASE. Will die after exit." C_RESET "\n", drone.id);
        // Ustawiam flagê - sprawdzê j¹ w main() po wylocie z bazy.
        drone.kamikaze_pending = 1; 
    }
}

// Inicjalizacja parametrów drona na starcie
void init_drone_params(DroneState *d, int id, int start_mode) {
    // Inicjalizacja generatora losowego (unikalna dla ka¿dego procesu dziêki XOR z PID)
    // Samo time(NULL) da³oby ten sam seed dla wszystkich dronów startuj¹cych w tej samej sekundzie.
    srand(time(NULL) ^ getpid());

    d->id = id;

    if (start_mode == 1) {
        // Tryb "Baza" (Replenish): Nowy dron z fabryki ma 100% baterii
        d->current_battery = BATTERY_FULL;
    } else {
        // Tryb "Powietrze" (Start symulacji): Losowa bateria 50-100%
        // To desynchronizuje rój, ¿eby wszyscy nie chcieli l¹dowaæ w tej samej chwili.
        d->current_battery = 50.0 + (rand() % 51);
    }

    d->T1 = CONST_CHARGE_TIME; // Czas ³adowania (20s)
    d->T2 = (int)(2.5 * d->T1); // Pojemnoœæ baku (czas lotu) zale¿y od czasu ³adowania
    // Wyliczamy, ile % baterii traciæ na sekundê, ¿eby roz³adowaæ siê w czasie T2
    // U¿ywamy double dla precyzji.
    d->drain_rate_per_sec = 80.0 / (double)d->T2; 
    d->cycles_flown = 0;
    d->max_cycles = LIFE_LIMIT; 
    d->location = ST_OUTSIDE;
    d->kamikaze_pending = 0;
    
    dlog("[Drone %d] Init: Flight=%ds, Charge=%ds, Life=%d cycles, Bat=%.1f%%\n", 
           d->id, d->T2, d->T1, d->max_cycles, d->current_battery);
}

// --- MAIN LOOP ---
// G³ówna pêtla ¿ycia procesu

int main(int argc, char *argv[]) {
    // Sprawdzenie argumentów przekazanych przez execl (ID, Tryb)
    if (argc < 3) return 1;
    int id = atoi(argv[1]);
    int start_mode = atoi(argv[2]); // 0=Start w powietrzu, 1=Start w bazie (Respawn)
    
    // Ustawienie nazwy pliku logów unikalnej dla PID (np. drone_1234.txt)
    snprintf(log_filename, sizeof(log_filename), "drone_%d.txt", getpid());
    FILE *f = fopen(log_filename, "w"); if (f) fclose(f); // Wyczyszczenie pliku

    // Rejestracja handlerów sygna³ów
    signal(SIGINT, sigint_handler);   // Ctrl+C
    signal(SIGUSR1, sigusr1_handler); // Komenda ataku

    // Pod³¹czenie do istniej¹cej kolejki komunikatów (stworzonej przez Operatora)
    // Brak flagi IPC_CREAT, bo dron nie jest w³aœcicielem kolejki.
    msqid = msgget(MSGQ_KEY, 0666);
    if (msqid == -1) { perror("msgget"); return 1; }

    // Zainicjowanie struktury stanu drona
    init_drone_params(&drone, id, start_mode);
    
    struct msg_resp resp; // Struktura na odbiór odpowiedzi od Operatora

    // Ustawienie pocz¹tkowego stanu lokalizacji (dla handlera Kamikadze)
    if (start_mode == 1) drone.location = ST_INSIDE;
    else drone.location = ST_OUTSIDE;

    dlog(C_GREEN "[Drone %d] Ready (PID %d). Mode: %s. Battery: %.1f%%" C_RESET "\n", 
         id, getpid(), start_mode ? "BASE" : "AIR", drone.current_battery);

    // Jeœli dron rodzi siê w bazie (start_mode=1), pomijamy fazê lotu i idziemy do startu
    if (start_mode == 1) {
        dlog(C_BLUE "[Drone %d] Created inside BASE. Preparing for immediate TAKEOFF." C_RESET "\n", id);
        goto start_from_base; // Skok do etykiety na dole pêtli
    }

    // Nieskoñczona pêtla symulacyjna (Cykl ¯ycia)
    while (keep_running) {
        // --- ETAP 1: LOT SWOBODNY ---
        drone.location = ST_OUTSIDE; 
        dlog(C_CYAN "[Drone %d] Flying... (Bat: %.1f%%)" C_RESET "\n", id, drone.current_battery);
        
        // Pêtla symuluj¹ca zu¿ycie baterii w locie
        while (drone.current_battery > BATTERY_CRITICAL && keep_running) {
            usleep(TICK_US); // Œpimy 100ms (symulacja czasu)
            // Odejmujemy odpowiedni¹ czêœæ baterii
            drone.current_battery -= (drone.drain_rate_per_sec * (TICK_US / 1000000.0));
            
            // Sprawdzenie czy bateria nie pad³a w locie
            if (drone.current_battery <= BATTERY_DEAD) {
                drone.current_battery = 0.0;
                drone_die(); // Koniec procesu
            }
        }
        if (!keep_running) break;
        
        // --- ETAP 2: OCZEKIWANIE NA L¥DOWANIE ---
        // Osi¹gniêto próg krytyczny (20%). Prosimy o l¹dowanie.
        dlog(C_YELLOW "[Drone %d] Requesting LANDING (Bat: %.1f%%)" C_RESET "\n", id, drone.current_battery);
        if (send_msg(MSG_REQ_LAND, id) == -1) break; // Wysy³amy proœbê typ 1

        int channel = -1;
        int granted = 0;
        
        // Pêtla oczekiwania na zgodê (wiszenie w powietrzu / kolejce)
        while (!granted && keep_running) {
            // Odbieramy wiadomoœæ TYLKO do nas (typ = RESPONSE_BASE + id)
            // U¿ywamy IPC_NOWAIT (nieblokuj¹co), aby móc traciæ bateriê podczas czekania!
            ssize_t r = msgrcv(msqid, &resp, sizeof(resp) - sizeof(long), RESPONSE_BASE + id, IPC_NOWAIT);
            
            if (r != -1) {
                // Otrzymano zgodê!
                granted = 1;
                channel = resp.channel_id; // Zapisujemy przydzielony tunel
            } else {
                // Brak wiadomoœci (Operator zajêty lub brak miejsc)
                if (errno == ENOMSG) {
                    // Czekaj¹c w kolejce, nadal tracimy paliwo!
                    usleep(TICK_US);
                    drone.current_battery -= (drone.drain_rate_per_sec * (TICK_US / 1000000.0));
                    
                    // Jeœli Operator nie zd¹¿y nas wpuœciæ, spadamy.
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
        sleep(CROSSING_TIME); // Symulacja fizycznego przelotu przez tunel (1s)
        
        drone.location = ST_INSIDE; // Zmieniamy status (ochrona przed Kamikadze)
        send_msg(MSG_LANDED, id);   // Informujemy Operatora: zwolniliœmy tunel, zajêliœmy hangar

        // --- ETAP 4: £ADOWANIE ---
        dlog(C_GREEN "[Drone %d] Charging..." C_RESET "\n", id);

        // Obliczamy ile ticków trwa ³adowanie (np. 20s / 0.1s = 200 ticków)
        int charge_ticks = (drone.T1 * 1000000) / TICK_US;
        // Obliczamy ile brakuje do pe³na
        double missing_charge = 100.0 - drone.current_battery;

        // Rozk³adamy ten brakuj¹cy ³adunek równomiernie na czas T1
        double charge_per_tick = (missing_charge / (double)drone.T1) * (TICK_US / 1000000.0);
        
        // Pêtla ³adowania
        for (int i = 0; i < charge_ticks && keep_running; i++) {
            // Sprawdzenie flagi opóŸnionej œmierci (Kamikadze)
            if (drone.kamikaze_pending) {
                dlog(C_RED "[Drone %d] Charging ABORTED due to KAMIKAZE order." C_RESET "\n", id);
                break; // Przerywamy ³adowanie, aby szybciej wylecieæ i wybuchn¹æ
            }
            
            usleep(TICK_US); // Czas p³ynie
            
            drone.current_battery += charge_per_tick; // Bateria roœnie
            if (drone.current_battery > 100.0) drone.current_battery = 100.0;
            
            // Loguj postêp co 1 sekundê (co 10 ticków), ¿eby nie zaœmiecaæ logów
            if (i % 10 == 0) dlog("[Drone %d] Charging: %.1f%%\n", id, drone.current_battery);
        }
        
        // Jeœli nie by³o przerwania, uznajemy bateriê za pe³n¹
        if (!drone.kamikaze_pending) drone.current_battery = BATTERY_FULL;
        
        // Inkrementacja licznika cykli ¿ycia
        drone.cycles_flown++;
        dlog("[Drone %d] Maintenance Log: Cycle %d/%d completed.\n", 
               id, drone.cycles_flown, drone.max_cycles);

start_from_base: // Etykieta dla dronów startuj¹cych w trybie "Baza"
        // --- ETAP 5: START ---
        drone.location = ST_INSIDE; // Upewnienie siê co do lokalizacji
        dlog("[Drone %d] Requesting TAKEOFF.\n", id);
        
        if (send_msg(MSG_REQ_TAKEOFF, id) == -1) break; // Proœba o start (typ 2)

        // Czekanie na zgodê - tutaj BLOKUJ¥CO (0 flag).
        // W bazie bateria nie spada drastycznie, a dron jest bezpieczny, wiêc mo¿e spaæ.
        if (msgrcv(msqid, &resp, sizeof(resp) - sizeof(long), RESPONSE_BASE + id, 0) == -1) {
             if (errno != EINTR) perror("[Drone] msgrcv blocking failed");
             break;
        }
        channel = resp.channel_id; // Otrzymano numer tunelu wyjœciowego

        // --- ETAP 6: WYLOT ---
        dlog(C_CYAN "[Drone %d] Crossing channel %d OUT..." C_RESET "\n", id, channel);
        sleep(CROSSING_TIME); // Symulacja przelotu (1s)

        send_msg(MSG_DEPARTED, id); // Informujemy Operatora: zwolniliœmy tunel i hangar
        drone.location = ST_OUTSIDE; // Jesteœmy na zewn¹trz (podatni na Kamikadze)
        
        dlog(C_CYAN "[Drone %d] Back in the air." C_RESET "\n", id);
        
        // --- ETAP 7: EWENTUALNY ZGON ---
        // Sprawdzenie czy mamy "zaleg³y" rozkaz samobójstwa
        if (drone.kamikaze_pending) {
            dlog(C_RED "[Drone %d] Mission complete. Detonating outside base." C_RESET "\n", id);
            drone_die(); // Wybuchamy bezpiecznie poza baz¹
        }
        
        // Sprawdzenie czy dron nie jest za stary (limit cykli)
        if (drone.cycles_flown >= drone.max_cycles) {
            dlog(C_YELLOW "[Drone %d] RETIRING: Wear limit reached (%d cycles). Goodbye." C_RESET "\n", id, drone.cycles_flown);
            drone_die(); // Z³omowanie (Replenish stworzy nowego)
        }
    }
    return 0; // Koniec main
}