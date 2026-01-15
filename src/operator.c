/* src/operator.c
 *
 * Modu³ Operatora (Zarz¹dca Bazy).
 * Odpowiada za:
 * - Zarz¹dzanie semaforem (limit miejsc P)
 * - Obs³ugê kolejek FIFO (start/l¹dowanie)
 * - Dynamiczne skalowanie (Sygna³y 1 i 2)
 * - Monitorowanie stanu roju (spawn nowych dronów)
 */

#include <stdio.h>      // Standardowe wejœcie/wyjœcie (printf, fopen)
#include <stdlib.h>     // Biblioteka standardowa (exit, atoi, rand)
#include <string.h>     // Operacje na ci¹gach znaków (memset)
#include <unistd.h>     // Funkcje systemowe POSIX (fork, exec, sleep)
#include <signal.h>     // Obs³uga sygna³ów (signal, SIGUSR1, SIGCHLD)
#include <errno.h>      // Zmienna globalna errno do obs³ugi b³êdów
#include <time.h>       // Funkcje czasu (time, localtime - do logów)
#include <stdarg.h>     // Obs³uga zmiennej liczby argumentów (va_list do logowania)
#include <sys/wait.h>   // Funkcje oczekiwania na procesy (waitpid)
#include <sys/ipc.h>    // flagi IPC (IPC_CREAT, IPC_NOWAIT)
#include <sys/msg.h>    // Kolejki komunikatów (msgget, msgrcv, msgsnd)
#include <sys/sem.h>    // Semafory (semget, semop, semctl)
#include <sys/shm.h>    // Pamiêæ dzielona (shmget, shmat)
#include <sys/types.h>  // Definicje typów systemowych (pid_t, key_t)

#include "common.h"     // Wspólne definicje (klucze IPC, struktury wiadomoœci)

#include "../include/common.h"
#include "../include/ipc_wrapper.h"

// --- KONFIGURACJA ---
#define CHANNELS 2      // Liczba dostêpnych tuneli (bramek)
#define DIR_NONE 0      // Tunel jest pusty / nieaktywny
#define DIR_IN   1      // Tunel wpuszcza drony (L¹dowanie)
#define DIR_OUT  2      // Tunel wypuszcza drony (Start)
#define CHECK_INTERVAL 5 // Co ile sekund sprawdzaæ stan roju (czy nie trzeba dodaæ nowych dronów)

// Stan tuneli - Operator musi pamiêtaæ, co siê dzieje w ka¿dym tunelu
int chan_dir[CHANNELS];   // Kierunek ruchu w tunelu [i] (IN/OUT/NONE)
int chan_users[CHANNELS]; // Liczba dronów aktualnie przebywaj¹cych w tunelu [i]

// Kolejki oczekuj¹cych (0=L¹dowanie, 1=Start)
// U¿ywamy bufora cyklicznego, aby efektywnie zarz¹dzaæ kolejk¹ FIFO bez przesuwania pamiêci
#define WAITQ_CAP 1024
int waitq[2][WAITQ_CAP];  // Dwie kolejki: waitq[0] dla l¹duj¹cych, waitq[1] dla startuj¹cych
int q_head[2] = {0, 0};   // Indeks "g³owy" (st¹d pobieramy drony do obs³u¿enia)
int q_tail[2] = {0, 0};   // Indeks "ogona" (tu wpisujemy nowe oczekuj¹ce drony)

// --- ZMIENNE GLOBALNE ---
static int msqid = -1;    // ID kolejki komunikatów (IPC)
static int semid = -1;    // ID zestawu semaforów (IPC) - kontroluje miejsca w hangarze
static int shmid = -1;    // ID pamiêci dzielonej (IPC) - przechowuje PID-y dronów
static struct SharedState *shared_mem = NULL; // WskaŸnik do pod³¹czonej pamiêci dzielonej
// Zmienna steruj¹ca pêtl¹ g³ówn¹. 'volatile' oznacza, ¿e mo¿e siê zmieniæ z zewn¹trz (przez sygna³)
static volatile sig_atomic_t keep_running = 1;

