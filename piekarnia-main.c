#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>       // fork(), getpid(), sleep()
#include <sys/types.h>    // pid_t
#include <sys/ipc.h>      // ftok()
#include <sys/sem.h>      // semget(), semop(), semctl()
#include <sys/shm.h>      // shmget(), shmat(), shmdt(), shmctl()
#include <signal.h>       // signal(), kill()
#include <string.h>       // memset()
#include <errno.h>        // errno
#include <time.h>         // time(), rand(), srand()
#include <sys/wait.h>   


#define P 12   // Liczba rodzajów produktów (P>10)
#define MAX_K 10   // Maksymalna liczba sztuk produktu w podajniku
#define MAX_IN_STORE    10    // Maksymalna liczba klientów w piekarni
#define MAX_CLIENTS     1000   // Maks. liczba klientów (dla tablic w pamięci)
#define N_CASHIERS      3     // Maks. 3 kasjerów

#define SIG_EWAKUACJA     SIGUSR1
#define SIG_INWENTARYZACJA SIGUSR2

#define SHM_KEY         1234
#define SEM_KEY         4321

// Godziny otwarcia 
#define T_p 8   
#define T_k 20


#define N_TOTAL_CLIENTS 25



typedef enum {CASHIER_INACTIVE=0, CASHIER_ACTIVE=1, CASHIER_CLOSING=2} cashier_state_t;

typedef struct {
    int    produkty[P];       
    double prices[P];

    int    podajnikQueue[P]; 

    int inStore;       // ile osób aktualnie w piekarni
    int storeQueue;    // ile osób czeka na wejście (jeśli w piekarni już max=10)

    // Kasjerzy
    cashier_state_t cashierState[N_CASHIERS];      // stan kasjera: INACTIVE, ACTIVE lub CLOSING ( zamykanie kasy i kończenie obsługi)
    int    kasjerQueue[N_CASHIERS];               // liczba klientów oczekujących do kasjera i
    double clientCosts[N_CASHIERS][MAX_CLIENTS];  // paragony klientów w kolejce kasjera i
    pid_t  clientPids[N_CASHIERS][MAX_CLIENTS];   // PID-y klientów
    int    nextClientId[N_CASHIERS];              // wskaźnik do wolnego miejsca w kolejce
    int    nextToServe[N_CASHIERS];               // który klient jest następny do obsługi

    double totalRevenue;
 
    int    totalClientsStarted;  
} shared_data_t;


int shmid;
shared_data_t *shdata;

int semid;
union semun {
    int val;
    struct semid_ds *buf;
    unsigned short *array;
};


void sem_op(int semid, int sem_num, int sem_op_value) {
    struct sembuf sb;
    sb.sem_num = sem_num;
    sb.sem_op = sem_op_value;
    sb.sem_flg = 0; 

    if (semop(semid, &sb, 1) == -1) {
        perror("semop");
        exit(EXIT_FAILURE);
    }
}

void lock_semaphore(int semid, int sem_num) {
    sem_op(semid, sem_num, -1);
}

void unlock_semaphore(int semid, int sem_num) {
    sem_op(semid, sem_num, 1);
}


int ewakuacja_flag = 0;
int inwentaryzacja_flag = 0;


void signal_handler(int sig) {
    if (sig == SIG_EWAKUACJA) {
        ewakuacja_flag = 1;
        printf("[PID %d] Otrzymano SIG_EWAKUACJA -> przerywam!\n", getpid());
    }
    else if (sig == SIG_INWENTARYZACJA) {
        inwentaryzacja_flag = 1;
        printf("[PID %d] Otrzymano SIG_INWENTARYZACJA!\n", getpid());
    }
    else {
        printf("[PID %d] Otrzymano nieznany sygnał: %d\n", getpid(), sig);
    }
}


void clear() {

    if (shmdt(shdata) == -1) {
        perror("shmdt");
    }
 
    if (shmctl(shmid, IPC_RMID, NULL) == -1) {
        perror("shmctl(IPC_RMID)");
    }
    
    if (semctl(semid, 0, IPC_RMID) == -1) {
        perror("semctl(IPC_RMID)");
    }
}


int IleKasjerow(int inStore) {

    int K = N_TOTAL_CLIENTS / 3; // np. 4 przy N=12
    int needed = (inStore + K - 1) / K;
    if (needed < 1 && inStore > 0) {
        needed = 1; // minimum 1, jeśli ktokolwiek jest w sklepie
    }
    if (needed > N_CASHIERS) {
        needed = N_CASHIERS; 
    }
    return needed;
}


