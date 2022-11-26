#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
//#include "pstat.h"

uint64
sys_exit(void)
{
  int n;
  if(argint(0, &n) < 0)
    return -1;
  exit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return fork();
}

uint64
sys_wait(void)
{
  uint64 p;
  if(argaddr(0, &p) < 0)
    return -1;
  return wait(p);
}

uint64
sys_sbrk(void)
{
  int addr;
  int n;

  if(argint(0, &n) < 0)
    return -1;
  addr = myproc()->sz;
  if(growproc(n) < 0)
    return -1;
  return addr;
}

uint64
sys_sleep(void)
{
  int n;
  uint ticks0;

  if(argint(0, &n) < 0)
    return -1;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(myproc()->killed){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64
sys_kill(void)
{
  int pid;

  if(argint(0, &pid) < 0)
    return -1;
  return kill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

uint64
sys_freepmem(void)
{
  return kfreepagecount();
}

//set given semaphore (and value) to the semaphore table
uint64
sys_sem_init(void)
{
  uint64 sem_addr;
  int pshared;
  int value;

  if(argaddr(0,&sem_addr) < 0)
    return -1;
  if(argint(1,&pshared)< 0)
    return -1;
  if(argint(2,&value)<0)
    return -1;

  int idx = semalloc(); //get free space in the table
  semtable.sem[idx].count = value;  //initialize given count

  copyout(myproc()->pagetable, sem_addr,(char*)&idx,sizeof(int));
  return (0);
}

uint64
sys_sem_wait(void){
  //aquires the values
  //checks if counts is greater than 0 -> subtract from count
  //if equal to 0 -> sleep (what moore wrote on the board) until count is > 0
  uint64 sem_addr;
  int idx = 0;

  if(argaddr(0,&sem_addr) < 0)
    return -1;

  acquire(&semtable.lock);

  acquire(&semtable.lock);
  while(semtable.sem->count == 0){
     sleep((void*)&semtable.sem[idx].lock,&semtable.sem[idx].lock);
     idx += 1;
  }
  semtable.sem->count -= 1;
  release(&semtable.lock);
  return(0);
}

//adds 1 to semtable count
uint64
sys_sem_post(void){
  uint64 sem_addr;
  //a0 address of users sem_t
  if(argaddr(0, &sem_addr) < 0)
    return -1;

  acquire(&semtable.lock);
  semtable.sem->count += 1;
  //uses wakeup function for any processes that are waiting on the post
  wakeup(&semtable.sem->lock);
  release(&semtable.lock);
  return(0);
};