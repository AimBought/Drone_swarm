/* src/commander.c
 *
 * Modu3 Dowódcy (G3ówny proces).
 * Odpowiada za:
 * - Walidacje danych wejociowych (P < N/2)
 * - Inicjalizacje struktur IPC
 * - Uruchomienie Operatora i pocz1tkowych Dronów
 * - Interfejs u?ytkownika (komendy 1, 2, 3)
 * - Generowanie raportu koncowego
 */

#include <stdio.h>      // Biblioteka standardowa wejœcia/wyjœcia (printf, fopen, itp.)
#include <stdlib.h>     // Biblioteka standardowa (malloc, exit, atoi, strtol)
#include <unistd.h>     // Standardowe funkcje systemowe POSIX (fork, pipe, read, write, sleep)
#include <signal.h>     // Obs³uga sygna³ów (signal, kill, SIGINT, SIGUSR1)
#include <sys/wait.h>   // Funkcje do oczekiwania na procesy potomne (waitpid)
#include <sys/select.h> // Funkcja select do monitorowania deskryptorów plików (nieblokuj¹ce wejœcie)
#include <sys/shm.h>    // Funkcje pamiêci dzielonej Systemu V (shmget, shmat, shmctl)
#include <sys/resource.h> // Funkcje do zarz¹dzania limitami zasobów (getrlimit)
#include <string.h>     // Funkcje do operacji na stringach (memset, strstr, snprintf)
#include <stdarg.h>     // Obs³uga zmiennej liczby argumentów funkcji (va_list)
#include <time.h>       // Funkcje czasu (time, strftime)
#include <errno.h>      // Obs³uga b³êdów systemowych (zmienna errno)

#include "common.h"     // W³asny plik nag³ówkowy ze wspólnymi definicjami (struktury, sta³e)

// --- ZMIENNE GLOBALNE ---
static pid_t op_pid = -1; // Zmienna do przechowywania ID procesu (PID) Operatora
static int N_val = 0;     // Przechowuje docelow¹ liczbê dronów
// volatile sig_atomic_t zapewnia bezpieczny dostêp do zmiennej w handlerze sygna³u
static volatile sig_atomic_t stop_requested = 0; // Flaga steruj¹ca pêtl¹ g³ówn¹ (0=dzia³aj, 1=stop)
static struct SharedState *shared_mem = NULL;    // WskaŸnik do struktury w pamiêci dzielonej
static int shmid = -1;    // Identyfikator segmentu pamiêci dzielonej

// Handler sygna³u SIGINT (reakcja na Ctrl+C)
void sigint_handler(int sig) {
    (void)sig;          // Rzutowanie na void, aby unikn¹æ ostrze¿enia kompilatora o nieu¿ywanym parametrze
    stop_requested = 1; // Ustawienie flagi zatrzymania, co spowoduje wyjœcie z pêtli while w main
}

// Wrapper logowania - funkcja pomocnicza do wypisywania logów na ekran i do pliku
void cmd_log(const char *format, ...) {
    va_list args;       // Deklaracja listy argumentów dla funkcji o zmiennej liczbie parametrów
    va_start(args, format); // Inicjalizacja listy argumentów
    vprintf(format, args);  // Wypisanie sformatowanego tekstu na standardowe wyjœcie (konsolê)
    va_end(args);       // Zakoñczenie pracy z list¹ argumentów

    FILE *f = fopen("commander.txt", "a"); // Otwarcie pliku logów w trybie "append" (dopisywanie na koñcu)
    if (f) {            // Sprawdzenie, czy plik otworzy³ siê poprawnie
        time_t now = time(NULL); // Pobranie aktualnego czasu systemowego
        struct tm *t = localtime(&now); // Konwersja czasu na strukturê lokaln¹ (godziny, minuty...)
        char timebuf[32]; // Bufor na sformatowany czas
        strftime(timebuf, sizeof(timebuf), "%H:%M:%S", t); // Formatowanie czasu do stringa "HH:MM:SS"
        fprintf(f, "[%s] ", timebuf); // Zapisanie znacznika czasu do pliku
        va_start(args, format); // Ponowna inicjalizacja listy argumentów (trzeba to zrobiæ dla ka¿dego u¿ycia)
        vfprintf(f, format, args); // Zapisanie w³aœciwej treœci logu do pliku
        va_end(args);   // Sprz¹tanie po liœcie argumentów
        fclose(f);      // Zamkniêcie pliku, aby zapisaæ zmiany na dysku
    }
}

