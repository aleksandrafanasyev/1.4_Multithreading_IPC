#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <setjmp.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <semaphore.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <errno.h>
#include "ipc.h"

static int procB_exit_code = EXIT_SUCCESS;

//send SIGTERM on exit if not NULL
static pid_t pid_proc_A;
static pid_t pid_proc_B;

static int pipedes[2];
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

static void errMsg(const char * str)
{
	if (getpid() == pid_proc_A)
		printf("Proc A error: %s. errno = %d\n", str, errno);
	if (getpid() == pid_proc_B){
		printf("Proc B error: %s. errno = %d\n", str, errno);
		procB_exit_code = EXIT_FAILURE; //used only for proc B
	}
	return;
}

static void onExitB(void)
{
	if (kill(getppid(), SIGTERM) == -1)  //SIGTERM for process A
		errMsg("kill");
	if (addr && addr->C_pid){
		if (kill(addr->C_pid, SIGTERM) == -1)  //SIGTERM for process C 
			errMsg("kill");
	}else{
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

static void errExitA(const char * str)
{
	printf("Proc A error: %s. errno = %d\n", str, errno);
	if (!pid_proc_B)
		kill(pid_proc_B, SIGTERM); // for proc B 
	if (wait(NULL) == -1)
		errMsg("wait");
	exit(EXIT_FAILURE);
}

static void errExitB(const char * str)
{
	printf("Proc B error: %s. errno = %d\n", str, errno);
	onExitB();
	exit(EXIT_FAILURE);
}

void procB_func(void) //Process B
{	
	//set new handler for SIGUSR1	
	struct sigaction act;
	sigemptyset(&act.sa_mask);
	act.sa_handler = &sig_usr1_handl;
	act.sa_flags = 0;
	if (sigaction(SIGUSR1, &act, NULL) == -1)
		errExitB("sigaction");

	//set new handler for SIGTERM	
	sigemptyset(&act.sa_mask);
	act.sa_handler = &sig_termB_handl;
	act.sa_flags = 0;
	if (sigaction(SIGTERM, &act, NULL) == -1)
		errExitB("sigaction");

	//create semophore
	if ((sem_procBC_read = sem_open(SEM_RNAME, O_CREAT))== SEM_FAILED)
		errExitB("sem_open");
	if ((sem_procBC_write = sem_open(SEM_WNAME, O_CREAT)) == SEM_FAILED)
		errExitB("sem_open");
	
	//create and mapped shared mamory
	if ((shm_procBC_des = shm_open(SHM_NAME, O_CREAT|O_RDWR|O_TRUNC, S_IRUSR|S_IWUSR)) == -1)
		errExitB("shm_open");
	if (ftruncate(shm_procBC_des, sizeof(struct ipc)) == -1)
		errExitB("ftruncate");
	if ((addr = mmap(NULL, sizeof(struct ipc), PROT_WRITE, MAP_SHARED, shm_procBC_des, 0)) == MAP_FAILED)
		errExitB("mmap");
	
	//exchange PID's between process B and C
	addr->B_pid = getpid();
	addr->C_pid = 0;
	if (sem_post(sem_procBC_write) == -1)	//unlock proc C
		errExitB("sem_post");
	if (sem_wait(sem_procBC_read) == -1)
		errExitB("sem_wait");

	//read from pipe and write to shared memory
	if (sem_post(sem_procBC_write) == -1)	//unlock for write
		errExitB("sem_post");

	if (!sigsetjmp(senv, 1))	//longjmp from handler SIGUSR1
		canJmp = 1;

	while (!usr1_flag){
		ssize_t cnt;
		unsigned int i;
		if ((cnt = read(pipedes[0], &i, sizeof(int))) == -1)
			errExitB("read");
		i = i * i;
		
		if (sem_wait(sem_procBC_write) == -1)
			errExitB("sem_wait");
		addr->num = i;
		if (sem_post(sem_procBC_read) == -1)
			errExitB("sem_post");
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
	if (pipe(pipedes) == -1)
		errExitA("pipe");
	
	switch (pid_proc_B = fork()){
	case -1:
		errExitA("fork");
	case 0: //procB
		pid_proc_B = getpid();
		if (close(pipedes[1]) == -1)
			errExitA("close");
		procB_func();
		exit(procB_exit_code);
	default://procA
		if (close(pipedes[0]) == -1)
			errExitA("close");
		//set new handler for SIGTERM	
		struct sigaction act;
		sigemptyset(&act.sa_mask);
		act.sa_handler = &sig_term_handl;
		act.sa_flags = 0;
		if (sigaction(SIGTERM, &act, NULL) == -1)
			errExitA("sigaction");
	}

	while (!term_flag){
		unsigned int i;		//int >= 2byte
		int cnt;
		printf("Введите число (0-256): ");
		if ((cnt = scanf("%d", &i)) == EOF)
			errExitA("scanf");
		if (cnt == 0 || i > 256){
			printf("\nОшибка ввода\n");
			continue;
		}
		printf("Введенное число: %d\n", i);
		if (write(pipedes[1], &i, sizeof(int)) == -1)
			errExitA("pipe");
//		if (fflush(pipedes[1]))
//			errExitA("fflush");
	}

	if (close(pipedes[1]) == -1)
		errExitA("close");
	if (wait(NULL) == -1)
		errExitA("wait");
	exit(EXIT_SUCCESS);
}



