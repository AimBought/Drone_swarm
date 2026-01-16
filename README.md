**FILIP TACIK SO 2025/26**

**RÓJ DRONÓW DOKUMENTACJA**

**1\. Założenia projektowe i opis ogólny:**

System został zaprojektowany jako zbiór współpracujących procesów w środowisku Linux, realizujących symulację roju dronów.

- **Architektura:** Hierarchiczna i **modułowa**. Kod został podzielony na warstwę logiczną (Commander, Operator, Dron) oraz warstwę abstrakcji systemowej (biblioteka wrapperów IPC). Proces główny (**Commander**) zarządza cyklem życia systemu i interakcją z użytkownikiem. Proces podrzędny (**Operator**) pełni rolę zarządcy zasobów (bazy). Procesy potomne (**Drony**) symulują niezależne jednostki autonomiczne.
- **Komunikacja (IPC):** Wykorzystano **Kolejki Komunikatów** do asynchronicznej wymiany żądań (lądowanie, start) oraz **Pamięć Dzieloną** do mapowania logicznych ID dronów na systemowe PID (niezbędne do celowania sygnałami).
- **Synchronizacja:** Dostęp do ograniczonej pojemności hangaru P regulowany jest przez **Semafor licznikowy**. Ruch w tunelach synchronizowany jest logicznie przez Operatora. Symulacja upływu czasu nie blokuje procesora i jest realizowana poprzez operacje na semaforach z timeoutem.
- **Założenia Implementacyjne i Graniczne:** Przyjęto następujące zasady stabilności:
    - **Model obsługi sygnałów:** Sygnały od Commandera (SIGUSR1/2) nie wymuszają natychmiastowej akcji w handlerze, lecz ustawiają **flagi stanu**. Faktyczna logika wykonywana jest w głównym wątku Operatora w bezpiecznym momencie pętli zdarzeń.
    - **Atomowość przelotu:** Dron znajdujący się w tunelu jest chroniony przed natychmiastowym usunięciem logicznym, aby nie doprowadzić do niespójności liczników tunelu. Śmierć w tunelu jest obsługiwana specjalnym komunikatem MSG_DEAD.
    - **Polityka tworzenia dronów:** Decyzję o tym, czy fizycznie można stworzyć nowy proces, podejmuje Operator w oparciu o limity systemowe oraz dostępność slotów w tablicy PID.

**2\. Ogólny opis kodu:**

Struktury Danych:

W celu optymalizacji wydajności zastosowano dedykowane struktury danych:

- **Bufor Cykliczny (Circular Buffer):** Wykorzystany w Operatorze do obsługi kolejek oczekujących. Gwarantuje stały czas dostępu przy dodawaniu i pobieraniu dronów, eliminując konieczność kosztownego przesuwania pamięci przy obsłudze FIFO.
- **Tablica Mapująca w Pamięci Dzielonej:** Pozwala na błyskawiczne tłmaczenie logicznego ID drona (0..N) na systemowy PID procesu, umożliwiając Commanderowi precyzyjne celowanie sygnałami.

Kod został podzielony na **cztery** elementy: bibliotekę współdzieloną oraz trzy moduły logiczne:

1.  **ipc_wrapper.c / ipc_wrapper.h:** Warstwa abstrakcji nad funkcjami systemowymi. Zawiera implementację bezpiecznych funkcji IPC (odpornych na przerwania sygnałami EINTR) oraz mechanizm custom_wait realizujący precyzyjne odmierzanie czasu przy użyciu semaforów.
2.  **commander.c**: Inicjuje struktury IPC, przeprowadza walidację danych wejściowych (zgodnie z warunkiem P < N/2 oraz limitami systemowymi ulimit), uruchamia procesy potomne i nasłuchuje komend użytkownika, przesyłając odpowiednie sygnały.
3.  **operator.c**: Zarządza kolejkami FIFO dla dronów oczekujących na start/lądowanie. Obsługuje dynamiczne skalowanie bazy (Sygnały 1 i 2) oraz dba o to, by w tunelu odbywał się ruch jednokierunkowy.
4.  **drone.c**: Implementuje maszynę stanów drona (Lot -> Kolejka -> Lądowanie -> Ładowanie ->Start). Symuluje zużycie baterii oraz cykl starzenia się urządzenia.

**3\. Co udało się zrobić:**

