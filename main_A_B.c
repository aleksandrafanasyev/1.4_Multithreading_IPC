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
#include <string.h>
#include "ipc.h"


#define MAXLINE 256
static int proc_exit_code = EXIT_SUCCESS;

static pid_t pid_proc_A;
static pid_t pid_proc_B;

static int pipedes[2];

static struct ipc * addr; //address shared memory
static int shm_procBC_des;
static sem_t * sem_procBC_read;
static sem_t * sem_procBC_write;

static volatile sig_atomic_t term_flag; //0-work, 1-terminated process 
static volatile sig_atomic_t usr1_flag; //0-work, 1-terminated process 
static volatile sig_atomic_t int_flag; //0-work, 1-terminated process 
static volatile sig_atomic_t canJmp; //0-before sigsetjmp, 1-after sigsetjmp 
static sigjmp_buf senv;

static void sig_term_handl(int sig) //Process A SIGTERM and SIGINT handler
{
//	printf("#########################\n");
	term_flag = 1;
}

static void sig_termB_handl(int sig) //Process B SIGTERM handler
{
//	printf("@@@@@@@@@@@@@@@@@@@@@@\n");
	term_flag = 1;
	if (canJmp == 1)
		siglongjmp(senv, 1);
}

static void sig_intB_handl(int sig) //Process B SIGINT handler
{
//	printf("@@@@@@@@@@@@@@@@@@@@@@\n");
	int_flag = 1;
	if (canJmp == 1)
		siglongjmp(senv, 1);
}

static void sig_usr1B_handl(int sig) //Process B SIGUSR1 handler
{
	usr1_flag = 1;
	if (canJmp == 1)
		siglongjmp(senv, 1);
}

static void errMsg(const char * str1, const char * str2)
{
	printf("Proc %s error: %s. errno = %s\n", str1, str2, strerror(errno));
	proc_exit_code = EXIT_FAILURE; //used only for proc B
	return;
}

static void onExitB(void) // deallocation resources 
{
//	printf("1111111111111111 \n");	
	
	if (sem_close(sem_procBC_read) == -1)
		errMsg("B", "sem_close");
	if (sem_unlink(SEM_RNAME) == -1)
		errMsg("B", "sem_unlink");
	
	if (sem_close(sem_procBC_write) == -1)
		errMsg("B", "sem_close");
	if (sem_unlink(SEM_WNAME) == -1)
		errMsg("B", "sem_unlink");
	
	if (munmap(addr, sizeof(struct ipc)) == -1)
		errMsg("B", "munmap");

	if (close(shm_procBC_des) == -1)
		errMsg("B", "close");
	if (shm_unlink(SHM_NAME) == -1)
		errMsg("B", "shm_unlink");
	
	if (close(pipedes[0]) == -1)
		errMsg("B", "close");
	return;
}

static void errExitA(const char * str)
{
	errMsg("A", str);
	if (pid_proc_B != 0)
		kill(pid_proc_B, SIGTERM); // for proc B 
	if (wait(NULL) == -1)
		errMsg("A", "wait");
	exit(EXIT_FAILURE);
}

static void errExitB(const char * str)
{
	errMsg("B", str);

	if (kill(getppid(), SIGTERM) == -1)  //SIGTERM for process A
		errMsg("B", "kill");
	if ((addr != 0) && (addr->C_pid != 0)){
		if (kill(addr->C_pid, SIGTERM) == -1)//SIGTERM for process C 
			errMsg("B", "kill");
	}else{
		printf("Proc B error: could not read pid process C\n");
	}

	onExitB(); // deallocate resources
	exit(EXIT_FAILURE);
}