// Generowanie statystyk na podstawie logów operatora
void generate_report() {
    FILE *f = fopen("operator.txt", "r"); // Otwarcie pliku z logami operatora w trybie do odczytu
    if (!f) {           // Jeœli plik nie istnieje lub nie mo¿na go otworzyæ
        cmd_log(C_RED "\n[Commander] Could not open operator.txt for reporting." C_RESET "\n");
        return;         // Przerwij funkcjê raportowania
    }

    // Inicjalizacja liczników statystyk
    int landings = 0;
    int takeoffs = 0;
    int deaths = 0;
    int spawns = 0;
    int blocked = 0;
    
    char line[512]; // Bufor na pojedyncz¹ liniê z pliku
    // Pêtla czytaj¹ca plik linia po linii, dopóki fgets nie zwróci NULL (koniec pliku)
    while (fgets(line, sizeof(line), f)) {
        // strstr sprawdza, czy dany podci¹g wystêpuje w linii. Jeœli tak, inkrementujemy licznik.
        if (strstr(line, "GRANT LAND")) landings++;
        if (strstr(line, "GRANT TAKEOFF")) takeoffs++;
        if (strstr(line, "RIP")) deaths++;
        if (strstr(line, "REPLENISH")) spawns++;
        if (strstr(line, "BLOCKED")) blocked++;
    }
    fclose(f); // Zamkniêcie pliku po odczycie

    // Wypisanie sformatowanego raportu koñcowego przy u¿yciu funkcji cmd_log
    cmd_log(C_YELLOW "\n");
    cmd_log("========================================\n");
    cmd_log("       FINAL SIMULATION REPORT          \n");
    cmd_log("========================================\n");
    cmd_log(" Total Landings Granted:      %d\n", landings);
    cmd_log(" Total Takeoffs Granted:      %d\n", takeoffs);
    cmd_log(" Total Drone Deaths (RIP):    %d\n", deaths);
    cmd_log(" New Drones Spawned:          %d\n", spawns);
    cmd_log(" Entry Denials (Blocked):     %d\n", blocked);
    cmd_log("========================================" C_RESET "\n");
}

// Funkcja pomocnicza do bezpiecznej konwersji string -> int
int parse_int(const char *str, const char *name) {
    char *endptr;       // WskaŸnik na pierwszy znak, którego nie uda³o siê przekonwertowaæ
    errno = 0;          // Zerowanie zmiennej b³êdu przed wywo³aniem funkcji systemowej
    long val = strtol(str, &endptr, 10); // Konwersja stringa na long int w systemie dziesiêtnym

    // Sprawdzenie b³êdów: b³¹d zakresu (errno), œmieci na koñcu stringa (*endptr), wartoœæ niedodatnia
    if (errno != 0 || *endptr != '\0' || val <= 0) {
        fprintf(stderr, C_RED "Error: Invalid value for %s: '%s'. Must be a positive integer." C_RESET "\n", name, str);
        return -1;      // Zwrócenie kodu b³êdu
    }
    return (int)val;    // Zwrócenie przekonwertowanej wartoœci jako int
}