- Pełna, stabilna symulacja cyklu życia roju bez zakleszczeń (deadlocków).
- Implementacja **dwukierunkowej komunikacji** w wąskich tunelach.
- Obsługa **wszystkich trzech sygnałów** sterujących w czasie rzeczywistym.
- Realistyczny model zużycia energii, drony z niską baterią i gnorują rozkaz ataku, a drony rozładowane giną.
- Mechanizm **"Garbage Collection"**: Operator poprawnie usuwa z kolejek drony, które zginęły, zwalniając zasoby.
- **Pełna obsługa przerwań systemowych:** Zastosowano autorskie wrappery (safe_msgrcv, safe_semop), dzięki którym program nie kończy się błędem w momencie otrzymania sygnału sterującego podczas operacji wejścia/wyjścia, lecz poprawnie wznawia działanie.

**4\. Dodane elementy specjalne:**

1.  **Algorytm "Pending Removal" (Odroczony Demontaż):** Rozwiązanie problemu zmniejszania bazy (Sygnał 2), gdy wszystkie miejsca są zajęte. Operator nie usuwa miejsca "siłowo", lecz oznacza je do usunięcia. Miejsce fizycznie znika z semafora dopiero, gdy dron wyleci z bazy. Zapobiega to błędom synchronizacji.
2.  **Zaawansowana walidacja danych:** Program sprawdza nie tylko poprawność logiczną P < N/2, ale też systemowe limity procesów użytkownika (getrlimit), zapobiegając awarii.
3.  **Pamięć Dzielona do celowania:** Użycie shm pozwala Commanderowi na natychmiastowe odnalezienie PID dowolnego drona w celu wysłania Sygnału 3.
4.  **Kolorowanie logów i podwójne raportowanie:** Wyjście terminala jest kolorowane (ANSI) dla czytelności, a jednocześnie prowadzone są szczegółowe logi w plikach .txt dla każdego procesu osobno.

**5\. Napotkane problemy i wyzwania:**

- **Problem "Ducha w tunelu":**Jeśli dron zginął w trakcie przelotu, Operator nie otrzymywał potwierdzenia zwolnienia tunelu. Prowadziło to do trwałego zablokowania wejścia, gdyż system "myślał", że tunel jest wciąż zajęty.
    - **Rozwiązanie:** Wdrożono obsługę komunikatu MSG_DEAD jako sygnału sprzątającego. Operator po wykryciu śmierci drona resetuje liczniki tuneli i czyści kolejki, przywracając spójność systemu.
- **Synchronizacja przy Sygnale 2:** Próba zmniejszenia semafora poniżej liczby zajętych miejsc spowodowałaby zawieszenie procesu Operatora (funkcja semop czekałaby w nieskończoność na zwolnienie zasobów), blokując całą symulację.
    - **Rozwiązanie:** Zastosowano bufor pending_removal. Nadmiarowe miejsca nie są usuwane siłowo, lecz oznaczane do usunięcia. Fizyczna redukcja semafora następuje asynchronicznie dopiero w momencie, gdy kolejne drony wylatują z bazy.
- **Logika śmierci "wewnątrz" vs "na zewnątrz":** Problem stanowiło określenie, jak powinien zachować się dron, który otrzymał rozkaz ataku (Sygnał 3) będąc wewnątrz bazy. Natychmiastowe zakończenie procesu w tym stanie zablokowałoby miejsce w semaforze na stałe.
    - **Rozwiązanie:** Wprowadzenie flagi stanu (ST_INSIDE/ST_OUTSIDE). Dron w bazie po otrzymaniu rozkazu nie ginie od razu, lecz przerywa ładowanie, wykonuje procedurę wylotu i "detonuje się" dopiero po zwolnieniu zasobów bazy.
- **Problem Wyczerpywania Puli ID:** W początkowej fazie projektu każde odtworzenie drona inkrementowało globalny licznik ID w nieskończoność. Po przekroczeniu limitu MAX_DRONE_ID (1024), nowe drony fizycznie powstawały, ale nie mogły zostać zarejestrowane w Pamięci Dzielonej. Commander tracił nad nimi kontrolę, ponieważ system nie mógł powiązać ich ID z systemowym PID.
    - **Rozwiązanie:** Zaimplementowano mechanizm Recyklingu ID. Zamiast tworzyć nowe numery, Operator przeszukuje tablicę w poszukiwaniu indeksów zwolnionych przez martwe drony. Dzięki temu, niezależnie od czasu trwania symulacji, identyfikatory aktywnych dronów zawsze mieszczą się w obsługiwanym zakresie \[0-1023\], gwarantując pełną kontrolę z poziomu Commandera.
