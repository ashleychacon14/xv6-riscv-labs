#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "pstat.h"
#include "stat.h"
//#include <string.h>

struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct proc *initproc;

int nextpid = 1;
struct spinlock pid_lock;

extern void forkret(void);
static void freeproc(struct proc *p);

static int enqueue_at_tail(struct proc *p, int priority);

static int enqueue_at_head(struct proc *p, int priority);
static struct proc *dequeue(int priority);

extern char trampoline[]; // trampoline.S

struct queue queue[NQUEUE];

int sched_policy = RR; // Should be set to RR or MLFQ

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;
struct spinlock listid_lock;
struct spinlock pid_lock;
struct mmr_list mmr_list[NPROC+MAX_MMR];

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void proc_mapstacks(pagetable_t kpgtbl)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++)
  {
    char *pa = kalloc();
    if (pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int)(p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

// initialize the proc table at boot time.
void procinit(void)
{
  struct proc *p;

  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
  for (p = proc; p < &proc[NPROC]; p++)
  {
    initlock(&p->lock, "proc");
    p->kstack = KSTACK((int)(p - proc));
  }
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu *
mycpu(void)
{
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc *
myproc(void)
{
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

int allocpid()
{
  int pid;

  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
static struct proc *
allocproc(void)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++)
  {
    acquire(&p->lock);
    if (p->state == UNUSED)
    {
      goto found;
    }
    else
    {
      release(&p->lock);
    }
  }
  return 0;

found:
  p->pid = allocpid();
  p->state = USED;
  p->cputime = 0;
  p->priority = HIGH;
  p->timeslice = TSTICKSHIGH;
  p->yielded = 0;
  p->next = 0;

  // Allocate a trapframe page.
  if ((p->trapframe = (struct trapframe *)kalloc()) == 0)
  {
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if (p->pagetable == 0)
  {
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;

  return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{
  if (p->trapframe)
    kfree((void *)p->trapframe);
  p->trapframe = 0;
  int dofree = 0;

  for (int i = 0; i < MAX_MMR; i++) {

  dofree = 0;

  if (p->mmr[i].valid == 1) {

  if (p->mmr[i].flags & MAP_PRIVATE){
    dofree = 1;
  }

  else { // MAP_SHARED
    acquire(&mmr_list[p->mmr[i].mmr_family.listid].lock);

  if (p->mmr[i].mmr_family.next == &(p->mmr[i].mmr_family)) { // no other family members
    dofree = 1;
    release(&mmr_list[p->mmr[i].mmr_family.listid].lock);
    dealloc_mmr_listid(p->mmr[i].mmr_family.listid);

  } else { // remove p from mmr family
    (p->mmr[i].mmr_family.next)->prev = p->mmr[i].mmr_family.prev;
    (p->mmr[i].mmr_family.prev)->next = p->mmr[i].mmr_family.next;
    release(&mmr_list[p->mmr[i].mmr_family.listid].lock);
  }

  }

  // Remove region mappings from page table

  for (uint64 addr = p->mmr[i].addr; addr < p->mmr[i].addr + p->mmr[i].length; addr += PGSIZE)
    if (walkaddr(p->pagetable, addr))
      uvmunmap(p->pagetable, addr, 1, dofree);

  }
  

}
  if (p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->state = UNUSED;
}

// Create a user page table for a given process,
// with no user memory, but with trampoline pages.
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if (pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if (mappages(pagetable, TRAMPOLINE, PGSIZE,
               (uint64)trampoline, PTE_R | PTE_X) < 0)
  {
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe just below TRAMPOLINE, for trampoline.S.
  if (mappages(pagetable, TRAPFRAME, PGSIZE,
               (uint64)(p->trapframe), PTE_R | PTE_W) < 0)
  {
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, sz);
}

// a user program that calls exec("/init")
// od -t xC initcode
uchar initcode[] = {
    0x17, 0x05, 0x00, 0x00, 0x13, 0x05, 0x45, 0x02,
    0x97, 0x05, 0x00, 0x00, 0x93, 0x85, 0x35, 0x02,
    0x93, 0x08, 0x70, 0x00, 0x73, 0x00, 0x00, 0x00,
    0x93, 0x08, 0x20, 0x00, 0x73, 0x00, 0x00, 0x00,
    0xef, 0xf0, 0x9f, 0xff, 0x2f, 0x69, 0x6e, 0x69,
    0x74, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00};

// Set up first user process.
void userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;

  // allocate one user page and copy init's instructions
  // and data into it.
  uvminit(p->pagetable, initcode, sizeof(initcode));
  p->sz = PGSIZE;

  // prepare for the very first "return" from kernel to user.
  p->trapframe->epc = 0;     // user program counter
  p->trapframe->sp = PGSIZE; // user stack pointer

  p->cur_max = MAXVA - 2*PGSIZE;

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  p->state = RUNNABLE;
  enqueue_at_head(p, p->priority); // Enqueue to tail
  // printf("userinit\n");

  release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int growproc(int n)
{
  uint sz;
  struct proc *p = myproc();

  sz = p->sz;
  if (n > 0)
  {
    if ((sz = uvmalloc(p->pagetable, sz, sz + n)) == 0)
    {
      return -1;
    }
  }
  else if (n < 0)
  {
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if ((np = allocproc()) == 0)
  {
    return -1;
  }

  // Copy user memory from parent to child.
  if (uvmcopy(p->pagetable, np->pagetable, 0, p->sz) < 0)
  {
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;
  np->cur_max = p->cur_max;

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for (i = 0; i < NOFILE; i++)
    if (p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  // Copy mmr table from parent to child

memmove((char*)np->mmr, (char *)p->mmr, MAX_MMR*sizeof(struct mmr));

// For each valid mmr, copy memory from parent to child, allocating new memory for

// private regions but not for shared regions, and add child to family for shared regions.

for (int i = 0; i < MAX_MMR; i++) {

if(p->mmr[i].valid == 1) {

if(p->mmr[i].flags & MAP_PRIVATE) {

for (uint64 addr = p->mmr[i].addr; addr < p->mmr[i].addr+p->mmr[i].length; addr += PGSIZE)

if(walkaddr(p->pagetable, addr))

if(uvmcopy(p->pagetable, np->pagetable, addr, addr+PGSIZE) < 0) {

freeproc(np);

release(&np->lock);

return -1;

}

np->mmr[i].mmr_family.proc = np;

np->mmr[i].mmr_family.listid = -1;

np->mmr[i].mmr_family.next = &(np->mmr[i].mmr_family);

np->mmr[i].mmr_family.prev = &(np->mmr[i].mmr_family);

} else { // MAP_SHARED

for (uint64 addr = p->mmr[i].addr; addr < p->mmr[i].addr+p->mmr[i].length; addr += PGSIZE)

if(walkaddr(p->pagetable, addr))

if(uvmcopyshared(p->pagetable, np->pagetable, addr, addr+PGSIZE) < 0) {

freeproc(np);

release(&np->lock);

return -1;

}

// add child process np to family for this mapped memory region

np->mmr[i].mmr_family.proc = np;

np->mmr[i].mmr_family.listid = p->mmr[i].mmr_family.listid;

acquire(&mmr_list[p->mmr[i].mmr_family.listid].lock);

np->mmr[i].mmr_family.next = p->mmr[i].mmr_family.next;

p->mmr[i].mmr_family.next = &(np->mmr[i].mmr_family);

np->mmr[i].mmr_family.prev = &(p->mmr[i].mmr_family);

if (p->mmr[i].mmr_family.prev == &(p->mmr[i].mmr_family))

p->mmr[i].mmr_family.prev = &(np->mmr[i].mmr_family);

release(&mmr_list[p->mmr[i].mmr_family.listid].lock);

}

}

}

  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&np->lock);
  np->state = RUNNABLE;
  enqueue_at_tail(np, np->priority);
  release(&np->lock);

  return pid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void reparent(struct proc *p)
{
  struct proc *pp;

  for (pp = proc; pp < &proc[NPROC]; pp++)
  {
    if (pp->parent == p)
    {
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void exit(int status)
{
  struct proc *p = myproc();

  if (p == initproc)
    panic("init exiting");

  // Close all open files.
  for (int fd = 0; fd < NOFILE; fd++)
  {
    if (p->ofile[fd])
    {
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  acquire(&wait_lock);

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup(p->parent);

  acquire(&p->lock);

  p->xstate = status;
  p->state = ZOMBIE;

  release(&wait_lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int wait(uint64 addr)
{
  struct proc *np;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for (;;)
  {
    // Scan through table looking for exited children.
    havekids = 0;
    for (np = proc; np < &proc[NPROC]; np++)
    {
      if (np->parent == p)
      {
        // make sure the child isn't still in exit() or swtch().
        acquire(&np->lock);

        havekids = 1;
        if (np->state == ZOMBIE)
        {
          // Found one.
          pid = np->pid;
          if (addr != 0 && copyout(p->pagetable, addr, (char *)&np->xstate,
                                   sizeof(np->xstate)) < 0)
          {
            release(&np->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(np);
          release(&np->lock);
          release(&wait_lock);
          return pid;
        }
        release(&np->lock);
      }
    }

    // No point waiting if we don't have any children.
    if (!havekids || p->killed)
    {
      release(&wait_lock);
      return -1;
    }

    // Wait for a child to exit.
    sleep(p, &wait_lock); // DOC: wait-sleep
  }
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
// Add cputime
int 
sys_wait2(uint64 addr1, uint64 addr2)
{
  struct proc *np;
  int havekids, pid;
  struct proc *p = myproc();
  struct rusage time;

  acquire(&wait_lock);

  for (;;)
  {
    // Scan through table looking for exited children.
    havekids = 0;
    for (np = proc; np < &proc[NPROC]; np++)
    {
      if (np->parent == p)
      {
        // make sure the child isn't still in exit() or swtch().
        acquire(&np->lock);

        havekids = 1;
        if (np->state == ZOMBIE)
        {
          // Found one.
          pid = np->pid;
          time.cpu_time = np->cputime;

          if (addr1 != 0 && copyout(p->pagetable, addr1, (char *)&np->xstate,
                                    sizeof(np->xstate)) < 0)
          {
            release(&np->lock);
            release(&wait_lock);
            return -1;
          }
          printf("cpu time: %d\n", time.cpu_time);
          if (addr2 != 0 && copyout(p->pagetable, addr2, (char *)&time,
                                    sizeof(time)) < 0)
          {
            release(&np->lock);
            release(&wait_lock);
            return -1;
          }

          freeproc(np);
          release(&np->lock);
          release(&wait_lock);
          return pid;
        }
        release(&np->lock);
      }
    }

    // No point waiting if we don't have any children.
    if (!havekids || p->killed)
    {
      release(&wait_lock);
      return -1;
    }

    // Wait for a child to exit.
    sleep(p, &wait_lock); // DOC: wait-sleep
  }
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
void scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();

  c->proc = 0;
  for (;;)
  {
    if (sched_policy == RR)
    {
      // Avoid deadlock by ensuring that devices can interrupt.
      intr_on();

      for (p = proc; p < &proc[NPROC]; p++)
      {
        acquire(&p->lock);
        if (p->state == RUNNABLE)
        {
          // Switch to chosen process.  It is the process's job
          // to release its lock and then reacquire it
          // before jumping back to us.
          p->state = RUNNING;
          c->proc = p;

          swtch(&c->context, &p->context);

          // Process is done running for now.
          // It should have changed its p->state before coming back.
          c->proc = 0;
          p->tsticks = 0;

        }
        release(&p->lock);
      }
    }
    else if (sched_policy == MLFQ)
    {
      p = dequeue(HIGH);

      if (!p)
      {
        p = dequeue(MEDIUM);
      }
      if (!p)
      {
        p = dequeue(LOW);
      }
      // printf("Pointer2: %p\n",p); // Debugging pointer
      if (p)
      {
        // Avoid deadlock by ensuring that devices can interrupt.
        intr_on();
        acquire(&p->lock);
        if (p->state == RUNNABLE)
        {
          // Switch to chosen process.  It is the process's job
          // to release its lock and then reacquire it
          // before jumping back to us.
          p->state = RUNNING;
          c->proc = p;

          swtch(&c->context, &p->context);

          // Process is done running for now.
          // It should have changed its p->state before coming back.
          c->proc = 0;
          p->tsticks = 0;
        }
        release(&p->lock);
      }
    }
  }
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void sched(void)
{
  int intena;
  struct proc *p = myproc();

  if (!holding(&p->lock))
    panic("sched p->lock");
  if (mycpu()->noff != 1)
    panic("sched locks");
  if (p->state == RUNNING)
    panic("sched running");
  if (intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = RUNNABLE;
  enqueue_at_tail(p, p->priority);
  // printf("yield\n");
  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by ()
// will swtch to forkret.
void forkret(void)
{
  static int first = 1;

  // Still holding p->lock from scheduler.
  release(&myproc()->lock);

  if (first)
  {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    first = 0;
    fsinit(ROOTDEV);
  }

  usertrapret();
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();

  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.

  acquire(&p->lock); // DOC: sleeplock1
  release(lk);

  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  release(&p->lock);
  acquire(lk);
}

// Wake up all processes sleeping on chan.
// Must be called without any p->lock.
void wakeup(void *chan)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++)
  {
    if (p != myproc())
    {
      acquire(&p->lock);
      if (p->state == SLEEPING && p->chan == chan)
      {
        p->state = RUNNABLE;
        enqueue_at_head(p, p->priority);
      }
      release(&p->lock);
    }
  }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int kill(int pid)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++)
  {
    acquire(&p->lock);
    if (p->pid == pid)
    {
      p->killed = 1;
      if (p->state == SLEEPING)
      {
        // Wake process from sleep().
        p->state = RUNNABLE;
        enqueue_at_head(p, p->priority);
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if (user_dst)
  {
    return copyout(p->pagetable, dst, src, len);
  }
  else
  {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if (user_src)
  {
    return copyin(p->pagetable, dst, src, len);
  }
  else
  {
    memmove(dst, (char *)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void procdump(void)
{
  static char *states[] = {
      [UNUSED] "unused",
      [SLEEPING] "sleep ",
      [RUNNABLE] "runble",
      [RUNNING] "run   ",
      [ZOMBIE] "zombie"};
  struct proc *p;
  char *state;

  printf("\n");
  for (p = proc; p < &proc[NPROC]; p++)
  {
    if (p->state == UNUSED)
      continue;
    if (p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%d %s %s", p->pid, state, p->name);
    printf("\n");
  }
}

int procinfo(uint64 addr)
{
  // return current process
  struct proc *currProc = myproc();
  struct proc *p;

  printf("\n");
  int count = 0;
  for (p = proc; p < &proc[NPROC]; p++)
  {

    if (p->state != UNUSED)
    {
      count += 1;
      struct pstat thisProc;
      thisProc.pid = p->pid;

      for (int i = 0; i < 16; i++)
      {
        thisProc.name[i] = p->name[i];
      }
      thisProc.state = p->state;
      thisProc.size = p->sz;
      thisProc.cpu_time = p->cputime;
      if (p->parent)
      {
        thisProc.ppid = (p->parent)->pid;
      }
      else
      {
        thisProc.ppid = 0;
      }
      // printf("%d %s\n", thisProc.pid, thisProc.name);

      if (copyout(currProc->pagetable, addr, (char *)&thisProc, sizeof(thisProc)) < 0)
      {
        return -1;
      }
      addr = addr + sizeof(thisProc);
    }
  }
  return count;
}
// Initializes scheduler queues

// Call from main() after call to procinit()

void queueinit(void)
{

  struct queue *q;

  int i = 0;
  for (q = queue; q < &queue[NQUEUE]; q++)
  {
    initlock(&q->lock, "queue");

    if (i == 0)
      q->timeslice = TSTICKSHIGH;

    else if (i == 1)
      q->timeslice = TSTICKSMEDIUM;

    else
      q->timeslice = TSTICKSLOW;

    q->head = 0;
    q->tail = 0;
    i++;
  }
}

int timeslice(int priority)
{
  if (priority == HIGH)
    return (TSTICKSHIGH);
  else if (priority == MEDIUM)
    return (TSTICKSMEDIUM);
  else if (priority == LOW)
  {
    return (TSTICKSLOW);
  }
  else
  {
    printf("timeslice: invalid priority %d\n", priority);
    return (-1);
  }
}

// queue[priority].lock is held on entry

/* Uncomment to use for debugging
static void
queueprint(int priority)
{
  struct proc *p;
  p = queue[priority].head;
  while (p) {
    printf("%d -> ", p->pid);
    p = p->next;
  }
  printf("0\n");
  return;
}
*/

int queue_empty(int priority)
{
  if (!queue[priority].head)
    return (1);

  return (0);
}

// Enqueues process p at the tail of the scheduler queue with priority == priority
// p->lock should be held on entry

static int
enqueue_at_tail(struct proc *p, int priority)
{
  if (!(p >= proc && p < &proc[NPROC]))
  {
    panic("enqueue_at_tail");
  }
  if (!(priority >= 0) && (priority < NQUEUE))
  {
    panic("enqueue_at_tail");
  }
  acquire(&queue[priority].lock);
  if ((queue[priority].head == 0) && (queue[priority].tail == 0))
  {
    queue[priority].head = p;
    queue[priority].tail = p;
    release(&queue[priority].lock);

    return (0);
  }

  if (queue[priority].tail == 0)
  {
    release(&queue[priority].lock);
    panic("enqueue_at_tail");
  }

  queue[priority].tail->next = p;
  queue[priority].tail = p;
  release(&queue[priority].lock);
  return (0);
}

// Enqueues process p at the head of the scheduler queue with priority == priority

// p->lock should be held on entry except for initial enqueue of init

static int
enqueue_at_head(struct proc *p, int priority)
{
  if (!(p >= proc && p < &proc[NPROC]))
  {
    panic("enqueue_at_head");
  }
  if (!(priority >= 0) && (priority < NQUEUE))
  {
    panic("enqueue_at_head");
  }
  // printf("entered enqueue_at_head, pid = %d\n", p->pid);
  acquire(&queue[priority].lock);
  if ((queue[priority].head == 0) && (queue[priority].tail == 0))
  {
    queue[priority].head = p;
    queue[priority].tail = p;
    release(&queue[priority].lock);
    return (0);
  }

  if (queue[priority].head == 0)
  {
    release(&queue[priority].lock);
    panic("enqueue_at_head");
  }

  p->next = queue[priority].head;
  queue[priority].head = p;
  release(&queue[priority].lock);
  return (0);
}

// Dequeues and returns process at head of queue with priority == priority, or
// returns 0 in the case of an empty queue

static struct proc *
dequeue(int priority)
{
  struct proc *p;

  if (!(priority >= 0) && (priority < NQUEUE))
  {
    printf("dequeue: invalid argument %d\n");
    return (0);
  }

  acquire(&queue[priority].lock);
  if ((queue[priority].head == 0) && (queue[priority].tail == 0))
  {
    release(&queue[priority].lock);
    return (0);
  }

  if (queue[priority].head == 0)
  {
    release(&queue[priority].lock);
    panic("dequeue");
  }

  p = queue[priority].head;
  acquire(&p->lock);
  queue[priority].head = p->next;
  p->next = 0;
  release(&p->lock);

  if (!queue[priority].head)
    queue[priority].tail = 0;

  release(&queue[priority].lock);

  return (p);
}

// Initialize mmr_list

void

mmrlistinit(void)

{

struct mmr_list *pmmrlist;

initlock(&listid_lock,"listid");

for (pmmrlist = mmr_list; pmmrlist < &mmr_list[NPROC*MAX_MMR]; pmmrlist++) {

  initlock(&pmmrlist->lock, "mmrlist");

  pmmrlist->valid = 0;

}

}

// find the mmr_list for a given listid

struct mmr_list*

get_mmr_list(int listid) {

acquire(&listid_lock);

if (listid >=0 && listid < NPROC*MAX_MMR && mmr_list[listid].valid) {

  release(&listid_lock);
  return(&mmr_list[listid]);

}

else {

  release(&listid_lock);

  return 0;

}

}

// free up entry in mmr_list array

void

dealloc_mmr_listid(int listid) {

  acquire(&listid_lock);

  mmr_list[listid].valid = 0;

  release(&listid_lock);

}

// find an unused entry in the mmr_list array

int

alloc_mmr_listid() {

acquire(&listid_lock);

int listid = -1;

for (int i = 0; i < NPROC*MAX_MMR; i++) {

  if (mmr_list[i].valid == 0) {

    mmr_list[i].valid = 1;

    listid = i;

    break;

  }

}

release(&listid_lock);

return(listid);

}