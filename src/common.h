#ifndef COMMON_H
#define COMMON_H

#include <sys/types.h>

#define MSGQ_KEY 0x1234
#define SEM_KEY  0x5678

// Typy komunikatów (Dron -> Operator)
#define MSG_REQ_LAND     1   // Proœba o l¹dowanie
#define MSG_REQ_TAKEOFF  2   // Proœba o start (wylot z bazy)
#define MSG_LANDED       3   // "Przelecia³em tunel i jestem w bazie"
#define MSG_DEPARTED     4   // "Przelecia³em tunel i jestem poza baz¹"
#define MSG_DEAD         5   // Awaria baterii

// Typ odpowiedzi (Operator -> Dron)
#define RESPONSE_BASE 1000

#define RESP_TEXT_SIZE 32

// Struktura ¿¹dania
struct msg_req {
    long mtype;
    int drone_id;
};

// Struktura odpowiedzi (Grant)
struct msg_resp {
    long mtype;
    int channel_id; // 0 lub 1 - informacja, którym tunelem lecimy
};

#endif