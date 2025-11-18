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
    while (1) {
        printf("[Drone %d] Taking off...\n", drone_id);
        sleep(10); // lot

        printf("[Drone %d] Returning to base...\n", drone_id);
        sleep(1); // powrót

        // Próba otwarcia FIFO (operator musi czekaæ)
        int fd;
        while ((fd = open(FIFO_PATH, O_WRONLY)) == -1) {
            printf("[Drone %d] Waiting for operator...\n", drone_id);
            sleep(1);
        }

        char msg[128];
        snprintf(msg, sizeof(msg), "Drone %d reporting: returned to base\n", drone_id);

        write(fd, msg, strlen(msg));
        close(fd);

        printf("[Drone %d] Report sent. Preparing next flight...\n", drone_id);
        sleep(1); // cooldown
    }

    return 0;
}
