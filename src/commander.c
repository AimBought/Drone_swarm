#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

// Commander uruchamia operatora i drony
int main() {
    printf("[Commander] Launching operator...\n");

    // 1. Forkujemy operatora
    pid_t op = fork();
    if (op == 0) {
        execl("./operator", "operator", NULL);
        perror("execl operator");
        exit(1);
    }

    sleep(1); // czekamy a¿ operator otworzy FIFO i zacznie dzia³aæ

    // 2. Uruchamiamy kilka dronów
    for (int i = 0; i < 3; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            char id[4];
            sprintf(id, "%d", i);
            execl("./drone", "drone", id, NULL);
            perror("execl drone");
            exit(1);
        }
    }

    // 3. Czekamy na wszystkie procesy potomne
    while (wait(NULL) > 0);

    printf("[Commander] All drones completed.\n");
    return 0;
}
