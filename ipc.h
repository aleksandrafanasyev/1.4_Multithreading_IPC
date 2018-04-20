
#define SHM_NAME "/shm.procB"
#define SEM_RNAME "/procB_rd"
#define SEM_WNAME "/procB_wr"

struct ipc{
	int num;
	pid_t A_pid;
	pid_t B_pid;
	pid_t C_pid;
};