int main(int argc, char *argv[]) {
    // Sprawdzenie liczby argumentów wywo³ania programu
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <P> <N>\n", argv[0]); // Wypisanie instrukcji u¿ycia na wyjœcie b³êdów
        return 1;       // Zakoñczenie programu z kodem b³êdu
    }

    // 1. Walidacja czy to w ogóle liczby (strtol)
    int P = parse_int(argv[1], "P"); // Parsowanie pierwszego argumentu (pojemnoœæ hangaru)
    int N = parse_int(argv[2], "N"); // Parsowanie drugiego argumentu (liczba dronów)
    if (P == -1 || N == -1) return 1; // Jeœli parsowanie siê nie uda, koñczymy

    // --- WALIDACJA DANYCH ---
    if (P <= 0 || N <= 0) { // Sprawdzenie czy wartoœci s¹ dodatnie
        fprintf(stderr, "Error: P and N must be positive integers.\n");
        return 1;
    }
    if (2 * P >= N) { // Sprawdzenie warunku projektowego: P < N/2
        fprintf(stderr, "Error: Invalid parameters!\n");
        fprintf(stderr, "Condition P < N/2 is NOT met.\n");
        return 1;
    }

    // Walidacja limitów pamieci wspó3dzielonej w kontekœcie tablicy PID-ów
    if (N > MAX_DRONE_ID) {
        fprintf(stderr, C_RED "Error: N exceeds MAX_DRONE_ID (%d).\n" C_RESET, MAX_DRONE_ID);
        return 1;
    }

    // Walidacja limitów systemowych (ulimit)
    struct rlimit limit; // Struktura przechowuj¹ca limity zasobów
    if (getrlimit(RLIMIT_NPROC, &limit) == 0) { // Pobranie limitu liczby procesów dla u¿ytkownika
        // Liczymy potrzebne procesy: N (drony) + 1 (operator) + 1 (commander - my) + zapas na system
        int needed = N + 5; 
        if (needed > (int)limit.rlim_cur) { // Jeœli potrzebujemy wiêcej ni¿ system pozwala
            fprintf(stderr, C_RED "Error: Requested N=%d exceeds system process limit.\n", N);
            fprintf(stderr, "Your limit is %lu. Try a smaller N.\n" C_RESET, (unsigned long)limit.rlim_cur);
            return 1;
        }
    }

    N_val = N; // Przypisanie liczby dronów do zmiennej globalnej
    
    // Wyczyszczenie pliku logów commandera na starcie (otwarcie w trybie "w" kasuje zawartoœæ)
    FILE *f = fopen("commander.txt", "w"); if(f) fclose(f);

    // Utworzenie Pamieci Dzielonej (do mapowania ID -> PID)
    // shmget tworzy segment pamiêci. IPC_CREAT - utwórz jeœli nie ma. 0600 - prawa rw dla w³aœciciela.
    shmid = shmget(SHM_KEY, sizeof(struct SharedState), IPC_CREAT | 0600);
    if (shmid == -1) { perror("shmget"); return 1; } // Obs³uga b³êdu utworzenia pamiêci
    
    // shmat do³¹cza segment pamiêci do przestrzeni adresowej tego procesu
    shared_mem = (struct SharedState *)shmat(shmid, NULL, 0);
    if (shared_mem == (void *)-1) { perror("shmat"); return 1; } // Obs³uga b³êdu do³¹czenia
    memset(shared_mem, 0, sizeof(struct SharedState)); // Wyzerowanie ca³ej struktury w pamiêci dzielonej
    
    cmd_log(C_BLUE "[Commander] Shared Memory created." C_RESET "\n");

    // Rejestracja obs³ugi sygna³u SIGINT (Ctrl+C), aby wywo³aæ funkcjê sigint_handler
    signal(SIGINT, sigint_handler);

    // Uruchomienie Operatora
    op_pid = fork(); // Utworzenie nowego procesu (dziecka)
    if (op_pid == -1) { perror("fork operator"); return 1; } // B³¹d fork
    if (op_pid == 0) { // Kod wykonywany tylko w procesie dziecka (Operator)
        char argP[16], argN[16];
        snprintf(argP, sizeof(argP), "%d", P); // Konwersja P na string
        snprintf(argN, sizeof(argN), "%d", N); // Konwersja N na string
        // execl podmienia obraz procesu na program "operator". Przekazujemy argumenty.
        execl("./operator", "operator", argP, argN, NULL);
        perror("execl operator"); // To wykona siê tylko, jeœli execl zawiedzie
        exit(1); // Zabicie procesu dziecka w przypadku b³êdu
    }
    
    sleep(1); // Uœpienie Commandera na 1s, aby Operator zd¹¿y³ zainicjowaæ kolejki komunikatów i semafory

    // Uruchomienie pocz1tkowych Dronów (Start z powietrza: arg "0")
    for (int i = 0; i < N; ++i) {
        pid_t pid = fork(); // Utworzenie procesu dla drona
        if (pid == -1) { perror("fork drone"); break; } // B³¹d fork
        if (pid == 0) { // Kod wykonywany w procesie drona
            char idstr[16];
            snprintf(idstr, sizeof(idstr), "%d", i); // Konwersja ID drona na string
            // execl uruchamia program "drone". "0" oznacza start w trybie "powietrze".
            execl("./drone", "drone", idstr, "0", NULL);
            perror("execl drone"); // Jeœli execl zawiedzie
            exit(1);
        }
        // Zapisanie PID-u drona w pamiêci dzielonej (tylko w procesie rodzica - Commandera)
        if (i < MAX_DRONE_ID) shared_mem->drone_pids[i] = pid;
    }

    cmd_log(C_GREEN "[Commander] Launched P=%d, N=%d. Monitoring..." C_RESET "\n", P, N);
    cmd_log(C_BLUE "[Commander] Commands: '1'=Grow, '2'=Shrink, '3'=Attack, Ctrl+C=Exit" C_RESET "\n");

    // G3ówna petla steruj1ca (Non-blocking input)
    // Pêtla dzia³a dopóki flaga stop_requested (ustawiana przez Ctrl+C) wynosi 0
    while (!stop_requested) {
        fd_set fds;             // Zbiór deskryptorów plików do monitorowania
        FD_ZERO(&fds);          // Wyzerowanie zbioru
        FD_SET(STDIN_FILENO, &fds); // Dodanie standardowego wejœcia (klawiatury) do zbioru
        struct timeval tv = {1, 0}; // Ustawienie czasu oczekiwania (timeout) na 1 sekundê
        
        // select sprawdza, czy na wejœciu s¹ dane. Nie blokuje programu na sta³e (wraca po timeout).
        int ret = select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);
        
        // Jeœli select zwróci³ wartoœæ > 0 i nasze wejœcie jest aktywne
        if (ret > 0 && FD_ISSET(STDIN_FILENO, &fds)) {
            char buffer[128];
            // Odczyt danych z klawiatury do bufora
            int n = read(STDIN_FILENO, buffer, sizeof(buffer));
            if (n > 0) {
                if (buffer[0] == '1') { // Klawisz '1' - powiêkszenie roju
                    cmd_log(C_MAGENTA "[Commander] Sending SIGUSR1 (Grow)..." C_RESET "\n");
                    // Wys³anie sygna³u SIGUSR1 do Operatora (rozkaz ekspansji)
                    if (kill(op_pid, SIGUSR1) == -1) perror("kill USR1");
                } 
                else if (buffer[0] == '2') { // Klawisz '2' - zmniejszenie roju
                    cmd_log(C_MAGENTA "[Commander] Sending SIGUSR2 (Shrink)..." C_RESET "\n");
                    // Wys³anie sygna³u SIGUSR2 do Operatora (rozkaz redukcji)
                    if (kill(op_pid, SIGUSR2) == -1) perror("kill USR2");
                }
                else if (buffer[0] == '3') { // Klawisz '3' - zniszczenie konkretnego drona
                    printf("\n" C_RED "[Commander] ENTER TARGET DRONE ID: " C_RESET);
                    int target_id = -1;
                    // Pobranie ID drona od u¿ytkownika
                    if (scanf("%d", &target_id) == 1) {
                        if (target_id >= 0 && target_id < MAX_DRONE_ID) {
                            // Pobranie PID drona z pamiêci dzielonej na podstawie logicznego ID
                            pid_t target_pid = shared_mem->drone_pids[target_id];
                            if (target_pid > 0) { // Jeœli PID jest prawid³owy (dron ¿yje)
                                cmd_log(C_RED "[Commander] Targeting Drone %d (PID %d). Sending SIGUSR1..." C_RESET "\n", target_id, target_pid);
                                // Wys³anie sygna³u SIGUSR1 bezpoœrednio do drona (rozkaz kamikaze)
                                if (kill(target_pid, SIGUSR1) == -1) perror("kill USR1 drone");
                            } else {
                                cmd_log("[Commander] Drone %d not active.\n", target_id);
                            }
                        }
                    }
                    while (getchar() != '\n'); // Wyczyszczenie bufora wejœcia z nadmiarowych znaków (np. enter)
                }
            }
        }

        // Sprawdzenie czy operator ?yje
        int status;
        // waitpid z flag¹ WNOHANG sprawdza stan dzieci bez blokowania programu
        pid_t res = waitpid(-1, &status, WNOHANG);
        if (res > 0) { // Jeœli jakiœ proces potomny zakoñczy³ dzia³anie
            if (res == op_pid) { // Jeœli tym procesem by³ Operator (awaria krytyczna)
                cmd_log(C_RED "[Commander] Operator died unexpectedly!" C_RESET "\n");
                stop_requested = 1; // Wymuszenie zatrzymania symulacji
            }
        }
    }

    // --- CLEANUP ---
    cmd_log("\n[Commander] Stopping...\n");
    // Wys³anie sygna³u zakoñczenia (SIGINT) do Operatora, jeœli ¿yje
    if (op_pid > 0) kill(op_pid, SIGINT);
    
    // Pêtla wysy³aj¹ca sygna³ zakoñczenia do wszystkich aktywnych dronów
    for(int i=0; i<MAX_DRONE_ID; i++) {
        if (shared_mem->drone_pids[i] > 0) kill(shared_mem->drone_pids[i], SIGINT);
    }
    
    // Czekanie a¿ proces Operatora zakoñczy sprz¹tanie swoich zasobów
    waitpid(op_pid, NULL, 0);

    generate_report(); // Wygenerowanie raportu koñcowego z logów

    shmdt(shared_mem); // Od³¹czenie segmentu pamiêci dzielonej od procesu
    shmctl(shmid, IPC_RMID, NULL); // Oznaczenie segmentu pamiêci dzielonej do usuniêcia przez system
    cmd_log("[Commander] Cleanup complete. Bye.\n");
    return 0; // Wyjœcie z programu z kodem sukcesu
}