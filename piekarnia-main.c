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
#include <pthread.h>
#include <semaphore.h>

#define P 12   // Liczba rodzajów produktów (P>10)
#define MAX_K 10   // Maksymalna liczba sztuk produktu w podajniku
#define N 10         // Maksymalna liczba klientów w sklepie
#define KASY 3       // Liczba kas

#define SIG_EWAKUACJA     SIGUSR1
#define SIG_INWENTARYZACJA SIGUSR2

sem_t podajniki[P];
int produkty[P];       
sem_t wejscie;         
sem_t kolejka_kasy;   
sem_t kasy[KASY];       
int sprzedane_produkty[P];
int ewakuacja_flag = 0; 

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

void signal_handler(int sig) {
    if (sig == SIG_EWAKUACJA) {
        // Ewakuacja: klienci przerywają zakupsemy i opuszczają piekarnię
  
        printf("[PID %d] Otrzymano sygnał EWAKUACJA!\n", getpid());
    } else if (sig == SIG_INWENTARYZACJA) {
        // Inwentaryzacja: klienci kończą zakupy
        // robione jest podsumowanie przez kasy/piekarza/kierownika
        printf("[PID %d] Otrzymano sygnał INWENTARYZACJA!\n", getpid());
    } else {
        // Inne sygnały
        printf("[PID %d] Otrzymano nieznany sygnał: %d\n", getpid(), sig);
    }
}

void piekarz_proces() {

    
    printf("[PIEKARZ %d] Kończę pracę.\n", getpid());
  
}

void kasjer_proces() {
    srand(time(NULL) ^ (getpid()<<16));

    for(int i = 0; i < 5; i++) {
        
        printf("[KASJER %d] Oczekuję na klientów / obsługa...\n", getpid());
        sleep(2);
    }

    printf("[KASJER %d] Kończę pracę.\n", getpid());
    
}

void klient_proces() {
   
    printf("[KLIENT %d] Kończę zakupy.\n", getpid());

}

void kierownik_proces() {
  

    printf("[KIEROWNIK %d] Kończę pracę.\n", getpid());

}