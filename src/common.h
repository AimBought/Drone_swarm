#ifndef COMMON_H
#define COMMON_H

#include <sys/types.h>

#define MSGQ_KEY 0x1234   // klucz kolejki komunikatów
#define SEM_KEY  0x5678   // klucz semaforów

// Typy wiadomoœci od dronów do operatora
#define MSG_REQUEST 1     // dron prosi o wejœcie (mtype)
#define MSG_LEAVING 2     // dron informuje, ¿e opuszcza bazê (mtype)

// Kiedy operator wysy³a 'grant' do konkretnego drona, u¿ywa typu:
// RESPONSE_BASE + drone_id
#define RESPONSE_BASE 1000

// rozmiary bufów tekstowych
#define RESP_TEXT_SIZE 32

#endif