void manager_proces() {
    int czasOtwarcia = (T_k - T_p) * 10;
    time_t startTime = time(NULL);

    printf("[MANAGER %d] Sklep otwarty w godz. %d-%d (symulacja %d sekund)\n",
           getpid(), T_p, T_k, czasOtwarcia);

  
    lock_semaphore(semid, 0);
    shdata->cashierState[0] = CASHIER_ACTIVE;
    shdata->cashierState[1] = CASHIER_ACTIVE; 
    shdata->cashierState[2] = CASHIER_INACTIVE;
    unlock_semaphore(semid, 0);

    while(!ewakuacja_flag) {
        sleep(3);

     
        time_t now = time(NULL);
        if (now - startTime >= czasOtwarcia) {
            printf("[MANAGER %d] Koniec godzin pracy (upłynęło %d sekund) -> SIG_EWAKUACJA\n",
                   getpid(), czasOtwarcia);
            kill(0, SIG_EWAKUACJA);
            break;
        }

        lock_semaphore(semid, 0);

        int inStore = shdata->inStore;
        int needed = IleKasjerow(inStore); 


        int activeCount = 0;
        for(int i=0; i<N_CASHIERS; i++){
            if (shdata->cashierState[i] == CASHIER_ACTIVE) {
                activeCount++;
            }
 
            if (shdata->cashierState[i] == CASHIER_CLOSING) {
                activeCount++;
            }
        }

    
        if (activeCount < needed) {
            for(int i=0; i<N_CASHIERS && activeCount < needed; i++){
                if (shdata->cashierState[i] == CASHIER_INACTIVE) {
                    shdata->cashierState[i] = CASHIER_ACTIVE;
                    activeCount++;
                    printf("[MANAGER %d] Aktywuję kasjera #%d (activeCount=%d)\n",
                           getpid(), i, activeCount);
                }
            }
        }

       
        int threshold = (2 * N_TOTAL_CLIENTS)/3; 
        if (inStore < threshold && activeCount > needed && activeCount > 1) {
        
            for(int i=N_CASHIERS-1; i>=0; i--) {
                if (shdata->cashierState[i] == CASHIER_ACTIVE) {
                    int queueSize = shdata->kasjerQueue[i];
                    if (queueSize == 0) {
                    
                        shdata->cashierState[i] = CASHIER_INACTIVE;
                        activeCount--;
                        printf("[MANAGER %d] Dezaktywuję kasjera #%d (kolejka=0)\n",
                               getpid(), i);
                        if (activeCount <= needed) break;
                    } else {
                   
                        shdata->cashierState[i] = CASHIER_CLOSING;
                        printf("[MANAGER %d] Kasjer #%d przechodzi w stan CLOSING (kolejka=%d)\n",
                               getpid(), i, queueSize);
                    
                        if (activeCount <= needed) break;
                    }
                }
            }
        }

        unlock_semaphore(semid, 0);
    } 

    printf("[MANAGER %d] Zakończono pętlę menedżera (ewakuacja_flag=%d)\n",
           getpid(), ewakuacja_flag);
    exit(EXIT_SUCCESS);
}


void piekarz_proces() {
    srand(time(NULL) ^ (getpid()<<16));
    while(!ewakuacja_flag) {
        int prod_index = rand() % P;
        int ile = (rand() % 5) + 1; //ilosc wyprodukowanych przez niego 

        lock_semaphore(semid, 0);
        if (shdata->produkty[prod_index] + ile <= MAX_K) {
            shdata->produkty[prod_index] += ile;
            printf("[PIEKARZ %d] Uzupełniam %d szt. produktu %d (stan=%d)\n",
                   getpid(), ile, prod_index, shdata->produkty[prod_index]);
        } else {
        
            // printf("[PIEKARZ %d] Podajnik %d pełny – czekam.\n",
            //        getpid(), prod_index);
        }
        unlock_semaphore(semid, 0);

        sleep(4);
    }
    printf("[PIEKARZ %d] EWAKUACJA – kończę.\n", getpid());
    exit(EXIT_SUCCESS);
}