void procB_func(void) //Process B
{	
	//set new handler for SIGUSR1	
	struct sigaction act;
	sigemptyset(&act.sa_mask);
	act.sa_handler = &sig_usr1B_handl;
	act.sa_flags = 0;
	if (sigaction(SIGUSR1, &act, NULL) == -1)
		errExitB("sigaction");

	//set new handler for SIGTERM and SIGINT	
	sigemptyset(&act.sa_mask);
	act.sa_handler = &sig_termB_handl;
	act.sa_flags = 0;
	if (sigaction(SIGTERM, &act, NULL) == -1)
		errExitB("sigaction");
	act.sa_handler = &sig_intB_handl;
	if (sigaction(SIGINT, &act, NULL) == -1)
		errExitB("sigaction");

	//create semophore
	if ((sem_procBC_read = sem_open(SEM_RNAME, O_CREAT|O_EXCL|O_RDWR, S_IRUSR|S_IWUSR, 0)) == SEM_FAILED)
		errExitB("sem_open SEM_RNAME");
	if ((sem_procBC_write = sem_open(SEM_WNAME, O_CREAT|O_EXCL|O_RDWR, S_IRUSR|S_IWUSR, 0)) == SEM_FAILED)
		errExitB("sem_open SEM_WNAME");
	
	//create and mapped shared mamory
	if ((shm_procBC_des = shm_open(SHM_NAME, O_CREAT|O_EXCL|O_RDWR|O_TRUNC, S_IRUSR|S_IWUSR)) == -1)
		errExitB("shm_open");
	if (ftruncate(shm_procBC_des, sizeof(struct ipc)) == -1)
		errExitB("ftruncate");
	if ((addr = mmap(NULL, sizeof(struct ipc), PROT_WRITE, MAP_SHARED, shm_procBC_des, 0)) == MAP_FAILED)
		errExitB("mmap");
	
	//exchange PID's between process B and C
	addr->A_pid = getppid();
	addr->B_pid = getpid();
	addr->C_pid = 0;
	if (sem_post(sem_procBC_write) == -1)	//unlock proc C
		errExitB("sem_post");
	if (sem_wait(sem_procBC_read) == -1)
		errExitB("sem_wait");
	
//printf("Proc B: pid_B = %d\n", getpid());	
//printf("Proc B: pid_C = %d\n", addr->C_pid);	

	//read from pipe and write to shared memory
	if (sem_post(sem_procBC_write) == -1)	//unlock for write
		errExitB("sem_post");

	if (sigsetjmp(senv, 1) == 0){	//longjmp from handler SIGUSR1
		canJmp = 1;
		for(;;){
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
	}

	if (usr1_flag == 1){//exit form SUGUSR1
		if (kill(getppid(), SIGTERM) == -1)  //SIGTERM for process A
			errMsg("B", "kill");
		if ((addr != 0) && (addr->C_pid != 0)){
			if (kill(addr->C_pid, SIGTERM) == -1)//SIGTERM for process C
				errMsg("B", "kill");
		}else{
			printf("Proc B error: could not read pid process C\n");
		}
	}
	if (int_flag == 1){// exit from SIGINT
		if ((addr != 0) && (addr->C_pid != 0)){
			if (kill(addr->C_pid, SIGTERM) == -1)//SIGTERM for process C
				errMsg("B", "kill");
		}else{
			printf("Proc B error: could not read pid process C\n");
		}
	}

	onExitB();// deallocate resources
	return;
}
/*
 *  User input for integer type >= 2 byte
 */
int main(void) //Process A, that fork process B
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
			errMsg("B", "close");
		procB_func();
		exit(proc_exit_code);
	default://procA
		if (close(pipedes[0]) == -1)
			errMsg("A", "close");
		//set new handler for SIGTERM	
		struct sigaction act;
		sigemptyset(&act.sa_mask);
		act.sa_handler = &sig_term_handl;
		act.sa_flags = 0;
		if (sigaction(SIGTERM, &act, NULL) == -1)
			errExitA("sigaction");
		if (sigaction(SIGINT, &act, NULL) == -1)
			errExitA("sigaction");
	}

	while (term_flag == 0){
		char str[MAXLINE];
		unsigned int i;		//int >= 2byte
		int cnt;
		printf("Введите число (0-256):\n");
		
		if (fgets(str, MAXLINE, stdin) == 0){
			printf("Ошибка ввода\n");
			continue;
		}
		if ((cnt = sscanf(str, "%d", &i)) == EOF)
			errExitA("scanf");
		if (cnt == 0 || i > 256){
			printf("Ошибка ввода\n");
			continue;
		}
		printf("Введенное число: %d\n", i);
		if (write(pipedes[1], &i, sizeof(int)) == -1)
			errExitA("pipe");
	}

	if (close(pipedes[1]) == -1)
		errMsg("A", "close");
	if (wait(NULL) == -1)
		errMsg("A", "wait");
	exit(proc_exit_code);
}



