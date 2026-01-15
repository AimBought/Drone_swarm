#ifndef IPC_WRAPPER_H
#define IPC_WRAPPER_H

#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/msg.h>

// Wrappery odporne na sygna³y (EINTR)
int safe_semop(int semid, struct sembuf *sops, size_t nsops);
ssize_t safe_msgrcv(int msqid, void *msgp, size_t msgsz, long msgtyp, int msgflg);

// Funkcja czasu (zamiast usleep)
void custom_wait(int semid, double seconds);

// Funkcje pomocnicze
int parse_int(const char *str, const char *name);

#endif