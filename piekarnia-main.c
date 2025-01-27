#include <stdio.h>

#include <stdlib.h>

#include <unistd.h>

#include <sys/types.h>

#include <sys/ipc.h>

#include <sys/sem.h>

#include <sys/shm.h>

#include <sys/msg.h>

#include <signal.h>

#include <string.h>

#include <errno.h>

#include <time.h>

#include <sys/wait.h>

#define RESET "\033[0m"
#define BLACK "\033[30m"
#define RED "\033[31m"
#define GREEN "\033[32m"
#define YELLOW "\033[33m"
#define BLUE "\033[34m"
#define MAGENTA "\033[35m"
#define CYAN "\033[36m"
#define WHITE "\033[37m"

// ------------------- USTAWIENIA ------------------------------------
#define P 12
// #define MAX_K 10
// #define MAX_IN_STORE 10

// Godziny otwarcia 
// #define T_p 8
// #define T_k 10

// #define N_TOTAL_CLIENTS 10000

#define SIG_EWAKUACJA SIGUSR1
#define SIG_INWENTARYZACJA SIGUSR2

#define SHM_KEY 123455
#define SEM_KEY 432155
#define MSG_KEY 567855

#define N_CASHIERS 3

typedef enum {
   CASHIER_INACTIVE = 0,
      CASHIER_ACTIVE = 1,
      CASHIER_CLOSING = 2
}
cashier_state_t;

typedef struct {
   int produkty[P];
   double prices[P];

   int podajnikQueue[P];
   int inStore;

   cashier_state_t cashierState[N_CASHIERS];
   double totalRevenue;
}
shared_data_t;

#define SEM_MUTEX 0 // do ochrony pamięci wspólnej
#define SEM_STORE 1 // kontrola liczby osób w sklepie
#define SEM_FILE 2 // do dostępu do pliku

union semun {
   int val;
   struct semid_ds * buf;
   unsigned short * array;
};

// Wiadomość żądania: klient -> kasjer
typedef struct msg_request {
   long mtype;
   pid_t pid;
   double cost;
   int cashier_id;
   int products[P];
}
msg_request_t;

// Wiadomość odpowiedzi: kasjer -> klient
typedef struct msg_response {
   long mtype;
   double final_cost;
}
msg_response_t;

int shmid;
shared_data_t * shdata;
int semid;
int ewakuacja_flag = 0;
int inwentaryzacja_flag = 0;