// Flagi sygna³ów (Async-safe)
// Ustawiane w handlerach, sprawdzane w pêtli g³ównej. Zapobiega to wyœcigom i b³êdom w funkcjach I/O.
static volatile sig_atomic_t flag_sig1 = 0; 
static volatile sig_atomic_t flag_sig2 = 0; 

// Stan bazy
static int current_P = 0;        // Aktualna pojemnoœæ hangaru (wartoœæ logiczna)
static int pending_removal = 0;  // Liczba miejsc do usuniêcia, gdy drony wylec¹ (D³ug Techniczny przy skalowaniu w dó³)
static int signal1_used = 0;     // Zabezpieczenie: Boost (powiêkszenie bazy) mo¿liwy tylko raz

static volatile int target_N = 0; // Docelowa liczba dronów (któr¹ utrzymuje Operator)
static int current_active = 0;    // Liczba aktualnie ¿ywych dronów (zarejestrowanych)
static int next_drone_id = 0;     // Pomocnicza zmienna do szukania wolnych ID

// --- LOGOWANIE ---
// Funkcja zapisuj¹ca logi do pliku operator.txt z dat¹ i godzin¹
void olog(const char *format, ...) {
    va_list args;           // Lista argumentów dla funkcji o zmiennej liczbie parametrów
    va_start(args, format); // Inicjalizacja listy
    vprintf(format, args);  // Wypisanie na konsolê
    va_end(args);           // Czyszczenie

    FILE *f = fopen("operator.txt", "a"); // Otwarcie pliku w trybie dopisywania ("append")
    if (f) {
        time_t now = time(NULL);        // Pobranie czasu
        struct tm *t = localtime(&now); // Konwersja na czas lokalny
        char timebuf[32];
        strftime(timebuf, sizeof(timebuf), "%H:%M:%S", t); // Formatowanie godziny
        fprintf(f, "[%s] ", timebuf);   // Zapis znacznika czasu
        va_start(args, format);         // Ponowne pobranie argumentów dla pliku
        vfprintf(f, format, args);      // Zapis treœci logu
        va_end(args);
        fclose(f);                      // Zamkniêcie pliku
    }
}

// --- HANDLERY SYGNA£ÓW ---
// Handlery s¹ minimalistyczne - ustawiaj¹ tylko flagê. Ca³a ciê¿ka praca dzieje siê w main().
void sigusr1_handler(int sig) { (void)sig; flag_sig1 = 1; } // Reakcja na sygna³ "1" (Grow)
void sigusr2_handler(int sig) { (void)sig; flag_sig2 = 1; } // Reakcja na sygna³ "2" (Shrink)

// Handler SIGCHLD - zapobiega powstawaniu procesów Zombie
void sigchld_handler(int sig) {
    (void)sig;
    int saved_errno = errno; // Zapisujemy errno, bo waitpid mo¿e je zmieniæ, co zepsu³oby logikê w main
    // waitpid z WNOHANG sprz¹ta wszystkie martwe dzieci bez blokowania programu
    while (waitpid(-1, NULL, WNOHANG) > 0); 
    errno = saved_errno;     // Przywracamy errno
}

// Handler SIGINT (Ctrl+C) - bezpieczne zatrzymanie pêtli
void cleanup(int sig) { (void)sig; keep_running = 0; }

// --- OBS£UGA KOLEJEK (Circular Buffer) ---
// Dodanie drona do kolejki oczekuj¹cych
void enqueue(int type, int id) {
    // Obliczamy nowy indeks ogona (modulo zapewnia cyklicznoœæ - powrót do 0 po osi¹gniêciu koñca tablicy)
    int next = (q_tail[type] + 1) % WAITQ_CAP;
    if (next == q_head[type]) return; // Jeœli ogon dogoni³ g³owê -> kolejka jest pe³na, odrzucamy
    waitq[type][q_tail[type]] = id;   // Zapisanie ID drona
    q_tail[type] = next;              // Przesuniêcie ogona
}

