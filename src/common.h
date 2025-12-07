#ifndef COMMON_H
#define COMMON_H

#include <sys/types.h>

#define MSGQ_KEY 0x1234
#define SEM_KEY  0x5678
#define SHM_KEY  0x9999

#define MSG_REQ_LAND     1
#define MSG_REQ_TAKEOFF  2
#define MSG_LANDED       3
#define MSG_DEPARTED     4
#define MSG_DEAD         5
#define RESPONSE_BASE 1000

#define RESP_TEXT_SIZE 32

// Maksymalna liczba dronów jak¹ obs³u¿ymy w rejestrze (np. ID od 0 do 1023)
#define MAX_DRONE_ID 1024

// Struktura, która bêdzie siedzieæ w pamiêci dzielonej
struct SharedState {
    pid_t drone_pids[MAX_DRONE_ID]; // Tablica PID-ów indeksowana przez ID drona
};

struct msg_req {
    long mtype;
    int drone_id;
};

struct msg_resp {
    long mtype;
    int channel_id;
};

#endif