- **Wyścig przy starcie systemu (Race Condition):** Commander uruchamiał procesy dronów niemal natychmiast po uruchomieniu Operatora. Często zdarzało się, że drony próbowały połączyć się z kolejką komunikatów, zanim Operator zdążył ją utworzyć, co powodowało błędy krytyczne przy starcie.
    - **Rozwiązanie:** Wdrożono mechanizm **aktywnej synchronizacji startu**. Commander po uruchomieniu Operatora wchodzi w pętlę sprawdzającą istnienie kolejki komunikatów. Drony są uruchamiane dopiero, gdy Commander otrzyma potwierdzenie, że infrastruktura IPC Operatora jest gotowa.

**6\. Testy:**

Poniżej przedstawiono 4 testy weryfikujące poprawność działania symulacji w oparciu o zdefiniowane wymagania.

### Test 1: Weryfikacja podstawowego cyklu życia i limitów

**Cel:** Sprawdzenie poprawności uruchomienia systemu, walidacji parametrów wejściowych (P &lt; N/2) oraz podstawowego cyklu drona: Start -&gt; Lot -> Lądowanie -> Ładowanie.

**Komenda:** ./commander 5 12 (Pojemność bazy: 5, Liczba dronów: 12)

**Procedura:**

1.  Uruchom program.
2.  Obserwuj logi Commandera i Operatora.
3.  Zweryfikuj, czy Operator wpuścił do bazy maksymalnie 5 dronów jednocześnie.
4.  Poczekaj, aż pierwsze drony się naładują i wylecą.
5.  Zakończ program (Ctrl+C).

**Obserwacje i Wnioski:** System uruchomił się poprawnie. W logach widać komunikaty GRANT LAND dla pierwszych 5 dronów, kolejne otrzymały status oczekujący (trafiły do kolejki). Po czasie ładowania drony opuściły bazę (MSG_DEPARTED), a na ich miejsce wpuszczono kolejne. Mechanizm semaforów poprawnie pilnuje limitu P=5. Drony poprawnie przechodzą przez maszynę stanów. Warunek startowy został spełniony.

**Screeny:**
![no_img](test_images/Pasted%20image%2020260116105913.png)
\- Drony lądują
![no_img](test_images/Pasted%20image%2020260116105942.png)
\- Drony odlatują, a na ich miejsce wchodzą nastepne

### Test 2: Weryfikacja Obsługi Dwóch Tuneli

**Cel:** Sprawdzenie poprawności zarządzania dwoma niezależnymi kanałami komunikacyjnymi (tunelami) przez Operatora. Weryfikacja, czy system potrafi obsłużyć sytuację "mijanki", gdzie jeden tunel służy do startów (OUT), a drugi w tym samym czasie do lądowań (IN), maksymalizując przepustowość bazy bez powodowania kolizji.

**Komenda:** ./commander 8 20 (Duża baza: 8 miejsc, Liczny rój: 20 dronów)

**Procedura:**

1.  Uruchom program. Baza początkowo jest pusta.
2.  Obserwuj pierwszą fazę: Drony masowo proszą o lądowanie (REQ_LAND).
3.  Poczekaj na moment krytyczny: Pierwsza grupa kończy ładowanie i prosi o start (REQ_TAKEOFF). W tym samym czasie drony wiszące na zewnątrz mają krytyczną baterię i błagają o lądowanie.
4.  Sprawdź w logach, czy Operator przydziela **różne kanały** dla sprzecznych kierunków (np. "GRANT TAKEOFF via Ch 0" oraz "GRANT LAND via Ch 1" w tym samym czasie).

**Obserwacje i Wnioski:** W fazie dużego natężenia ruchu zaobserwowano pełne wykorzystanie infrastruktury. Operator poprawnie wykrył konflikt kierunków i odseparował ruch: Kanał 0 został przydzielony dla dronów wylatujących (OUT), podczas gdy Kanał 1 obsługiwał drony lądujące (IN). Nie zaobserwowano sytuacji, w której jeden tunel miałby przypisane sprzeczne kierunki w tym samym momencie. System dynamicznie zmieniał przeznaczenie tuneli w zależności od potrzeb, co potwierdza poprawność implementacji logiki tuneli jednokierunkowych przy zachowaniu ciągłości operacyjnej roju.

