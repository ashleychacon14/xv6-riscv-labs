#define NPROC        64  // maximum number of processes
#define NCPU          8  // maximum number of CPUs
#define NOFILE       16  // open files per process
#define NFILE       100  // open files per system
#define NINODE       50  // maximum number of active i-nodes
#define NDEV         10  // maximum major device number
#define ROOTDEV       1  // device number of file system root disk
#define MAXARG       32  // max exec arguments
#define MAXOPBLOCKS  10  // max # of blocks any FS op writes
#define LOGSIZE      (MAXOPBLOCKS*3)  // max data blocks in on-disk log
#define NBUF         (MAXOPBLOCKS*3)  // size of disk block cache
#define FSSIZE       1000  // size of file system in blocks
#define MAXPATH      128   // maximum file path name
#define TSTICKSHIGH  1     // ticks per time slice for HIGH queue
#define TSTICKSMEDIUM 50   // ticks per time slice for MEDIUM queue
#define TSTICKSLOW 200     // ticks per time slice for LOW queue
#define NQUEUE 3           // Number of queues for MLFQ scheduler
#define HIGH 0             // High priority for scheduling
#define MEDIUM 1
#define LOW 2
#define MLFQ 1             // 0 for RR, 1 for MLFQ
#define RR 0
#define MAX_MMR 10         // maximum number of memory-mapped regions per process
#define NSEM 100           // max open semaphores per system

enum procstate { UNUSED, USED, SLEEPING, RUNNABLE, RUNNING, ZOMBIE };