void kasjer_proces(int kasjer_id) {
    while(!ewakuacja_flag) {
        lock_semaphore(semid, 0);

        cashier_state_t state = shdata->cashierState[kasjer_id];
        int queueCount = shdata->kasjerQueue[kasjer_id];

    
        if (state == CASHIER_INACTIVE) {
            unlock_semaphore(semid, 0);
            sleep(1);
            continue;
        }

    
        if (queueCount > 0 && shdata->nextToServe[kasjer_id] < shdata->nextClientId[kasjer_id])
        {
          
            shdata->kasjerQueue[kasjer_id]--;

            int clientIndex = shdata->nextToServe[kasjer_id];
            double cost     = shdata->clientCosts[kasjer_id][clientIndex];
            pid_t clientPid = shdata->clientPids[kasjer_id][clientIndex];

            shdata->nextToServe[kasjer_id]++;

            unlock_semaphore(semid, 0);

          
            printf("[KASJER %d:%d] Obsługuję klienta PID=%d (rachunek=%.2f)\n",
                   getpid(), kasjer_id, (int)clientPid, cost);
            sleep(2);

            // Podliczenie pieniądzy
            lock_semaphore(semid, 0);
            shdata->totalRevenue += cost;
            unlock_semaphore(semid, 0);

            printf("[KASJER %d:%d] Zakończono obsługę klienta PID=%d (%.2f)\n",
                   getpid(), kasjer_id, (int)clientPid, cost);
        }
        else {
          
            if (state == CASHIER_CLOSING && queueCount == 0) {
                shdata->cashierState[kasjer_id] = CASHIER_INACTIVE;
                printf("[KASJER %d:%d] Przechodzę w stan INACTIVE (kolejka pusta, byłem CLOSING)\n",
                       getpid(), kasjer_id);
            }
            unlock_semaphore(semid, 0);
            sleep(1);
        }
    }

    printf("[KASJER %d:%d] EWAKUACJA – kończę.\n", getpid(), kasjer_id);
    exit(EXIT_SUCCESS);
}


void klient_proces() {
    srand(time(NULL) ^ (getpid()<<16));

    // Losowanie 2-3 rodzaji produktów
    int ile_rodz = 2 + (rand() % 2);
    int kupowane[P];
    memset(kupowane, 0, sizeof(kupowane));
    for(int i=0; i<ile_rodz; i++){
        int prod_index = rand() % P;
        kupowane[prod_index] = (rand() % 3) + 1; 
    }

    int wszedlem = 0;
    while(!ewakuacja_flag && !wszedlem) {
        lock_semaphore(semid, 0);
        if (shdata->inStore < MAX_IN_STORE) {
            shdata->inStore++;
            wszedlem = 1;
            printf("[KLIENT %d] Wchodzę do piekarni (inStore=%d)\n", getpid(), shdata->inStore);
        } else {
            shdata->storeQueue++;
            int sq = shdata->storeQueue;
            unlock_semaphore(semid, 0);

            printf("[KLIENT %d] Piekarnia pełna – czekam (storequeue=%d)\n", getpid(), sq);
            sleep(2);

            lock_semaphore(semid, 0);
            shdata->storeQueue--;
            unlock_semaphore(semid, 0);
        }
        if (wszedlem) unlock_semaphore(semid, 0);
    }
    if (ewakuacja_flag) {
        printf("[KLIENT %d] EWAKUACJA – rezygnuję.\n", getpid());
        exit(EXIT_SUCCESS);
    }


    double mojRachunek = 0.0;
    for(int i=0; i<P; i++){
        if (kupowane[i] == 0) continue;

        lock_semaphore(semid, 0);
        shdata->podajnikQueue[i]++;
        int pq = shdata->podajnikQueue[i];
        unlock_semaphore(semid, 0);

        printf("[KLIENT %d] Ustawiam się w kolejce do podajnika %d (kolejka=%d)\n",
               getpid(), i, pq);
        sleep(1); 

        lock_semaphore(semid, 0);
        shdata->podajnikQueue[i]--;
        if (shdata->produkty[i] >= kupowane[i]) {
            shdata->produkty[i] -= kupowane[i];
            double cost = kupowane[i] * shdata->prices[i];
            mojRachunek += cost;
            printf("[KLIENT %d] Pobieram %d szt. prod.%d (%.2f/szt). Zostało=%d\n",
                   getpid(), kupowane[i], i, shdata->prices[i], shdata->produkty[i]);
        } else {
            printf("[KLIENT %d] Za mało prod.%d, pomijam.\n", getpid(), i);
        }
        unlock_semaphore(semid, 0);
    }

    // Wybieranie kasjera
    int possibleKasjerIDs[N_CASHIERS];
    int countPossible = 0;

    lock_semaphore(semid, 0);
    for(int i=0; i<N_CASHIERS; i++){
        cashier_state_t st = shdata->cashierState[i];
        if (st == CASHIER_ACTIVE || st == CASHIER_CLOSING) {
            possibleKasjerIDs[countPossible++] = i;
        }
    }
    if (countPossible == 0) {
        possibleKasjerIDs[0] = 0;
        countPossible = 1;
    }
    int chosen_cashier = possibleKasjerIDs[rand() % countPossible];

    int indeksKlient = shdata->nextClientId[chosen_cashier];
    shdata->nextClientId[chosen_cashier]++;

    shdata->clientCosts[chosen_cashier][indeksKlient] = mojRachunek;
    shdata->clientPids[chosen_cashier][indeksKlient]  = getpid();

    shdata->kasjerQueue[chosen_cashier]++;

    printf("[KLIENT %d] Ustawiam się w kolejce kasjera #%d (rachunek=%.2f, kolejka=%d)\n",
           getpid(), chosen_cashier, mojRachunek, shdata->kasjerQueue[chosen_cashier]);
    unlock_semaphore(semid, 0);


    sleep(3);

    lock_semaphore(semid, 0);
    shdata->inStore--;
    int inS = shdata->inStore;
    unlock_semaphore(semid, 0);
    printf("[KLIENT %d] Opuszczam piekarnię (inStore=%d)\n", getpid(), inS);
    exit(EXIT_SUCCESS);
}


