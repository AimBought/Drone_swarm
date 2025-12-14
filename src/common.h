/* include/common.h
 *
 * Plik nag³ówkowy zawieraj¹cy definicje wspólne dla wszystkich modu³ów:
 * - Klucze IPC (Kolejki, Semafory, Pamiêæ dzielona)
 * - Struktury wiadomoœci
 * - Makra kolorów do terminala
 */

#ifndef COMMON_H
#define COMMON_H

#include <sys/types.h>

// --- KOLORY ANSI (Dla czytelnoœci logów w terminalu) ---
#define C_RED     "\033[1;31m" // B³êdy, Œmieræ, Blokady
#define C_GREEN   "\033[1;32m" // Sukcesy, Start, L¹dowanie
#define C_YELLOW  "\033[1;33m" // Ostrze¿enia, Raporty
#define C_BLUE    "\033[1;34m" // Informacje systemowe
#define C_MAGENTA "\033[1;35m" // Sygna³y steruj¹ce
#define C_CYAN    "\033[1;36m" // Ruch dronów
#define C_RESET   "\033[0m"    // Reset koloru
// -------------------------------------------------------

// --- KLUCZE IPC (System V) ---
#define MSGQ_KEY 0x1234  // Kolejka komunikatów
#define SEM_KEY  0x5678  // Semafor (miejsca w hangarze)
#define SHM_KEY  0x9999  // Pamiêæ dzielona (mapa PID-ów)

// --- TYPY KOMUNIKATÓW ---
#define MSG_REQ_LAND     1 // Dron -> Operator: Proœba o l¹dowanie
#define MSG_REQ_TAKEOFF  2 // Dron -> Operator: Proœba o start
#define MSG_LANDED       3 // Dron -> Operator: Potwierdzenie wlotu
#define MSG_DEPARTED     4 // Dron -> Operator: Potwierdzenie wylotu
#define MSG_DEAD         5 // Dron -> Operator: Zg³oszenie zgonu
#define RESPONSE_BASE 1000 // Baza dla typów odpowiedzi (1000 + drone_id)

#define MAX_DRONE_ID 1024 // Maksymalne ID drona w systemie

// --- STRUKTURY DANYCH ---

// Pamiêæ dzielona: Pozwala Commanderowi namierzyæ PID drona po jego ID
struct SharedState {
    pid_t drone_pids[MAX_DRONE_ID];
};

// Struktura ¿¹dania (Dron -> Operator)
struct msg_req {
    long mtype;   // Typ komunikatu (np. MSG_REQ_LAND)
    int drone_id; // ID nadawcy
};

// Struktura odpowiedzi (Operator -> Dron)
struct msg_resp {
    long mtype;     // RESPONSE_BASE + drone_id (adresowane do konkretnego drona)
    int channel_id; // Przydzielony numer tunelu (0 lub 1)
};

#endif