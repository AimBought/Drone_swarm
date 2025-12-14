/* include/common.h */
#ifndef COMMON_H
#define COMMON_H

#include <sys/types.h>

// --- DEFINICJE KOLORÓW (ANSI) ---
#define C_RED     "\033[1;31m"
#define C_GREEN   "\033[1;32m"
#define C_YELLOW  "\033[1;33m"
#define C_BLUE    "\033[1;34m"
#define C_MAGENTA "\033[1;35m"
#define C_CYAN    "\033[1;36m"
#define C_RESET   "\033[0m"
// --------------------------------

#define MSGQ_KEY 0x1234
#define SEM_KEY  0x5678
#define SHM_KEY  0x9999

#define MSG_REQ_LAND     1
#define MSG_REQ_TAKEOFF  2
#define MSG_LANDED       3
#define MSG_DEPARTED     4
#define MSG_DEAD         5
#define RESPONSE_BASE 1000

#define MAX_DRONE_ID 1024

struct SharedState {
    pid_t drone_pids[MAX_DRONE_ID];
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