void sem_op(int semid, int sem_num, int sem_op_value) {
   struct sembuf sb;
   sb.sem_num = sem_num;
   sb.sem_op = sem_op_value;
   sb.sem_flg = 0;

   if (semop(semid, & sb, 1) == -1) {
      if (errno == EINTR) return;
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

void signal_handler(int sig) {
   if (sig == SIG_EWAKUACJA) {
      ewakuacja_flag = 1;
      // printf("[PID %d] Otrzymano SIG_EWAKUACJA -> przerywam!\n", getpid());
   } else if (sig == SIG_INWENTARYZACJA) {
      inwentaryzacja_flag = 1;
      // printf("[PID %d] Otrzymano SIG_INWENTARYZACJA!\n", getpid());
   } else {
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

   int msgid = msgget(MSG_KEY, 0);
   if (msgid != -1) {
      if (msgctl(msgid, IPC_RMID, NULL) == -1) {
         perror("msgctl(IPC_RMID)");
      }
   }
}
int get_valid_input(const char *prompt, int min, int max) {
    int value;
    while (1) {
        printf("%s (%d - %d): ", prompt, min, max);
        if (scanf("%d", &value) == 1 && value >= min && value <= max) {
            return value; 
        } else {
            printf("Nieprawidłowa wartość! Spróbuj ponownie.\n");
            while (getchar() != '\n'); 
        }
    }
}

// ------------------- MANAGER ------------------------
void manager_proces(int T_k, int T_p, int MAX_IN_STORE) {

   int czasOtwarcia = (T_k - T_p) * 10;
   time_t startTime = time(NULL);

   printf("[MANAGER %d] Sklep otwarty w godz. %d-%d (symulacja %d sekund)\n",
      getpid(), T_p, T_k, czasOtwarcia);

   lock_semaphore(semid, SEM_MUTEX);
   shdata -> cashierState[0] = CASHIER_ACTIVE;
   shdata -> cashierState[1] = CASHIER_ACTIVE;
   shdata -> cashierState[2] = CASHIER_INACTIVE;
   unlock_semaphore(semid, SEM_MUTEX);


   srand(time(NULL));
   int los = (rand() % 2);
    printf(YELLOW"[MANAGER %d] Los %d\n"RESET,
      getpid(), los);

   while (!ewakuacja_flag && !inwentaryzacja_flag) {
      sleep(3);

      time_t now = time(NULL);

   if(los == 0){
      if (now - startTime >= czasOtwarcia) {
         printf(YELLOW "[MANAGER %d] Koniec godzin pracy -> SIG_INWENTARYZACJA\n"
            RESET, getpid());
         kill(0, SIG_INWENTARYZACJA);
         break;
         }
      }else{
         if (now - startTime >= 30) {
          printf(RED"[MANAGER %d] EWAKUACJA -> SIG_EWAKUACJA\n"RESET, getpid());
          kill(0, SIG_EWAKUACJA);
          break;
         }
      }
       

      lock_semaphore(semid, SEM_MUTEX);

      int inStore = shdata -> inStore;
      int K = MAX_IN_STORE / 3;
      int needed = (inStore + K - 1) / K;
      if (needed < 1) {
         needed = 1;
      }
      if (needed > N_CASHIERS) {
         needed = N_CASHIERS;
      }

      int activeCount = 0;
      for (int i = 0; i < N_CASHIERS; i++) {
         if (shdata -> cashierState[i] == CASHIER_ACTIVE) {
            activeCount++;
         }
      }

      // Aktywacja, jeśli za mało
      if (activeCount < needed) {
         for (int i = 0; i < N_CASHIERS && activeCount < needed; i++) {
            if (shdata -> cashierState[i] == CASHIER_INACTIVE) {
               shdata -> cashierState[i] = CASHIER_ACTIVE;
               activeCount++;
               printf(MAGENTA "[MANAGER %d] Aktywuję kasjera #%d (activeCount=%d)\n"
                  MAGENTA,
                  getpid(), i, activeCount);
            }
         }
      }

      // Dezaktywacja, jeśli za dużo
      int threshold = (2 * MAX_IN_STORE / 3);
      if (inStore < threshold && activeCount > needed && activeCount > 1) {
         for (int i = N_CASHIERS - 1; i >= 0; i--) {
            if (shdata -> cashierState[i] == CASHIER_ACTIVE) {
               shdata -> cashierState[i] = CASHIER_CLOSING;
               activeCount--;
               printf(MAGENTA "[MANAGER %d] Kasjer #%d przechodzi w stan CLOSING.\n"
                  MAGENTA,
                  getpid(), i);
               printf("active = %d needed = %d \n", activeCount, needed);
               if (activeCount <= needed) break;
            }
         }
      }
      unlock_semaphore(semid, SEM_MUTEX);
   }

   

   if (inwentaryzacja_flag) {
      lock_semaphore(semid, SEM_MUTEX);
      int totalProducts = 0;
      for (int i = 0; i < P; i++) {
         totalProducts += shdata -> produkty[i];
      }
      unlock_semaphore(semid, SEM_MUTEX);


      lock_semaphore(semid, SEM_FILE);
      FILE * f = fopen("raport.txt", "a");
      if (f) {
         fprintf(f, "[MANAGER %d] Koniec pracy. Pozostało %d szt. towaru.\n",
            getpid(), totalProducts);
         fclose(f);
      }
      unlock_semaphore(semid, SEM_FILE);

      printf(MAGENTA "[MANAGER %d] Zakończono pętlę KONIEC DNIA (inwentaryzacja_flag=%d)\n"
         RESET, getpid(), inwentaryzacja_flag);
   }
   if (ewakuacja_flag) {
      printf(MAGENTA "[MANAGER %d] Zakończono pętlę KONIEC DNIA (ewakuacja_flag=%d)\n"
         RESET, getpid(), ewakuacja_flag);
   }

   exit(EXIT_SUCCESS);
}

// ------------------- PIEKARZ ------------------------
void piekarz_proces(int MAX_K) {
   srand(time(NULL) ^ (getpid() << 16));

   int producedLocal[P];
   memset(producedLocal, 0, sizeof(producedLocal));

   while (!ewakuacja_flag && !inwentaryzacja_flag) {
      int prod_index = rand() % P;
      int ile = (rand() % 5) + 1;

      lock_semaphore(semid, SEM_MUTEX);
      if (shdata -> produkty[prod_index] + ile <= MAX_K) {
         shdata -> produkty[prod_index] += ile;
         producedLocal[prod_index] += ile;
         printf(CYAN "[PIEKARZ %d] +%d szt. produktu %d (stan=%d)\n"
            RESET,
            getpid(), ile, prod_index, shdata -> produkty[prod_index]);
      }
      unlock_semaphore(semid, SEM_MUTEX);
      sleep(4);
   }

   if (inwentaryzacja_flag) {
      lock_semaphore(semid, SEM_FILE);
      FILE * f = fopen("raport.txt", "a");
      if (f) {
         fprintf(f, "[PIEKARZ %d] Wyprodukowałem:\n", getpid());
         for (int i = 0; i < P; i++) {
            if (producedLocal[i] > 0) {
               fprintf(f, "  - Produkt %d: %d szt.\n", i, producedLocal[i]);
            }
         }
         fclose(f);
      }
      unlock_semaphore(semid, SEM_FILE);

      printf(CYAN "[PIEKARZ %d] INWENTARYZACJA – kończę.\n"
         RESET, getpid());
   }

   if (ewakuacja_flag) {
      printf(CYAN "[PIEKARZ %d] EWAKUACJA – kończę.\n"
         RESET, getpid());
   }
   exit(EXIT_SUCCESS);
}

// ------------------- KASJER ------------------------
void kasjer_proces(int kasjer_id) {

   int msgid = msgget(MSG_KEY, IPC_CREAT | 0600);
   if (msgid == -1) {
      perror("msgget kasjer");
      exit(EXIT_FAILURE);
   }

   double kasjerEarnings = 0.0;
   int servedCount = 0;
   int sellLocal[P];
   memset(sellLocal, 0, sizeof(sellLocal));

   int should_exit = 0;

   while (!ewakuacja_flag && !should_exit) {

      cashier_state_t st;
      int inStoreNow;

      lock_semaphore(semid, SEM_MUTEX);
      st = shdata -> cashierState[kasjer_id];
      inStoreNow = shdata -> inStore;
      unlock_semaphore(semid, SEM_MUTEX);

      if (st == CASHIER_INACTIVE) {
         if ((inwentaryzacja_flag && inStoreNow == 0) || ewakuacja_flag ){
            should_exit = 1;
            break;
         }
         sleep(1);
         continue;
      }

      msg_request_t req;
      ssize_t rcv;

      while (1) {

         rcv = msgrcv(msgid, & req, sizeof(req) - sizeof(long),
            kasjer_id + 1, IPC_NOWAIT);
         if (rcv == -1) {
      lock_semaphore(semid, SEM_MUTEX);
      st = shdata -> cashierState[kasjer_id];
      inStoreNow = shdata -> inStore;
      unlock_semaphore(semid, SEM_MUTEX);
            if (errno == ENOMSG) {
               if ((inwentaryzacja_flag && inStoreNow == 0) || ewakuacja_flag) {
                  should_exit = 1;
                  break;
               } else {
                  continue;
               }

            } else if (errno == EINTR) {
               if (ewakuacja_flag) {
                  should_exit = 1;
                  break;
               }
               if (inwentaryzacja_flag) {
                  continue;
               }
               continue;
            }
            perror("msgrcv kasjer");
            should_exit = 1;
            break;
         }
         break;
      }

      if (should_exit) {
         break;
      }

      pid_t cpid = req.pid;
      double cost = req.cost;

      printf(GREEN "[KASJER %d:%d] Obsługuję klienta PID=%d (rachunek=%.2f)\n"
         RESET,
         getpid(), kasjer_id, (int) cpid, cost);

      sleep(2); // obsługa klienta 

      kasjerEarnings += cost;
      servedCount++;

      lock_semaphore(semid, SEM_MUTEX);
      shdata -> totalRevenue += cost;
      unlock_semaphore(semid, SEM_MUTEX);

      printf(GREEN "[KASJER %d:%d] Zakończono obsługę klienta PID=%d (%.2f)\n"
         RESET,
         getpid(), kasjer_id, (int) cpid, cost);

      msg_response_t resp;
      resp.mtype = cpid;
      resp.final_cost = cost;

      while (1) {
         if (msgsnd(msgid, & resp, sizeof(resp) - sizeof(long), 0) == -1) {
            if (errno == EINTR) {
               if (ewakuacja_flag) {
                  printf(GREEN "[KASJER %d:%d] Otrzymałem sygnał EWAKUACJA podczas wysyłania. Rezygnuję.\n"
                     RESET, getpid(), kasjer_id);
                  should_exit = 1;
                  break;
               }
               continue;
            } else {
               perror("msgsnd kasjer->klient");
               should_exit = 1;
               break;
            }
         }
         break;
      }

      if (should_exit) {
         break;
      }

      for (int i = 0; i < P; i++) {
         if (req.products[i] > 0) {
            sellLocal[i] += req.products[i];
         }
      }
      // lock_semaphore(semid, SEM_FILE);
      // FILE *f = fopen("raport.txt", "a");
      // if (f) {
      //     // fprintf(f, "[KASJER %d:%d] Obsłużyłem klienta PID=%d, kwota=%.2f\n",
      //     //         getpid(), kasjer_id, (int)cpid, cost);

      //     // fprintf(f, "  - Produkty zakupione przez klienta PID=%d:\n", (int)cpid);
      //     // for(int i=0; i<P; i++) {
      //     //     if(req.products[i] > 0) {
      //     //         fprintf(f, "    * Produkt %d: %d szt.\n", i, req.products[i]);
      //     //     }
      //     // }
      //     fclose(f);
      // }
      // unlock_semaphore(semid, SEM_FILE);
   }

if(inwentaryzacja_flag){
   lock_semaphore(semid, SEM_FILE);
   FILE * f = fopen("raport.txt", "a");
   if (f) {
      fprintf(f, "[KASJER %d:%d] KONIEC PRACY. Obsłużyłem %d klientów, łącznie zarobiłem %.2f\n",
         getpid(), kasjer_id, servedCount, kasjerEarnings);
      fprintf(f, "  - Suma sprzedanych produktów:\n");
      for (int i = 0; i < P; i++) {
         if (sellLocal[i] > 0) {
            fprintf(f, "    * Produkt %d: %d szt.\n", i, sellLocal[i]);
         }
      }
      fclose(f);
   }
   unlock_semaphore(semid, SEM_FILE);
}

   if (ewakuacja_flag) {
      printf(GREEN "[KASJER %d:%d] EWAKUACJA – kończę.\n"
         RESET, getpid(), kasjer_id);
   } else if (inwentaryzacja_flag) {
      printf(GREEN "[KASJER %d:%d] INWENTARYZACJA – kończę.\n"
         RESET, getpid(), kasjer_id);
   }
   // else {
   //     printf(GREEN"[KASJER %d:%d] Normalne wyjście z pętli.\n", getpid(), kasjer_id);
   // }

   exit(EXIT_SUCCESS);
}

// ------------------- KLIENT ------------------------
void klient_proces() {
   srand(time(NULL) ^ (getpid() << 16));

   int msgid = msgget(MSG_KEY, IPC_CREAT | 0600);
   if (msgid == -1) {
      perror("msgget klient");
      exit(1);
   }

   // Wejście do sklepu -> semafor
   sem_op(semid, SEM_STORE, -1);

   if (ewakuacja_flag) {
      sem_op(semid, SEM_STORE, 1);
      printf(BLUE "[KLIENT %d] EWAKUACJA – rezygnuję.\n"
         RESET, getpid());
      exit(EXIT_SUCCESS);
   }

   if (inwentaryzacja_flag) {
      sem_op(semid, SEM_STORE, 1);
      printf(BLUE "[KLIENT %d] ZAMKNIĘCIE – rezygnuję.\n"
         RESET, getpid());
      exit(EXIT_SUCCESS);
   }
   lock_semaphore(semid, SEM_MUTEX);
   shdata -> inStore++;
   int inStoreNow = shdata -> inStore;
   unlock_semaphore(semid, SEM_MUTEX);

   printf(BLUE "[KLIENT %d] Wchodzę do piekarni (inStore=%d)\n"
      RESET, getpid(), inStoreNow);

   // Losowanie 2-3 rodzajów produktów
   int ile_rodz = 2 + (rand() % 2);
   int kupowane[P];
   memset(kupowane, 0, sizeof(kupowane));

   for (int i = 0; i < ile_rodz; i++) {
      int prod_index = rand() % P;
      kupowane[prod_index] = (rand() % 3) + 1;
   }

   double mojRachunek = 0.0;
   for (int i = 0; i < P; i++) {
      if (kupowane[i] == 0) continue;

      lock_semaphore(semid, SEM_MUTEX);
      shdata -> podajnikQueue[i]++;
      int pq = shdata -> podajnikQueue[i];
      unlock_semaphore(semid, SEM_MUTEX);

      printf(BLUE "[KLIENT %d] Kolejka do podajnika %d (kolejka=%d)\n"
         RESET,
         getpid(), i, pq);
      sleep(1);

      lock_semaphore(semid, SEM_MUTEX);
      shdata -> podajnikQueue[i]--;
      if (shdata -> produkty[i] >= kupowane[i]) {
         shdata -> produkty[i] -= kupowane[i];
         double cost = kupowane[i] * shdata -> prices[i];
         mojRachunek += cost;
         printf(BLUE "[KLIENT %d] Pobieram %d szt. prod.%d (%.2f/szt). Zostało=%d\n"
            RESET,
            getpid(), kupowane[i], i, shdata -> prices[i], shdata -> produkty[i]);
      } else {
         printf(BLUE "[KLIENT %d] Za mało prod.%d, pomijam.\n"
            RESET, getpid(), i);
      }
      unlock_semaphore(semid, SEM_MUTEX);
   }

   // Wybieramy kasjera ACTIVE
   int possibleKasjerIDs[N_CASHIERS];
   int countPossible = 0;

   lock_semaphore(semid, SEM_MUTEX);
   for (int i = 0; i < N_CASHIERS; i++) {
      if (shdata -> cashierState[i] == CASHIER_ACTIVE) {
         possibleKasjerIDs[countPossible++] = i;
      }
   }

   // if (countPossible == 0) {
   //     possibleKasjerIDs[0] = 0;
   //     countPossible = 1;
   // }

   int chosen_cashier = possibleKasjerIDs[rand() % countPossible];
   unlock_semaphore(semid, SEM_MUTEX);

   printf(BLUE "[KLIENT %d] Wybrałem kasjera #%d, rachunek=%.2f\n"
      RESET,
      getpid(), chosen_cashier, mojRachunek);

   msg_request_t req;
   req.mtype = chosen_cashier + 1;
   req.pid = getpid();
   req.cost = mojRachunek;
   req.cashier_id = chosen_cashier;
   memcpy(req.products, kupowane, sizeof(kupowane));

   if (!ewakuacja_flag) {
      while (1) {
         if (msgsnd(msgid, & req, sizeof(req) - sizeof(long), 0) == -1) {
            if (errno == EINTR) {
               if (ewakuacja_flag) {
                  printf(BLUE "[KLIENT %d] Otrzymałem sygnał EWAKUACJA podczas wysyłania. Rezygnuję.\n"
                     RESET, getpid());
                  break;
               }

               continue;
            } else {
               perror("msgsnd klient->kasjer");

            }
         }

         break;
      }

      msg_response_t resp;
      while (1) {
         ssize_t rcv = msgrcv(msgid, & resp, sizeof(resp) - sizeof(long),
            getpid(), 0);
         if (rcv == -1) {
            if (errno == EINTR) {
               if (ewakuacja_flag) {

                  break;
               }

               if (inwentaryzacja_flag) {

                  continue;
               }
               continue;
            } else {
               perror("msgrcv klient<-kasjer");

            }
         } else {
            printf(BLUE "[KLIENT %d] Otrzymałem potwierdzenie obsługi (final cost=%.2f)\n"
               RESET,
               getpid(), resp.final_cost);
         }

         break;
      }

   }

   lock_semaphore(semid, SEM_MUTEX);
   shdata -> inStore--;
   int inS = shdata -> inStore;

   sem_op(semid, SEM_STORE, 1);
   if (ewakuacja_flag) {
      printf(BLUE "[KLIENT %d] Odkładam towar przy kasie i opuszczam piekarnię (inStore=%d)\n"
         RESET, getpid(), inS);
   } else {
      printf(BLUE "[KLIENT %d] Opuszczam piekarnię (inStore=%d)\n"
         RESET, getpid(), inS);
   }
   unlock_semaphore(semid, SEM_MUTEX);
   exit(EXIT_SUCCESS);
}

// ------------------- GENERATOR KLIENTOW ------------------------
void generator_klient_proces() {

   // for (int i = 0; i < N_TOTAL_CLIENTS; i++) {
   //    if (inwentaryzacja_flag || ewakuacja_flag) break;
   //    //  int sleep_duration = 2 + rand() % 9; 
   //    //  sleep(sleep_duration);
   //    pid_t pid = fork();
   //    if (pid == 0) {
   //       klient_proces();
   //       exit(EXIT_SUCCESS);
   //    } else if (pid < 0) {
   //       perror("fork KLIENT");
   //       clear();
   //       exit(EXIT_FAILURE);
   //    }

   // }
}

// ------------------- MAIN ------------------------
int main(int argc, char * argv[]) {


    int MAX_K, MAX_IN_STORE, T_p, T_k, N_TOTAL_CLIENTS;

    printf("Podaj wartości dla zmiennych:\n");

    MAX_K = get_valid_input("Podaj wartość MAX_K", 10, 20);
    MAX_IN_STORE = get_valid_input("Podaj wartość MAX_IN_STORE", 10, 100);
    T_p = get_valid_input("Podaj wartość T_p", 7, 12);
    T_k = get_valid_input("Podaj wartość T_k", 13, 23);
    N_TOTAL_CLIENTS = get_valid_input("Podaj wartość N_TOTAL_CLIENTS", 20, 10000);


    printf("\nWprowadzone wartości:\n");
    printf("MAX_K = %d\n", MAX_K);
    printf("MAX_IN_STORE = %d\n", MAX_IN_STORE);
    printf("T_p = %d\n", T_p);
    printf("T_k = %d\n", T_k);
    printf("N_TOTAL_CLIENTS = %d\n", N_TOTAL_CLIENTS);

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
   shdata = (shared_data_t * ) shmat(shmid, NULL, 0);
   if (shdata == (void * ) - 1) {
      perror("shmat");
      exit(EXIT_FAILURE);
   }
   memset(shdata, 0, sizeof( * shdata));

   srand(time(NULL));
   for (int i = 0; i < P; i++) {
      double base = (rand() % 401) + 100;
      shdata -> prices[i] = base / 100.0; // 1.00..5.00
   }

   semid = semget(SEM_KEY, 3, IPC_CREAT | 0600);
   if (semid == -1) {
      perror("semget");
      clear();
      exit(EXIT_FAILURE);
   } {
      union semun arg;
      unsigned short initValues[3];
      initValues[SEM_MUTEX] = 1;
      initValues[SEM_STORE] = MAX_IN_STORE;
      initValues[SEM_FILE] = 1;

      arg.array = initValues;
      if (semctl(semid, 0, SETALL, arg) == -1) {
         perror("semctl(SETALL)");
         clear();
         exit(EXIT_FAILURE);
      }
   }

   int msgid = msgget(MSG_KEY, IPC_CREAT | 0600);
   if (msgid == -1) {
      perror("msgget(main)");
      clear();
      exit(EXIT_FAILURE);
   }

   lock_semaphore(semid, SEM_FILE);
   FILE * f = fopen("raport.txt", "w");
   if (f) {
      fprintf(f, "[MAIN %d] Start symulacji – czyszczę raport.\n", getpid());
      fclose(f);
   }
   unlock_semaphore(semid, SEM_FILE);

   pid_t pid = fork();
   if (pid == 0) {
      manager_proces(T_k, T_p, MAX_IN_STORE);
   } else if (pid < 0) {
      perror("fork MANAGER");
      clear();
      exit(EXIT_FAILURE);
   }
   // pid = fork();
   // if(pid == 0) {
   //     generator_klient_proces();
   // } else if(pid < 0) {
   //     perror("fork generator");
   //     clear();
   //     exit(EXIT_FAILURE);
   // }

   for (int i = 0; i < 5; i++) {
      pid = fork();
      if (pid == 0) {
         piekarz_proces(MAX_K);
      } else if (pid < 0) {
         perror("fork PIEKARZ");
         clear();
         exit(EXIT_FAILURE);
      }
   }

   for (int i = 0; i < N_CASHIERS; i++) {
      pid = fork();
      if (pid == 0) {
         kasjer_proces(i);
      } else if (pid < 0) {
         perror("fork KASJER");
         clear();
         exit(EXIT_FAILURE);
      }
   }
   for (int i = 0; i < N_TOTAL_CLIENTS; i++) {
      pid = fork();
      if (pid == 0) {
         klient_proces();
      } else if (pid < 0) {
         perror("fork KLIENT");
         clear();
         exit(EXIT_FAILURE);
      }
   }

   // Czekamy na wszystkie procesy potomne
   while (wait(NULL) > 0) {}

   printf("\n[MAIN %d] Wszystkie procesy zakończyły pracę.\n", getpid());
   printf("Stan pamięci dzielonej:\n");
   for (int i = 0; i < P; i++) {
      printf("  Podajnik[%d] = %d szt. (cena=%.2f)\n",
         i, shdata -> produkty[i], shdata -> prices[i]);
   }
   for (int i = 0; i < N_CASHIERS; i++) {
      printf("  Kasjer #%d => state=%d\n",
         i, (int) shdata -> cashierState[i]);
   }
   printf("  totalRevenue = %.2f\n", shdata -> totalRevenue);
   printf("  inStore=%d\n", shdata -> inStore);

   clear();
   printf("[MAIN %d] Koniec programu.\n", getpid());
   return 0;
}