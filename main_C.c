#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>
#include <pthread.h>
#include "ipc.h"
#include <setjmp.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <semaphore.h>
#include <sys/mmap.h>
#

static pid_t pid_C2;
static int proc_exit_code = EXIT_SUCCESS;
//IPC 
static struct ipc * addr; //address shared memory
static int shm_procBC_des;
static sem_t * sem_procBC_read;
static sem_t * sem_procBC_write;
//Threads mutex and cond
static pthread_mutex_t mutex_rw = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond_rw = PTHREAD_COND_INITIALIZER;
// square user number
static unsigned int number;

static volatile sig_atomic_t term_flag;//0-work, 1-terminated process 
static volatile sig_atomic_t wakeup_flag; //0-sleep(1), 1-don't sleep! 
static volatile sig_atomic_t canJmp; //0-before sigsetjmp, 1-after sigsetjmp 
static sigjmp_buf senv;


static void sig_term_handl(int sig) //Process C SIGTERM handler
{
	term_flag = 1;
	if (canJmp)
		siglongjmp(senv, 1);
}

static void sig_alarm_handl(int sig)
{
	wakeup_flag = 1;
}

static void onExit(void)
{
	if (sem_procBC_read){
		if (sem_close(sem_procBC_read) == -1)
			errMsg("sem_close");
		if (sem_unlink(SEM_RNAME) == -1)
			errMsg("sem_unlink");
	}
	if (sem_procBC_write){ 
		if (sem_close(sem_procBC_write) == -1)
			errMsg("sem_close");
		if (sem_unlink(SEM_WNAME) == -1)
			errMsg("sem_unlink");
	}
	if (addr)
		if (munmap(addr, sizeof(int)) == -1)
			errMsg("munmap");
	if (shm_procBC_des){
		if (close(shm_procBC_des) == -1)
			errMsg("close");
		if (shm_unlink(SHM_NAME) == -1)
			errMsg("shm_unlink");
	}
	return;
}

static void errMsg(const char * str)
{
	printf("Proc C error: %s. errno = %d\n", str, errno);
	procC_exit_code = EXIT_FAILURE;
	return;
}

static void errExit(const char * str)
{
	
	printf("Proc C error: %s. errno = %d\n", str, errno);
	onExit();
	exit(EXIT_FAILURE);
}

static void * thread_C2_func (void * arg)
{
	//set new handler for SIGALARM	
	struct sigaction act;
	sigemptyset(&act.sa_mask);
	act.sa_handler = &sig_alarm_handl;
	act.sa_flags = 0;
	if (sigaction(SIGALARM, &act, NULL) == -1)
		errExit("sigaction");

	for (;;) {
		int i;
		i = pthread_mutex_trylock(&mutex_rw);
		switch (i){
		case 0: //lock
			if (printf("square of user number = %u\n", number) < 0)
				errExit("printf");
			if (pthread_mutex_unlock(&mutex_rw))
				errExit("pthread_mutex_unlock");
			wakeup_flag = 0;
			if (pthread_cond_signal(&cond_rw))
				errExit("pthread_cond_signal");
			break;
		case EBUSY: //mutex busy
			if (printf("proc C: I am alive\n") < 0)
				errExit("printf");
			if (!wakeup_flag) sleep(1);
			break;
		default: // error
			errExit("pthread_mutex_trylock");
		}
		pthread_testcancel();
	}
}


int main(void)
{
	//set new handler for SIGTERM	
	struct sigaction act;
	sigemptyset(&act.sa_mask);
	act.sa_handler = &sig_term_handl;
	act.sa_flags = 0;
	if (sigaction(SIGTERM, &act, NULL) == -1)
		errExit("sigaction");

	//create semophore
	if (sem_procBC_read = sem_open(SEM_RNAME, O_CREAT) == SEM_FAILED)
		errExit("sem_open");
	if (sem_procBC_write = sem_open(SEM_WNAME, O_CREAT) == SEM_FAILED)
		errExit("sem_open");

	//create and mapped shared mamory
	if (shm_procBC_des = shm_open(SHM_NAME, O_RDWR, S_IRUSR|S_IWUSR) == -1)
		errExit("shm_open");
	if (ftruncate(shm_procBC_des, sizeof(ipc)) == -1)
		errExit("ftruncate");
	if (addr = mmap(NULL, sizeof(ipc), PROT_WRITE, MAP_SHARED, shm_procBC_des, 0) == MAP_FAILED)
		errExit("mmap");
	
	//exchange PID's between process B and C
	if (sem_wait(sem_procBC_write) == -1)
		errExit("sem_wait");
	addr-C_pid = getpid();	//send proc B C_pid
	if (sem_post(sem_procBC_read) == -1)	//unlock proc B
		errExit("sem_post");

	//create thread C2 
	pthread_t  thread_C2_des;
	if (pthread_create(&thread_C2_des, NULL, thread_C2_func, NULL))
		errExit("pthread_create");

	//read user number from shared memory and set global var "number"
	if (pthread_mutex_lock(&mutex_rw))
		errExit("phread_mutex_lock");

	if (!sigsetjmp(senv, 1))	//longjmp from handler SIGTERM
		canJmp = 1;

	while (!term_flag){
		if (sem_wait(sem_procBC_read) == -1)
			errExit("sem_wait");			
		number = addr->num;
		if (sem_post(sem_procBC_write) == -1)
			errExit("sem_post");

		// wake up thread C2 from sleep
		if (pthread_kill(pthread_C2_des, SIGALARM))
			errExit("pthread_kill");
	
		if (pthread_cond_wait(&cond_rw, &mutex_rw))
			errExit("pthread_cond_wait");

		// send process B SIGUSER1
		if (number == 100)
			if (kill(addr->B_pid, SIGUSR1) == -1)
				errMsg("kill");
	}
	
	if (!pthread_mutex_trylock(&mutex_rw))
		if (pthread_mutex_unlock(&mutex_rw))
			errExit("phread_mutex_unlock");

	if (prthread_cancel(thread_C2_des))
		errMsg("pthread_cancel");

	if (pthread_join(thread_C2_des, NULL)) 
		errMsg("pthread_join");

	onExit();
	exit(procC_exit_code);
}