**Screeny:**
![no_img](test_images/Pasted%20image%2020260116105956.png)
\- Drony lądują wlatując przez channel 0
![no_img](test_images/Pasted%20image%2020260116110009.png)
\- Drony wylatują z bazy przez channel 0, a nastepne w tym momencie wlatują przez channel 1

### Test 3: Dynamiczne skalowanie i "Pending Removal" (Sygnały 1 i 2)

**Cel:** Sprawdzenie reakcji systemu na sygnały zmiany pojemności bazy. Szczególny nacisk na algorytm **Pending Removal** (bezpieczne zmniejszanie bazy) weryfikacja, czy Operator nie usuwa dronów "siłowo".

**Komenda:** ./commander 10 30

**Procedura:**

1.  Poczekaj, aż baza się zapełni.
2.  Wciśnij 2 (Sygnał zmniejszenia bazy o połowę -> do 5).
3.  Obserwuj logi Operatora pod kątem komunikatu o "Pending Removal".
4.  Poczekaj chwilę i wciśnij 1 (Sygnał powiększenia bazy).

**Obserwacje i Wnioski:** Po wciśnięciu 2, mimo że w bazie było 10 dronów, żaden nie został wyrzucony ani zabity. Logi pokazały Pending: 5. Wartość semafora malała stopniowo dopiero wtedy, gdy drony same wylatywały. Po wciśnięciu 1 baza natychmiast zwiększyła limit, pozwalając na nowe lądowania. Mechanizm asynchronicznej zmiany zasobów działa bezbłędnie. Rozwiązano problem zakleszczeń przy zmniejszaniu semafora – system zachowuje spójność logiczną.

**Screeny:**
![no_img](test_images/Pasted%20image%2020260116110017.png)
\- Baza się zapełnia
![no_img](test_images/Pasted%20image%2020260116110023.png)
\- Podaje sygnał 2, widizmy pending 5 bo wszystkie miejsca zajęte
![no_img](test_images/Pasted%20image%2020260116110026.png)
\- Po wylocie dronów usuwamy od razu ich miejsca
![no_img](test_images/Pasted%20image%2020260116110031.png)
\- Wysłanie sygnału 1

### Test 4: Precyzyjne celowanie i Pamięć Dzielona (Sygnał 3 - Kamikaze)

**Cel:** Weryfikacja poprawności mapowania logicznego ID drona na PID procesu przy użyciu **Pamięci Dzielonej**. Sprawdzenie różnicy w zachowaniu drona wewnątrz bazy (opóźniony wybuch) i na zewnątrz (natychmiastowy wybuch).

**Komenda:** ./commander 5 12

**Procedura:**

1.  Wybierz ID drona, który jest **w powietrzu** (np. ID 3).
2.  Wciśnij 3, podaj ID 3 i zatwierdź. Obserwuj natychmiastową śmierć.
3.  Wybierz ID drona, który jest **w trakcie ładowania** w bazie.
4.  Wciśnij 3, podaj ID 7. Obserwuj, czy dron dokończy wylot przed wybuchem.

**Obserwacje i Wnioski:** Dron w powietrzu zginął natychmiast (RIP). Dron w bazie odebrał rozkaz, ale wyświetlił komunikat Will die after exit. Dokończył procedurę wylotu, zwolnił semafor i dopiero wtedy proces się zakończył. Struktura SharedState poprawnie mapuje ID na PID, umożliwiając Commanderowi precyzyjny atak. Logika ST_INSIDE/ST_OUTSIDE zapobiegła trwałemu zablokowaniu miejsca w hangarze (deadlock), co potwierdza odporność systemu na błędy krytyczne.

**Screeny:**
![np_img](test_images/Pasted%20image%2020260116110040.png)
\- Nowo utworzony dron dostaje sygnał 3
![no_img](test_images/Pasted%20image%2020260116110045.png)
\- Dron 0 dostaje sygnał 3 ale jest w bazie dlatego najpierw wylatuje i ginie na zewnątrz

### 7\. Linki do istotnych fragmentów kodu:Odnośniki do kodu (Linki do GitHub)

**a) Tworzenie plików i Wejście/Wyjście (Logowanie i Nieblokujące Wejście)**

Wymagane funkcje: fopen(), fprintf(), select(), read(), vprintf()

