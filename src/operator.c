#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <signal.h>
#include <errno.h>
#include "common.h"

// Flaga steruj¹ca g³ówn¹ pêtl¹ programu
static volatile sig_atomic_t keep_running = 1;

// Handler SIGINT (Ctrl+C)
void cleanup(int sig) {
    (void)sig;  // usuwa warning: unused parameter

    printf("\n[Operator] SIGINT received. Cleaning up...\n");

    // Usuniêcie FIFO z systemu plików
    unlink(FIFO_PATH);

    // Sygna³ do zakoñczenia g³ównej pêtli
    keep_running = 0;
}

int main() {
    // Rejestrujemy funkcjê cleanup dla SIGINT (Ctrl+C)
    signal(SIGINT, cleanup);

    // Tworzymy FIFO — je¿eli istnieje, ignorujemy b³¹d EEXIST
    if (mkfifo(FIFO_PATH, 0666) == -1) {
        if (errno != EEXIST) {
            perror("[Operator] mkfifo");
            exit(1);
        }
    }

    printf("[Operator] Ready. Waiting for drones...\n");

    // G³ówna pêtla operatora
    while (keep_running) {

        // Je¿eli przerwaliœmy program — przerywamy natychmiast
        if (!keep_running) break;

        // Otwieramy FIFO tylko do odczytu
        int fd = open(FIFO_PATH, O_RDONLY);
        if (fd == -1) {
            // Je¿eli FIFO zosta³o usuniête — Koñczymy bez b³êdów
            if (!keep_running) break;

            perror("[Operator] open");
            continue;
        }

        char buffer[256];
        int n;

        // Czytamy dane dopóki drony pisz¹
        while ((n = read(fd, buffer, sizeof(buffer) - 1)) > 0) {
            buffer[n] = '\0';
            printf("[Operator] Received: %s", buffer);
        }

        close(fd);

        // n==0 › wszystkie procesy zamknê³y deskryptory FIFO
        if (n == 0 && keep_running) {
            printf("[Operator] All drones disconnected. Reopening FIFO...\n");
        }
    }

    printf("[Operator] Shutdown complete.\n");
    return 0;
}
