#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>
#include <pthread.h>
#include <setjmp.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <semaphore.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include "ipc.h"

//IPC 
static struct ipc * addr; //address shared memory
static int shm_procBC_des;
static sem_t * sem_procBC_read;
static sem_t * sem_procBC_write;
//Threads mutex and cond
static pthread_mutex_t mutex_rw = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond_rw = PTHREAD_COND_INITIALIZER;
// square of user number
static unsigned int number;
static int pnumber_flag; // for proc C2: 0 - sleep(1), 1 - print number

static volatile sig_atomic_t term_flag;//0-work, 1-terminated process 
static volatile sig_atomic_t int_flag;//0-work, 1-terminated process 
static volatile sig_atomic_t canJmp; //0-before sigsetjmp, 1-after sigsetjmp 
static sigjmp_buf senv;


static void sig_term_handl(int sig) //Process C SIGTERM handler
{
//	printf("##########sigterm\n");
	term_flag = 1;
	if (canJmp == 1)
		siglongjmp(senv, 1);
}

static void sig_int_handl(int sig) //Process C SIGINT handler
{
//	printf("@@@@@@@@@@sigint\n");
	int_flag = 1;
	if (canJmp == 1)
		siglongjmp(senv, 1);
}

static void errMsg(const char * str)
{
	printf("Proc C error: %s. errno = %s\n", str, strerror(errno));
	return;
}

static void send_STerm_AB()
{
	// send process A SIGTERM
	if ((addr != 0) && (addr->A_pid != 0)){
		if (kill(addr->A_pid, SIGTERM) == -1)  //SIGTERM for process A 
			errMsg("kill");
	}else{
		printf("Proc C error: could not read pid process A\n");
	}
	// send process B SIGTERM
	if ((addr != 0) && (addr->B_pid != 0)){
		if (kill(addr->B_pid, SIGTERM) == -1)  //SIGTERM for process B 
			errMsg("kill");
	}else{
		printf("Proc C error: could not read pid process B\n");
	}
	return;
}

static void onExit(void)
{
	if (sem_close(sem_procBC_read) == -1)
		errMsg("sem_close");
	if (sem_close(sem_procBC_write) == -1)
		errMsg("sem_close");
	if (addr != 0)
		if (munmap(addr, sizeof(int)) == -1)
			errMsg("munmap");
	if (close(shm_procBC_des) == -1)
		errMsg("close");
	return;
}

static void errExit(const char * str)
{
	errMsg(str);
	send_STerm_AB();
	onExit();
	exit(EXIT_FAILURE);
}

static void * thread_C2_func (void * arg)
{
//	printf("Start thread C2\n");
	int i;
	unsigned int tmp;
	for (;;) {
		i = pthread_mutex_trylock(&mutex_rw);
		switch (i){
		case 0: //lock
			if (pnumber_flag == 1){
				tmp = number;
				pnumber_flag = 0;	
				if (pthread_mutex_unlock(&mutex_rw) != 0)
					errExit("pthread_mutex_unlock");
				if (pthread_cond_signal(&cond_rw) != 0)
					errExit("pthread_cond_signal");
				if (printf("square of user number = %u\n", tmp) < 0)
					errExit("printf");
			}else{
				if (pthread_mutex_unlock(&mutex_rw) != 0)
					errExit("pthread_mutex_unlock");
				if (pthread_cond_signal(&cond_rw) != 0)
					errExit("pthread_cond_signal");
				if (printf("proc C: I am alive\n") < 0)
					errExit("printf");
				sleep(1);
			}
			break;
		case EBUSY: //mutex busy
			if (printf("proc C: I am alive\n") < 0)
				errExit("printf");
			sleep(1);
			break;
		default: // error
			errExit("pthread_mutex_trylock");
		}
		pthread_testcancel();
	}
	return 0;
}


int main(void)
{
	//set new handler for SIGTERM and SIGINT	
	struct sigaction act;
	sigemptyset(&act.sa_mask);
	act.sa_handler = &sig_term_handl;
	act.sa_flags = 0;
	if (sigaction(SIGTERM, &act, NULL) == -1)
		errExit("sigaction");
	act.sa_handler = &sig_int_handl;
	if (sigaction(SIGINT, &act, NULL) == -1)
		errExit("sigaction");

	//create semophore
	if ((sem_procBC_read = sem_open(SEM_RNAME, O_RDWR, S_IRUSR|S_IWUSR, 0)) == SEM_FAILED){
		printf("maby main_A_C did not run\n");
		errExit("sem_open");
	}
	if ((sem_procBC_write = sem_open(SEM_WNAME, O_RDWR, S_IRUSR|S_IWUSR, 0)) == SEM_FAILED)
		errExit("sem_open");

	//create and mapped shared mamory
	if ((shm_procBC_des = shm_open(SHM_NAME, O_RDWR, S_IRUSR|S_IWUSR)) == -1)
		errExit("shm_open");
	if (ftruncate(shm_procBC_des, sizeof(struct ipc)) == -1)
		errExit("ftruncate");
	if ((addr = mmap(NULL, sizeof(struct ipc), PROT_WRITE, MAP_SHARED, shm_procBC_des, 0)) == MAP_FAILED)
		errExit("mmap");

	//exchange PID's between process B and C
	if (sem_wait(sem_procBC_write) == -1)
		errExit("sem_wait");
	addr->C_pid = getpid();	//send proc B C_pid
	if (sem_post(sem_procBC_read) == -1)	//unlock proc B
		errExit("sem_post");

//printf("Proc C: pid_C = %d\n", getpid());	
//printf("Proc C: pid_A = %d\n", addr->A_pid);	
//printf("Proc C: pid_B = %d\n", addr->B_pid);	

	if (pthread_mutex_lock(&mutex_rw) !=0)
		errExit("phread_mutex_lock");

	//create thread C2 
	pthread_t  thread_C2_des;
	if (pthread_create(&thread_C2_des, NULL, thread_C2_func, NULL) != 0)
		errExit("pthread_create");

	if (sigsetjmp(senv, 1) == 0){	//longjmp from handler SIGTERM
		canJmp = 1;
		//read user number from shared memory and set global var "number"
		for(;;){
			if (sem_wait(sem_procBC_read) == -1)
				errExit("sem_wait");			
			number = addr->num;
			if (sem_post(sem_procBC_write) == -1)
				errExit("sem_post");
		
			pnumber_flag = 1;

			if (pthread_cond_wait(&cond_rw, &mutex_rw) != 0)
				errExit("pthread_cond_wait");

			// send process B SIGUSER1
			if (number == 100)
				if (kill(addr->B_pid, SIGUSR1) == -1)
					errMsg("kill");
		}
	}
	
	if (pthread_mutex_trylock(&mutex_rw) == 0)
		if (pthread_mutex_unlock(&mutex_rw) != 0)
			errExit("phread_mutex_unlock");

	if (pthread_cancel(thread_C2_des) != 0)
		errMsg("pthread_cancel");

	if (pthread_join(thread_C2_des, NULL) != 0) 
		errMsg("pthread_join");

	if (int_flag == 1)
		send_STerm_AB();
	onExit();
	exit(EXIT_SUCCESS);
}