// Pobranie drona z kolejki
int dequeue(int type) {
    // Dopóki kolejka nie jest pusta (g³owa != ogon)
    while (q_head[type] != q_tail[type]) {
        int id = waitq[type][q_head[type]]; // Pobranie ID spod g³owy
        q_head[type] = (q_head[type] + 1) % WAITQ_CAP; // Przesuniêcie g³owy
        if (id != -1) return id; // Zwróæ ID jeœli dron jest "¿ywy" (nie ma flagi -1)
    }
    return -1; // Kolejka pusta
}

// Usuwanie martwego drona z kolejki (Lazy Deletion)
void remove_dead(int id) {
    for (int t = 0; t < 2; t++) { // Sprawdzamy obie kolejki (Start/L¹dowanie)
        int i = q_head[t];
        while (i != q_tail[t]) { // Przegl¹damy ca³¹ zajêt¹ czêœæ bufora
            // Jeœli znajdziemy ID martwego drona, zamazujemy go "-1"
            // Funkcja dequeue pominie te wartoœci. To szybsze ni¿ przesuwanie ca³ej tablicy.
            if (waitq[t][i] == id) waitq[t][i] = -1;
            i = (i + 1) % WAITQ_CAP;
        }
    }
}

// --- ZARZ¥DZANIE SEMAFOREM (MIEJSCA W BAZIE) ---

// Próba zajêcia miejsca w hangarze (operacja P / -1)
int reserve_hangar_spot() {
    // Struktura operacji: semafor nr 0, odejmij 1, flaga IPC_NOWAIT
    struct sembuf op = {0, -1, IPC_NOWAIT};
    // semop wykonuje operacjê. Jeœli semafor == 0, IPC_NOWAIT sprawia, ¿e funkcja wraca z b³êdem EAGAIN
    if (safe_semop(semid, &op, 1) == -1) {
        if (errno != EAGAIN) { // Inny b³¹d ni¿ "brak miejsc" jest krytyczny
            perror("[Operator] semop reserve failed");
        }
        return 0; // Fail - brak miejsc
    }
    return 1; // Success - miejsce zarezerwowane
}

// Zwolnienie miejsca z obs³ug¹ "Pending Removal" (Sygna³ 2)
void free_hangar_spot() {
    // Czy Commander kaza³ zmniejszyæ bazê i mamy d³ug techniczny?
    if (pending_removal > 0) {
        // Zamiast oddawaæ miejsce, niszczymy je (sp³acamy d³ug)
        // Dron wylecia³, miejsce siê zwolni³o, ale my NIE zwiêkszamy semafora.
        pending_removal--;
        olog(C_MAGENTA "[Operator] Platform dismantled after departure. Pending: %d" C_RESET "\n", pending_removal);
    } else {
        // Normalne zwolnienie: semafor nr 0, dodaj 1 (+1)
        struct sembuf op = {0, 1, 0};
        if (safe_semop(semid, &op, 1) == -1) {
            perror("[Operator] semop free failed");
        }
    }
}

// Pobranie aktualnej wartoœci semafora (liczby wolnych miejsc) bez zmieniania go
int get_hangar_free_slots() {
    // GETVAL to komenda "odczytaj wartoœæ"
    int val = semctl(semid, 0, GETVAL);
    if (val == -1) perror("[Operator] semctl GETVAL failed");
    return val;
}

// --- DYNAMICZNE SKALOWANIE ---