int main(int argc, char *argv[]) {
    if (signal(SIG_EWAKUACJA, signal_handler) == SIG_ERR) {
        perror("signal(SIG_EWAKUACJA)");
        exit(EXIT_FAILURE);
    }
    if (signal(SIG_INWENTARYZACJA, signal_handler) == SIG_ERR) {
        perror("signal(SIG_INWENTARYZACJA)");
        exit(EXIT_FAILURE);
    }

  
    shmid = shmget(SHM_KEY, sizeof(shared_data_t), IPC_CREAT | 0600);
    if (shmid == -1) {
        perror("shmget");
        exit(EXIT_FAILURE);
    }
    shdata = (shared_data_t*) shmat(shmid, NULL, 0);
    if (shdata == (void*)-1) {
        perror("shmat");
        exit(EXIT_FAILURE);
    }


    memset(shdata, 0, sizeof(*shdata));

    // Losowe ceny produktów
    srand(time(NULL));
    for(int i=0; i<P; i++){
        double base = (rand() % 401) + 100; // 100..500
        shdata->prices[i] = base / 100.0;
    }
   
    shdata->totalRevenue = 0.0;

   
    semid = semget(SEM_KEY, 1, IPC_CREAT | 0600);
    if (semid == -1) {
        perror("semget");
        exit(EXIT_FAILURE);
    }
    union semun arg;
    arg.val = 1; // mutex
    if (semctl(semid, 0, SETVAL, arg) == -1) {
        perror("semctl(SETVAL)");
        clear();
        exit(EXIT_FAILURE);
    }


    pid_t pid = fork();
    if(pid == 0) {
        manager_proces();
    } else if(pid < 0) {
        perror("fork MANAGER");
        clear();
        exit(EXIT_FAILURE);
    }


    for(int i=0; i<2; i++){
        pid = fork();
        if(pid == 0) {
            piekarz_proces();
        } else if(pid < 0) {
            perror("fork PIEKARZ");
            clear();
            exit(EXIT_FAILURE);
        }
    }

    for(int i=0; i<N_CASHIERS; i++){
        pid = fork();
        if(pid == 0) {
            kasjer_proces(i);
        } else if(pid < 0) {
            perror("fork KASJER");
            clear();
            exit(EXIT_FAILURE);
        }
    }


    for(int i=0; i<N_TOTAL_CLIENTS; i++){
        pid = fork();
        if(pid == 0) {
            klient_proces();
        } else if(pid < 0) {
            perror("fork KLIENT");
            clear();
            exit(EXIT_FAILURE);
        }
    }

    // Proces rodzic czeka na wszystkie dzieci
    while(wait(NULL) > 0);


    printf("\n[MAIN %d] Wszystkie procesy zakończyły pracę.\n", getpid());
    printf("Stan pamięci dzielonej:\n");
    for(int i=0; i<P; i++){
        printf("  Podajnik[%d] = %d szt. (cena=%.2f)\n",
               i, shdata->produkty[i], shdata->prices[i]);
    }
    for(int i=0; i<N_CASHIERS; i++){
        printf("  Kasjer #%d => state=%d, kolejka=%d, nextClientId=%d, nextToServe=%d\n",
               i,
               (int)shdata->cashierState[i],
               shdata->kasjerQueue[i],
               shdata->nextClientId[i],
               shdata->nextToServe[i]);
    }
    printf("  totalRevenue = %.2f\n", shdata->totalRevenue);
    printf("  inStore=%d, storequeue=%d\n", shdata->inStore, shdata->storeQueue);

    clear();
    printf("[MAIN %d] Koniec programu.\n", getpid());
    return 0;
}
