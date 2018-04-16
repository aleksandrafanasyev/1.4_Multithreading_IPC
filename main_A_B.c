#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>
#include "ipc.h"
#include <sys/wait.h>
#include <setjmp.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <semaphore.h>
#include <sys/mmap.h>


static int procB_exit_code = EXIT_SUCCESS;

//send SIGTERM on exit if not NULL
static pid_t pid_proc_A;
static pid_t pid_proc_B;

//unlink on exit if not NULL
static struct ipc * addr; //address shared memory
static int shm_procBC_des;
static sem_t * sem_procBC_read;
static sem_t * sem_procBC_write;

static volatile sig_atomic_t term_flag; //0-work, 1-terminated process 

static volatile sig_atomic_t usr1_flag; //0-work, 1-terminated process 
static volatile sig_atomic_t canJmp; //0-before sigsetjmp, 1-after sigsetjmp 
static sigjmp_buf senv;

static void sig_term_handl(int sig) //Process A SIGTERM handler
{
	term_flag = 1;
}

static void sig_termB_handl(int sig) //Process B SIGTERM handler
{
	usr1_flag = 1;
}


// work after call sigsetjmp
static void sig_usr1_handl(int sig) //Process B SIGUSR1 handler
{
	usr1_flag = 1;
	if (canJmp)
		siglongjmp(senv, 1);
}


static void onExitB(void)
{
	if (kill(getppid(), SIGTERM) == -1)  //SIGTERM for process A
		errMsg("kill");
	if (addr && addr->C_pid){
		if (kill(addr->C_pid, SIGTERM) == -1)  //SIGTERM for process C 
			errMsg("kill");
		else{
			errMsg("could not read pid process C");
		}
	// deallocation resources and exit	
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
	if (close(pipedes[0]) == -1)
		errMsg("close");
	return;
}


static void errMsg(const char * str)
{
	switch (getpid()){
	case pid_proc_A:
		printf("Proc A error: %s. errno = %d\n", str, errno);
		break;
	case pid_proc_B:
		printf("Proc B error: %s. errno = %d\n", str, errno);
		procB_exit_code = EXIT_FAILURE; //used only for proc B
	}
	return;
}

// recognize process A and B
static void errExit(const char * str)
{
	switch (getpid()){ 
	case pid_proc_A:
		printf("Proc A error: %s. errno = %d\n", str, errno);
		if (!pid_proc_B)
			kill(pid_proc_B, SIGTERM); // for proc B 
		if (wait(NULL) == -1)
			errMsg("wait");
		exit(EXIT_FAILURE);
	case pid_proc_B:
		printf("Proc B error: %s. errno = %d\n", str, errno);
		onExitB();
		exit(EXIT_FAILURE);
	}
}

void procB_func(void) //Process B
{	
	//set new handler for SIGUSR1	
	struct sigaction act;
	sigemptyset(&act.sa_mask);
	act.sa_handler = &sig_usr1_handl;
	act.sa_flags = 0;
	if (sigaction(SIGUSR1, &act, NULL) == -1)
		errExit("sigaction");

	//set new handler for SIGTERM	
	struct sigaction act;
	sigemptyset(&act.sa_mask);
	act.sa_handler = &sig_termB_handl;
	act.sa_flags = 0;
	if (sigaction(SIGTERM, &act, NULL) == -1)
		errExit("sigaction");

	//create semophore
	if (sem_procBC_read = sem_open(SEM_RNAME, O_CREAT) == SEM_FAILED)
		errExit("sem_open");
	if (sem_procBC_write = sem_open(SEM_WNAME, O_CREAT) == SEM_FAILED)
		errExit("sem_open");
	
	//create and mapped shared mamory
	if (shm_procBC_des = shm_open(SHM_NAME, O_CREAT|O_RDWR|O_TRUNC, S_IRUSR|S_IWUSR) == -1)
		errExit("shm_open");
	if (ftruncate(shm_procBC_des, sizeof(ipc)) == -1)
		errExit("ftruncate");
	if (addr = mmap(NULL, sizeof(ipc), PROT_WRITE, MAP_SHARED, shm_procBC_des, 0) == MAP_FAILED)
		errExit("mmap");
	
	//exchange PID's between process B and C
	addr->B_pid = getpid();
	addr-C_pid = 0;
	if (sem_post(sem_procBC_write) == -1)	//unlock proc C
		errExit("sem_post");
	if (sem_wait(sem_procBC_read) == -1)
		errExit("sem_wait");

	//read from pipe and write to shared memory
	if (sem_post(sem_procBC_write) == -1)	//unlock for write
		errExit("sem_post");

	if (!sigsetjmp(senv, 1))	//longjmp from handler SIGUSR1
		canJmp = 1;

	while (!usr1_flag){
		ssize_t cnt;
		unsigned int i;
		if ((cnt = read(pipedes[0], &i, sizeof(int))) == -1)
			errExit("read");
		i = i * i;
		
		if (sem_wait(sem_procBC_write) == -1)
			errExit("sem_wait");
		addr->num = i;
		if (sem_post(sem_procBC_read) == -1)
			errExit("sem_post");
	}
	// deallocate resources
	onExitB();	
	return;
}
/*
 *  User input for integer type >= 2 byte
 */
int main(void) //Process A, that  fork process B
{
	pid_proc_A = getpid();
	int pipedes[2];
	if (pipe() == -1)
		errExit("pipe");
	
	switch (pid_proc_B = fork()){
	case -1:
		errExit("fork");
	case 0: //procB
		pid_proc_B = getpid();
		if (close(pipedes[1]) == -1)
			errExit("close");
		procB_func();
		exit(procB_exit_code);
	default://procA
		if (close(pipedes[0]) == -1)
			errExit("close");
		//set new handler for SIGTERM	
		struct sigaction act;
		sigemptyset(&act.sa_mask);
		act.sa_handler = &sig_term_handl;
		act.sa_flags = 0;
		if (sigaction(SIGTERM, &act, NULL) == -1)
			errExit("sigaction");
	}

	while (!term_flag){
		unsigned int i;		//int >= 2byte
		int cnt;
		printf("Введите число (0-256): ")
		if (cnt = scanf("%d", &i) == EOF)
			errExit("scanf");
		if (cnt == 0 || i > 256){
			printf("\nОшибка ввода\n");
			continue;
		}
		printf("Введенное число: %d\n", i);
		if (write(pipedes[1], &i, sizeof(int)) == -1)
			errExit("pipe");
		if (fflush(pipedes[1]))
			errExit("fflush");
	}

	if (close(pipedes[1]) == -1)
		errExit("close");
	if (wait(NULL) == -1)
		errExit("wait");
	exit(EXIT_SUCCESS);
}



