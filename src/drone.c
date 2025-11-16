#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include "common.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: drone <id>\n");
        return 1;
    }

    int drone_id = atoi(argv[1]);

    printf("[Drone %d] Starting...\n", drone_id);

    sleep(1); // symulacja startu

    int fd;
    int attempts = 5;

    // Dron czeka na operatora (który musi otworzyæ FIFO do odczytu)
    while ((fd = open(FIFO_PATH, O_WRONLY)) == -1 && attempts-- > 0) {
        printf("[Drone %d] Waiting for operator...\n", drone_id);
        sleep(1);
    }

    if (fd == -1) {
        perror("[Drone] Failed to open FIFO");
        return 1;
    }

    // Budujemy wiadomoœæ do operatora
    char msg[64];
    snprintf(msg, sizeof(msg), "Hello from drone %d\n", drone_id);

    if (write(fd, msg, strlen(msg)) == -1) {
        perror("[Drone] write");
    }

    close(fd);

    printf("[Drone %d] Finished mission.\n", drone_id);
    return 0;
}