// Funkcja obs³uguj¹ca rozkaz '1' - powiêkszenie bazy
void increase_base_capacity() {
    if (signal1_used) { // Zabezpieczenie przed wielokrotnym u¿yciem
        olog(C_YELLOW "[Operator] Signal 1 IGNORED (One-time use only)." C_RESET "\n");
        return;
    }
    
    // Obliczamy, ile dronów mielibyœmy po powiêkszeniu
    int potential_new_N = target_N * 2;

    // Sprawdzamy czy nie przekroczymy limitu tablicy w pamiêci dzielonej
    if (potential_new_N > MAX_DRONE_ID) {
        olog(C_RED "[Operator] Signal 1 DENIED: Doubling population (%d -> %d) exceeds system limit (%d)." C_RESET "\n", 
             target_N, potential_new_N, MAX_DRONE_ID);
        return; // Przerywamy! Nie zmieniamy semafora ani N.
    }

    int added_slots = current_P; // Chcemy podwoiæ, wiêc dodajemy drugie tyle miejsc
    current_P *= 2;
    target_N *= 2;
    signal1_used = 1;

    // Fizyczne zwiêkszenie semafora o 'added_slots' (operacja V o wartoœci > 1)
    struct sembuf op = {0, added_slots, 0};
    if (safe_semop(semid, &op, 1) == -1) { 
        perror("[Operator] semop increase failed");
    }
    
    olog(C_BLUE "[Operator] !!! BASE EXPANDED !!! New P=%d, New Target N=%d" C_RESET "\n", current_P, target_N);
}

// Funkcja obs³uguj¹ca rozkaz '2' - pomniejszenie bazy
void decrease_base_capacity() {
    if (current_P <= 1) { // Nie mo¿emy zejœæ do 0 miejsc
        olog(C_YELLOW "[Operator] Signal 2 IGNORED (Minimum P=1 reached)." C_RESET "\n");
        return;
    }

    int remove_cnt = current_P / 2; // Ile miejsc chcemy usun¹æ (po³owa)
    current_P -= remove_cnt;
    target_N /= 2;
    if (target_N < 1) target_N = 1;

    // Próba usuniêcia wolnych slotów - sprawdzamy ile jest pustych
    int free_slots = get_hangar_free_slots();
    // Mo¿emy usun¹æ natychmiast tyle, ile jest wolnych, ale nie wiêcej ni¿ planujemy
    int immediate_remove = (free_slots >= remove_cnt) ? remove_cnt : free_slots;
    // Reszta to "d³ug" - musimy poczekaæ a¿ drony wylec¹
    int deferred_remove = remove_cnt - immediate_remove;
    
    // Usuwamy co siê da od razu (zmniejszamy semafor)
    if (immediate_remove > 0) {
        struct sembuf op = {0, -immediate_remove, IPC_NOWAIT};
        if (safe_semop(semid, &op, 1) == -1) {
            perror("[Operator] semop decrease failed");
        }
    }
    
    // Reszta "wisi" do usuniêcia w funkcji free_hangar_spot()
    pending_removal += deferred_remove;

    olog(C_MAGENTA "[Operator] !!! BASE SHRINKING !!! New P=%d, Target N=%d. Removed now: %d, Pending: %d" C_RESET "\n", 
         current_P, target_N, immediate_remove, pending_removal);
}

// --- LOGIKA TUNELI ---
// Znajduje ID tunelu pasuj¹cego do ¿¹danego kierunku
int find_available_channel(int needed_dir) {
    // Priorytet 1: Szukamy tunelu, który ju¿ dzia³a w tym kierunku (efekt konwoju)
    for (int i = 0; i < CHANNELS; i++) {
        if (chan_dir[i] == needed_dir) return i; // Istniej¹cy kierunek
    }
    // Priorytet 2: Szukamy ca³kowicie wolnego tunelu
    for (int i = 0; i < CHANNELS; i++) {
        if (chan_dir[i] == DIR_NONE) return i; // Wolny kana³
    }
    return -1; // Brak dostêpnych tuneli
}