- src/commander.c: [cmd_log()](https://github.com/AimBought/Drone_swarm/blob/d567fc2ab8f50666b7abf57dd884b98358dc75e4/src/commander.c#L48-L67), src/operator.c: [olog()](https://github.com/AimBought/Drone_swarm/blob/d567fc2ab8f50666b7abf57dd884b98358dc75e4/src/operator.c#L71-L91), src/drone.c: [dlog()](https://github.com/AimBought/Drone_swarm/blob/d567fc2ab8f50666b7abf57dd884b98358dc75e4/src/drone.c#L61-L81)

Każdy proces posiada własny wrapper do logowania, który pisze jednocześnie na standardowe wyjście stdout (z kolorami ANSI) oraz do dedykowanego pliku .txt przy użyciu vfprintf.

- src/commander.c: [main()](https://github.com/AimBought/Drone_swarm/blob/d567fc2ab8f50666b7abf57dd884b98358dc75e4/src/commander.c#L241-L288)

Implementacja nieblokującego odczytu z klawiatury przy użyciu select(). Pozwala to Commanderowi nasłuchiwać komend użytkownika ('1', '2', '3') bez zamrażania pętli symulacyjnej.

- src/commander.c: [IPC Check](https://github.com/AimBought/Drone_swarm/blob/d567fc2ab8f50666b7abf57dd884b98358dc75e4/src/commander.c#L197-L201)

Alternatywne wykorzystanie funkcji select() jako precyzyjnego, nieblokującego timera, używanego w fazie startowej do oczekiwania na inicjalizację Operatora.

**b) Tworzenie i zarządzanie procesami**

Wymagane funkcje: fork(), exec(), exit(), waitpid(), kill()

- src/commander.c: [main()](https://github.com/AimBought/Drone_swarm/blob/d567fc2ab8f50666b7abf57dd884b98358dc75e4/src/commander.c#L170-L181)

Utworzenie procesu Operatora. Argumenty P oraz N są przekazywane jako ciągi znaków przez funkcję execl.

- src/commander.c: [main()](https://github.com/AimBought/Drone_swarm/blob/d567fc2ab8f50666b7abf57dd884b98358dc75e4/src/commander.c#L222-L236)

Utworzenie procesów początkowych dronów.

- src/operator.c: [spawn_new_drone()](https://github.com/AimBought/Drone_swarm/blob/d567fc2ab8f50666b7abf57dd884b98358dc75e4/src/operator.c#L310-L363)

Logika dynamicznego tworzenia procesów ("Replenish"). Zawiera mechanizm recyklingu slotów PID w Pamięci Dzielonej przed wywołaniem fork().

- src/drone.c: [drone_die()](https://github.com/AimBought/Drone_swarm/blob/d567fc2ab8f50666b7abf57dd884b98358dc75e4/src/drone.c#L102-L111)

Kontrolowane zakończenie procesu. Obejmuje wysłanie "wiadomości pożegnalnej" (MSG_DEAD) przed wywołaniem exit(0).

- src/operator.c: [sigchld_handler()](https://github.com/AimBought/Drone_swarm/blob/d567fc2ab8f50666b7abf57dd884b98358dc75e4/src/operator.c#L98-L105)

Zapobieganie powstawaniu "Procesów Zombie" przy użyciu waitpid z flagą WNOHANG.

**c) Obsługa sygnałów i bezpieczeństwo asynchroniczne**

Wymagane funkcje: signal(), kill()

- src/ipc_wrapper.c: [safe_semop(), safe_msgrcv()](https://github.com/AimBought/Drone_swarm/blob/d567fc2ab8f50666b7abf57dd884b98358dc75e4/src/ipc_wrapper.c#L20-L34)

Kluczowa część projektu. Implementacja wrapperów obsługujących błąd EINTR (Interrupted System Call). Gwarantuje to, że sygnały wysłane przez Commandera nie spowodują awarii operacji wejścia/wyjścia Operatora lub Drona.

- src/operator.c: [sigusr1_handler(), sigusr2_handler()](https://github.com/AimBought/Drone_swarm/blob/d567fc2ab8f50666b7abf57dd884b98358dc75e4/src/operator.c#L93-L96)

Implementacja podejścia opartego na flagach. Handlery ustawiają jedynie zmienne typu volatile sig_atomic_t. Właściwa logika (zmiana rozmiaru bazy) wykonywana jest bezpiecznie wewnątrz głównej pętli programu.

- src/drone.c: [sigusr1_handler() (Kamikaze)](https://github.com/AimBought/Drone_swarm/blob/d567fc2ab8f50666b7abf57dd884b98358dc75e4/src/drone.c#L118-L143)

Złożona logika obsługi rozkazu samobójstwa. Rozróżnia stan bycia "na zewnątrz" (natychmiastowa śmierć) oraz "wewnątrz bazy" (śmierć opóźniona, aby uniknąć zakleszczenia zasobów/deadlocka).

**d) Synchronizacja procesów i Czas (Semafory)**

Wymagane funkcje: semget(), semop(), semtimedop() (poprzez wrapper)

- src/ipc_wrapper.c: [custom_wait()](https://github.com/AimBought/Drone_swarm/blob/d567fc2ab8f50666b7abf57dd884b98358dc75e4/src/ipc_wrapper.c#L36-L45)

Implementacja symulacji czasu bez użycia funkcji usleep. Wykorzystuje semtimedop na trwale zablokowanym semaforze (SEM_TIMER) do wstrzymania wykonywania na precyzyjnie określony czas.

- src/operator.c: [reserve_hangar_spot()](https://github.com/AimBought/Drone_swarm/blob/d567fc2ab8f50666b7abf57dd884b98358dc75e4/src/operator.c#L146-L158), src/operator.c: [free_hangar_spot()](https://github.com/AimBought/Drone_swarm/blob/d567fc2ab8f50666b7abf57dd884b98358dc75e4/src/operator.c#L160-L175)

Synchronizacja dostępu do Hangaru przy użyciu semafora licznikowego (SEM_HANGAR). Zawiera logikę "Pending Removal" (odroczone zmniejszanie semafora).

- src/drone.c: [main() (Pętla Lotu)](https://github.com/AimBought/Drone_swarm/blob/d567fc2ab8f50666b7abf57dd884b98358dc75e4/src/drone.c#L220-L357)

Wykorzystanie custom_wait() do symulacji rozładowywania baterii w czasie.

**e) Komunikacja Międzyprocesowa (Kolejki Komunikatów)**

Wymagane funkcje: msgget(), msgsnd(), msgrcv()

Uwaga: Kolejki komunikatów zostały użyte zamiast potoków (Pipe/FIFO), aby umożliwić asynchroniczną komunikację wiele-do-jednego.

- src/commander.c: [main() (Synchronizacja Startu)](https://github.com/AimBought/Drone_swarm/blob/d567fc2ab8f50666b7abf57dd884b98358dc75e4/src/commander.c#L188-L218)

Użycie msgget z flagami ustawionymi na 0, aby sprawdzić, czy Operator poprawnie utworzył infrastrukturę IPC.

- src/operator.c: [main() (Pętla Komunikatów)](https://github.com/AimBought/Drone_swarm/blob/d567fc2ab8f50666b7abf57dd884b98358dc75e4/src/operator.c#L467-L536)

"Mózg" systemu. Maszyna stanów przetwarzająca żądania (REQ_LAND, REQ_TAKEOFF, MSG_DEAD) odbierane przez safe_msgrcv.

- src/drone.c: [send_msg()](https://github.com/AimBought/Drone_swarm/blob/d567fc2ab8f50666b7abf57dd884b98358dc75e4/src/drone.c#L83-L100)

Funkcja pomocnicza do wysyłania ustandaryzowanych struktur żądań do Operatora.

**f) Pamięć Dzielona (Mapowanie PID)**

Wymagane funkcje: shmget(), shmat(), shmdt(), shmctl()

- include/common.h: [struct SharedState](https://github.com/AimBought/Drone_swarm/blob/d567fc2ab8f50666b7abf57dd884b98358dc75e4/include/common.h#L37-L39)

Definicja struktury danych przechowywanej w Pamięci Dzielonej (tablica PID-ów).

- src/commander.c: [main() (Tworzenie)](https://github.com/AimBought/Drone_swarm/blob/d567fc2ab8f50666b7abf57dd884b98358dc75e4/src/commander.c#L156-L158)

Utworzenie segmentu pamięci i inicjalizacja zerami (memset).

- src/commander.c: [main() (Celowanie)](https://github.com/AimBought/Drone_swarm/blob/d567fc2ab8f50666b7abf57dd884b98358dc75e4/src/commander.c#L268-L286)

Odczyt z Pamięci Dzielonej w celu przetłumaczenia logicznego ID Drona (podanego przez użytkownika) na systemowy PID dla funkcji kill().

- src/operator.c: [spawn_new_drone()](https://github.com/AimBought/Drone_swarm/blob/d567fc2ab8f50666b7abf57dd884b98358dc75e4/src/operator.c#L318-L328)

Zapis PID nowego procesu potomnego do współdzielonej tablicy natychmiast po wywołaniu fork().
