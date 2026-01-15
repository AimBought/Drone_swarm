/* include/common.h
 *
 * Plik nag³ówkowy zawieraj¹cy definicje wspólne dla wszystkich modu³ów:
 * - Klucze IPC (Kolejki, Semafory, Pamiêæ dzielona)
 * - Struktury wiadomoœci
 * - Makra kolorów do terminala
 */

#ifndef COMMON_H
#define COMMON_H

#define _GNU_SOURCE // semtimedop

#include <sys/types.h>
// --- DODATKOWE INCLUDY DLA WRAPPERÓW ---
#include <errno.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/sem.h>

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

// --- WRAPPERY ODPORNE NA SYGNA£Y (EINTR) ---

// Bezpieczny semop (operacje na semaforach)
static inline int safe_semop(int semid, struct sembuf *sops, size_t nsops) {
    int res;
    do {
        res = semop(semid, sops, nsops);
    } while (res == -1 && errno == EINTR);
    return res;
}

// Bezpieczny msgrcv (odbieranie wiadomoœci)
static inline ssize_t safe_msgrcv(int msqid, void *msgp, size_t msgsz, long msgtyp, int msgflg) {
    ssize_t res;
    do {
        res = msgrcv(msqid, msgp, msgsz, msgtyp, msgflg);
    } while (res == -1 && errno == EINTR);
    return res;
}

// --- ZARZ¥DZANIE CZASEM (SEMAFORY) ---

// Indeksy w tablicy semaforów
#define SEM_HANGAR 0  // Ten semafor pilnuje miejsc w bazie (dzia³a jak wczeœniej)
#define SEM_TIMER  1  // Ten semafor s³u¿y tylko do odmierzania czasu (zawsze 0)
#define SEM_COUNT  2  // Ca³kowita liczba semaforów

// Funkcja zastêpuj¹ca usleep/sleep.
// Wykorzystuje semtimedop na semaforze SEM_TIMER (który ma wartoœæ 0).
// Próba wykonania operacji -1 spowoduje zablokowanie procesu na okreœlony czas.
static inline void custom_wait(int semid, double seconds) {
    struct timespec ts;
    ts.tv_sec = (time_t)seconds;
    ts.tv_nsec = (long)((seconds - ts.tv_sec) * 1000000000L);

    // Operacja na semaforze nr 1 (Timer), spróbuj odj¹æ 1 (-1)
    struct sembuf op = {SEM_TIMER, -1, 0};
    
    // To wywo³anie zawsze zwróci b³¹d (bo semafor ma 0), ale po up³ywie czasu.
    // Jeœli zwróci -1 i errno=EAGAIN -> Czas min¹³ (Sukces symulacji).
    // Jeœli zwróci -1 i errno=EINTR  -> Przerwano sygna³em (np. Kamikaze).
    semtimedop(semid, &op, 1, &ts);
}

#endif