// Wys³anie wiadomoœci "Grant" (Zgoda) do drona
void send_grant(int id, int channel) {
    struct msg_resp resp;
    resp.mtype = RESPONSE_BASE + id; // Typ wiadomoœci = unikalny kana³ drona (np. 10005)
    resp.channel_id = channel;       // Przydzielony numer tunelu
    // msgsnd wrzuca wiadomoœæ do kolejki. Odejmujemy sizeof(long) bo mtype siê nie liczy do rozmiaru danych.
    if (msgsnd(msqid, &resp, sizeof(resp) - sizeof(long), 0) == -1) {
        perror("[Operator] msgsnd grant failed");
    }
}

// Przetwarzanie oczekuj¹cych dronów (Scheduler)
void process_queues() {
    // 1. Obs³uga wylotów (START) - maj¹ priorytet, bo zwalniaj¹ miejsca w hangarze
    int cid_out = find_available_channel(DIR_OUT); // Szukamy tunelu na zewn¹trz
    if (cid_out != -1) {
        int id = dequeue(1); // Pobieramy drona z kolejki startowej
        if (id != -1) {
            // Aktualizujemy stan tunelu i wysy³amy zgodê
            chan_dir[cid_out] = DIR_OUT; chan_users[cid_out]++; send_grant(id, cid_out);
            olog(C_GREEN "[Operator] GRANT TAKEOFF drone %d via Channel %d" C_RESET "\n", id, cid_out);
        }
    }
    
    // Jeœli populacja jest za du¿a (trwa redukcja bazy), blokujemy l¹dowania
    if (current_active > target_N) return; 

    // 2. Obs³uga wlotów (L¥DOWANIE) - Tylko jeœli s¹ fizyczne miejsca w hangarze
    if (get_hangar_free_slots() > 0) {
        int cid_in = find_available_channel(DIR_IN); // Szukamy tunelu do œrodka
        if (cid_in != -1) {
            int id = dequeue(0); // Pobieramy drona z kolejki l¹dowania
            if (id != -1) {
                // Próba rezerwacji semafora (krytyczne!)
                if (reserve_hangar_spot()) {
                    // Sukces: mamy tunel I mamy miejsce. Wpuszczamy.
                    chan_dir[cid_in] = DIR_IN; chan_users[cid_in]++; send_grant(id, cid_in);
                    olog(C_GREEN "[Operator] GRANT LAND %d via Ch %d" C_RESET "\n", id, cid_in);
                } else enqueue(0, id); // Powrót do kolejki jeœli reserve_hangar_spot zawiód³ (wyœcig)
            }
        }
    }
}

// Tworzenie nowego drona (Wewn¹trz bazy) - Funkcja "Replenish"
void spawn_new_drone() {
    // Najpierw musimy zarezerwowaæ miejsce w hangarze dla nowego drona
    if (!reserve_hangar_spot()) {
        olog(C_RED "[Operator] ERROR: Tried to spawn drone inside base, but reserve failed!" C_RESET "\n");
        return; 
    }

    // Szukamy pierwszego wolnego ID w "ksi¹¿ce adresowej" (Pamiêæ Dzielona)
    // Recykling ID po zmar³ych dronach.
    int new_id = -1;
    if (shared_mem != NULL) {
        for (int i = 0; i < MAX_DRONE_ID; i++) {
            if (shared_mem->drone_pids[i] == 0) { // Slot wolny (0)
                new_id = i;
                break;
            }
        }
    }

    // Zabezpieczenie na wypadek braku wolnych slotów ID (limit 1024)
    if (new_id == -1) {
        olog(C_RED "[Operator] CRITICAL: No free ID slots in Shared Memory (Limit %d reached)!" C_RESET "\n", MAX_DRONE_ID);
        // Musimy oddaæ semafor (Rollback), bo jednak nie tworzymy drona!
        struct sembuf op = {0, 1, 0};
        safe_semop(semid, &op, 1);
        return;
    }

    // Tworzenie procesu
    pid_t pid = fork();
    if (pid == -1) {
        perror("[Operator] fork failed");
        // Rollback semafora w przypadku b³êdu fork
        struct sembuf op = {0, 1, 0};
        safe_semop(semid, &op, 1);
        return;
    }
    
    if (pid == 0) { // Proces dziecka (Nowy Dron)
        char idstr[16];
        snprintf(idstr, sizeof(idstr), "%d", new_id);
        // execl uruchamia program drona. Argument "1" oznacza "Startuj w bazie (tryb respawn)"
        execl("./drone", "drone", idstr, "1", NULL); 
        perror("[Operator] execl drone failed");
        exit(1);
    } else if (pid > 0) { // Proces rodzica (Operator)
        olog(C_BLUE "[Operator] REPLENISH: Spawned drone %d INSIDE BASE (pid %d). Slot recycled." C_RESET "\n", new_id, pid);
        current_active++; // Aktualizacja licznika ¿ywych dronów
        if (shared_mem != NULL) {
            shared_mem->drone_pids[new_id] = pid; // Rejestracja PID w pamiêci dzielonej
        }
    }
}

