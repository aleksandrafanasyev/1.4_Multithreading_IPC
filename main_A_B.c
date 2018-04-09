#include <stdio.h>
#include <signal.h>
#include <unistd.h>


#define SHM_NAME "/shm.procB"
#define SEM_RNAME "/sem.procB_read"
#define SEM_WNAME "/sem.procB_write"

//unlink if not NULL
int shm_procB_des;
sem_t * sem_procB_read;
sem_t * sem_procB_write;

static volatile sig_atomic_t term_flag; //0-work, 1-terminated process 

static volatile sig_atomic_t usr1_flag; //0-work, 1-terminated process 

static void sig_term_handl(int sig) //Process A SIGTERM handler
{
	term_flag = 1;
}

static void sig_usr1_handl(int sig) //Process B SIGUSR1 handler
{
	usr1_flag = 1;
}

void errExit(const char * str)
{
	printf("error:%s . errno = %d\n", str, errno);
	if (!shm_procB_des)
		shm_unlink(shm_procB_des);
	if (!sem_procB_read)
		sem_unlink(sem_procB_read);
	if (!sem_procB_write)
		sem_unlink(sem_procB_write);
	exit(EXTE_FAILURE);
}

void procB_func(void) //Process B
{
	//create, mapped and close shared mamory
	if (shm_procB_des = shm_open(SHM_NAME, O_CREAT|O_RDWR|O_TRUNC, S_IRUSR|S_IWUSR) == -1)
		errExit("shm_open");
	if (ftruncate(shm_procB_des, sizeof(int)) == -1)
		errExit("ftruncate");
	void * addr;
	if (addr = mmap(NULL, sizeof(int), PROT_WRITE, MAP_SHARED, shm_procB_des, 0) == MAP_FAILED)
		errExit("mmap");
	
	//create semophore
	if (sem_procB_read = sem_open(SEM_RNAME, O_CREAT) == SEM_FAILED)
		errExit("sem_open");
	if (sem_procB_write = sem_open(SEM_WNAME, O_CREAT) == SEM_FAILED)
		semunlink(SEM_WNAME);
		errExit("sem_open");
	
	//read from pipe and write to shared memory
	if (sem_post(sem_write) == -1)	//unlock for write
		errExit("sem_post");
	unsigned int i;
	while (!usr1_flag){
		ssize_t cnt;
		if ((cnt = read(pipedes[0], &i, sizeof(int))) == -1)
			errExit("read");
		i = i * i;
		
		if (sem_wait(sem_pricB_write) == -1)// wait for write
			errExit("sem_wait");
		memcpy(addr, &i, sizeof(int));
		if (sem_post(sem_procB_read) == -1)//unlock read
			errExit("sem_post");
	}

	//send SIGTERM for process A and C





	// deallocation resources and exit	
	if (sem_close(sem_procB_read) == -1)
		errExit("sem_close");
	sem_procB_read = 0;
	if (sem_unlink(SEM_RNAME) == -1)
		errExit("sem_unlink");
	if (sem_close(sem_procB_write) == -1)
		errExit("sem_close");
	sem_pricB_write = 0;
	if (sem_unlink(SEM_WNAME) == -1)
		errExit("sem_unlink");

	if (munmap(addr, sizeof(int)) == -1)
		errExit("munmap");
	if (close(shm_procB_des) == -1)
		errExit("close");
	shm_pricB_des = 0;
	if (shm_unlink(SHM_NAME) == -1)
		errExit("shm_unlink");

	if (close(pipedes[0]) == -1)
		errExit("close");
	return;
}
/*
 *  User input for integer type >= 2 byte
 */
int main(void) //Process A, that  fork process B
{
	int pipedes[2];
	if (pipe() == -1)
		errExit("pipe");
	
	switch (fork()){
	case -1:
		errExit("fork");
	case 0: //procB
		if (close(pipedes[1]) == -1)
			errExit("close");
		procB_func();
		exit(EXIT_SUCCESS);
	default://procA
		if (close(pipedes[0]) == -1)
			errExit("close");
		
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
		if (fflush(pipedes[1]) != 0)
			errExit("fflush");
	}

	if (close(pipedes[1]) == -1)
		errExit("close");
	if (wait(NULL) == -1)
		errExit("wait");
	exit(EXIT_SUCCESS);
}