// --- MAIN LOOP ---

int main(int argc, char *argv[]) {
    // Sprawdzenie argumentów (Pojemnoœæ, Liczba Dronów) przekazanych przez Commandera
    if (argc < 3) return 1;
    int P = atoi(argv[1]);
    target_N = atoi(argv[2]);
    
    // Inicjalizacja zmiennych stanu
    current_P = P;
    current_active = target_N;
    next_drone_id = target_N; 
    
    // Wyczyszczenie pliku logów operatora
    FILE *f = fopen("operator.txt", "w"); if(f) fclose(f);

    // Rejestracja sygna³ów systemowych
    if (signal(SIGINT, cleanup) == SIG_ERR) perror("signal SIGINT"); // Sprz¹tanie
    if (signal(SIGCHLD, sigchld_handler) == SIG_ERR) perror("signal SIGCHLD"); // Sprz¹tanie zombie
    if (signal(SIGUSR1, sigusr1_handler) == SIG_ERR) perror("signal SIGUSR1"); // Rozkaz Grow
    if (signal(SIGUSR2, sigusr2_handler) == SIG_ERR) perror("signal SIGUSR2"); // Rozkaz Shrink

    // Inicjalizacja IPC - Kolejka Komunikatów
    msqid = msgget(MSGQ_KEY, IPC_CREAT | 0600);
    if (msqid == -1) { perror("msgget failed"); return 1; }

    // Inicjalizacja IPC - Semafory
    semid = semget(SEM_KEY, SEM_COUNT, IPC_CREAT | 0600);
    if (semid == -1) { perror("semget failed"); return 1; }

    // Inicjalizacja IPC - Pamiêæ Dzielona
    shmid = shmget(SHM_KEY, sizeof(struct SharedState), 0600);
    if (shmid != -1) {
        shared_mem = (struct SharedState *)shmat(shmid, NULL, 0); // Pod³¹czenie pamiêci
        if (shared_mem == (void *)-1) {
             perror("shmat failed");
             shared_mem = NULL;
        } else olog("[Operator] Attached to Shared Memory.\n");
    }
    
    // Ustawienie pocz¹tkowej wartoœci semafora na P (liczba miejsc)
    union semun { int val; struct semid_ds *buf; unsigned short *array; } arg;
    // 1. Semafor Hangar (Indeks 0) = P
    arg.val = P;
    if (semctl(semid, SEM_HANGAR, SETVAL, arg) == -1) { perror("semctl SETVAL HANGAR"); return 1; }

    // 2. Semafor Timer (Indeks 1) = 0
    arg.val = 0;
    if (semctl(semid, SEM_TIMER, SETVAL, arg) == -1) { perror("semctl SETVAL TIMER"); return 1; }

    // Wyzerowanie stanu tuneli
    for(int i=0; i<CHANNELS; i++) { chan_dir[i]=DIR_NONE; chan_users[i]=0; }

    olog(C_GREEN "[Operator] Ready. P=%d, Target N=%d." C_RESET "\n", P, target_N);
    time_t last_check = time(NULL);

    // G£ÓWNA PÊTLA OPERATORA (EVENT LOOP)
    while (keep_running) {
        // Obs³uga flag sygna³ów (Asynchroniczne zdarzenia od Commandera)
        if (flag_sig1) { increase_base_capacity(); flag_sig1 = 0; }
        if (flag_sig2) { decrease_base_capacity(); flag_sig2 = 0; }

        // Okresowe sprawdzanie stanu (Replenish / Watchdog)
        time_t now = time(NULL);
        if (now - last_check >= CHECK_INTERVAL) {
            last_check = now;
            
            // Watchdog: Fix na martwe semafory (reset jeœli pusto i brak d³ugu)
            // Zapobiega sytuacji, gdzie semafor "zgubi³" wartoœæ przez b³¹d drona
            if (current_active == 0 && get_hangar_free_slots() < current_P && pending_removal == 0) {
                union semun arg; arg.val = current_P; 
                if (semctl(semid, 0, SETVAL, arg) == -1) perror("semctl RESET failed");
                else olog(C_YELLOW "[Operator] Reset semaphore to %d." C_RESET "\n", current_P);
            }
            // Logika Replenish: Spawnowanie nowych dronów, jeœli populacja spad³a poni¿ej celu
            if (current_active < target_N) {
                int needed = target_N - current_active; // Ilu brakuje
                int free_slots = get_hangar_free_slots(); // Ile jest miejsca
                if (free_slots > 0) {
                    olog(C_BLUE "[Operator] CHECK: Spawning inside base..." C_RESET "\n");
                    // Tworzymy tyle ile brakuje, ale nie wiêcej ni¿ jest miejsc w hangarze
                    int to_spawn = (needed < free_slots) ? needed : free_slots;
                    for (int k=0; k<to_spawn; k++) spawn_new_drone();
                } 
            }
        }

        // Odbiór wiadomoœci z kolejki
        struct msg_req req;
        // msgrcv z flag¹ IPC_NOWAIT - nie blokuje pêtli, jeœli brak wiadomoœci.
        // -MSG_DEAD oznacza odbiór priorytetowy: wiadomoœci o typie <= MSG_DEAD (czyli 1..5)
        ssize_t r = safe_msgrcv(msqid, &req, sizeof(req) - sizeof(long), -MSG_DEAD, IPC_NOWAIT);
        
        if (r == -1) {
            // Jeœli brak wiadomoœci (ENOMSG), œpimy chwilê, ¿eby nie obci¹¿aæ CPU (Busy Waiting prevention)
            if (errno == ENOMSG) { custom_wait(semid, 0.05); continue; }
            if (errno != EINTR) { perror("[Operator] msgrcv failed"); break; }
            continue;
        }

        int did = req.drone_id;

        // Maszyna stanów komunikatów - Reakcja na typ wiadomoœci
        switch (req.mtype) {
            case MSG_REQ_LAND: // Dron prosi o l¹dowanie
                if (current_active > target_N) { 
                    // Jeœli trwa redukcja populacji, blokujemy l¹dowanie (naturalne wygaszanie)
                    enqueue(0, did); 
                    olog(C_RED "[Operator] BLOCKED %d" C_RESET "\n", did); 
                }
                else if (get_hangar_free_slots() > 0) { // Czy jest miejsce w hangarze?
                    int ch = find_available_channel(DIR_IN); // Czy jest wolny tunel?
                    // Jeœli mamy tunel ORAZ uda siê zarezerwowaæ semafor
                    if (ch != -1 && reserve_hangar_spot()) {
                        chan_dir[ch] = DIR_IN; chan_users[ch]++; send_grant(did, ch);
                        olog(C_GREEN "[Operator] GRANT LAND %d via Ch %d" C_RESET "\n", did, ch);
                    } else enqueue(0, did); // Jak nie, do kolejki
                } else enqueue(0, did); // Jak nie ma miejsca, do kolejki
                break;
                
            case MSG_REQ_TAKEOFF: // Dron prosi o start
                {
                    int ch = find_available_channel(DIR_OUT); // Czy jest tunel na zewn¹trz?
                    if (ch != -1) {
                        chan_dir[ch] = DIR_OUT; chan_users[ch]++; send_grant(did, ch);
                        olog(C_GREEN "[Operator] GRANT TAKEOFF %d via Ch %d" C_RESET "\n", did, ch);
                    } else enqueue(1, did); // Jak nie, do kolejki startowej
                }
                break;
                
            case MSG_LANDED: // Dron wlecia³ do œrodka (zwolni³ tunel, zaj¹³ hangar)
                {
                    int found = 0;
                    // Szukamy, którym tunelem wlecia³ i zwalniamy licznik w tym tunelu
                    for(int i=0; i<CHANNELS; i++) {
                        if (chan_dir[i] == DIR_IN && chan_users[i] > 0) {
                            chan_users[i]--; if (chan_users[i] == 0) chan_dir[i] = DIR_NONE;
                            olog(C_CYAN "[Operator] Drone %d entered base." C_RESET "\n", did);
                            found = 1; break;
                        }
                    }
                    if (!found) olog(C_RED "[Operator] WARN: Unexpected LANDED from %d" C_RESET "\n", did);
                    process_queues(); // Zwolnienie tunelu mog³o odblokowaæ innych - sprawdzamy kolejki
                }
                break;
                
            case MSG_DEPARTED: // Dron wylecia³ (zwolni³ tunel i hangar)
                {
                    int found = 0;
                    // Zwalniamy tunel
                    for(int i=0; i<CHANNELS; i++) {
                        if (chan_dir[i] == DIR_OUT && chan_users[i] > 0) {
                            chan_users[i]--; if (chan_users[i] == 0) chan_dir[i] = DIR_NONE;
                            olog(C_CYAN "[Operator] Drone %d left." C_RESET "\n", did);
                            found = 1; break;
                        }
                    }
                    if (!found) olog(C_RED "[Operator] ERROR: Got MSG_DEPARTED but no channel active OUT!" C_RESET "\n");
                    free_hangar_spot(); // Zwolnienie semafora (lub obs³uga pending removal)
                    process_queues();   // Zwolnienie miejsca mog³o odblokowaæ l¹duj¹cych - sprawdzamy kolejki
                }
                break;
                
            case MSG_DEAD: // Dron zg³asza œmieræ
                olog(C_RED "[Operator] RIP drone %d." C_RESET "\n", did);
                remove_dead(did); // Usuwamy go z kolejek oczekuj¹cych (¿eby nie wywo³ywaæ duchów)
                current_active--; // Zmniejszamy licznik populacji
                if (shared_mem != NULL && did < MAX_DRONE_ID) shared_mem->drone_pids[did] = 0; // Czyœcimy slot PID
                olog(C_BLUE "[Operator] Active: %d/%d" C_RESET "\n", current_active, target_N);
                break;
        }
    }

    // Sprz¹tanie po wyjœciu z pêtli (Ctrl+C)
    if (shared_mem) shmdt(shared_mem); // Od³¹czenie pamiêci
    if (msqid != -1) msgctl(msqid, IPC_RMID, NULL); // Usuniêcie kolejki
    if (semid != -1) semctl(semid, 0, IPC_RMID);    // Usuniêcie semaforów
    return 0;
}