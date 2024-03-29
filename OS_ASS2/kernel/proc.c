#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

extern uint64 cas(volatile void *addr, int expected, int newval);
struct proc* remove_head(enum procstate list_type, int cpu_num);

int is_initialize = 0;
struct proc *zombie_list = 0;
struct proc *sleeping_list = 0;
struct proc *unused_list = 0;

struct spinlock ready_list_head_locks[CPUS];
struct spinlock zombie_list_head_lock;
struct spinlock sleeping_list_head_lock;
struct spinlock unused_list_head_lock;

struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct proc *initproc;

int nextpid = 1;
struct spinlock pid_lock;

extern void forkret(void);
static void freeproc(struct proc *p);

extern char trampoline[]; // trampoline.S

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

void
acquire_list(enum procstate list_type, int cpu_num){
  switch (list_type)
  {
  case RUNNABLE:
    acquire(&ready_list_head_locks[cpu_num]);
    break;
  case ZOMBIE:
    acquire(&zombie_list_head_lock);
    break;
  case SLEEPING:
    acquire(&sleeping_list_head_lock);
    break;
  case UNUSED:
    acquire(&unused_list_head_lock);
    break;

  default:
    panic("list type doesn't exist");
  }
}

void
release_list(enum procstate list_type, int cpu_num){
  switch (list_type)
  {
  case RUNNABLE:
    release(&ready_list_head_locks[cpu_num]);
    break;
  case ZOMBIE:
    release(&zombie_list_head_lock);
    break;
  case SLEEPING:
    release(&sleeping_list_head_lock);
    break;
  case UNUSED:
    release(&unused_list_head_lock);
    break;

  default:
    panic("list type doesn't exist");
  }}

struct proc* 
get_head(enum procstate list_type, int cpu_num) {
  struct proc* p;
  switch(list_type){
    case RUNNABLE:
      p = cpus[cpu_num].runnable_list_head;
      break;
    case ZOMBIE:
      p = zombie_list;
      break;
    case SLEEPING:
      p = sleeping_list;
      break;
    case UNUSED:
      p = unused_list;
      break;

    default:
      panic("list type doesn't exist");
  }

  return p;
}

void
set_head(struct proc *new_head, enum procstate list_type, int cpu_num){
  switch (list_type){
  case RUNNABLE:
    cpus[cpu_num].runnable_list_head = new_head;
    break;
  case ZOMBIE:
    zombie_list = new_head;
    break;
  case SLEEPING:
    sleeping_list = new_head;
    break;
  case UNUSED:
    unused_list = new_head;
    break;

  
  default:
    panic("list type doesn't exist");
  }
}

int
remove_proc_from_list(struct proc *p, enum procstate list_type)
{
  acquire_list(list_type, p->cpu_num);
  struct proc *curr = get_head(list_type, p->cpu_num);
  if(!curr){
    release_list(list_type, p->cpu_num);
    return 0;
  }
  struct proc *prev = 0;
  if(p == curr){ //p is head
    acquire(&p->link_lock);
    set_head(curr->next, list_type, p->cpu_num);
    p->next = 0;
    release(&p->link_lock);
    release_list(list_type, p->cpu_num);
    return 1;
  }
  //p is not head
  while(curr){
    acquire(&curr->link_lock);
    if(p == curr){
      // acquire(&prev->link_lock);
      // if(!prev){
      //   release_list(list_type, p->cpu_num); //todo
      // }
      prev->next = curr->next;
      curr->next = 0;
      release(&curr->link_lock);
      release(&prev->link_lock);
      return 1; 
    }
      if(!prev){
        release_list(list_type, p->cpu_num); //todo
      }
      else{
        release(&prev->link_lock);
      }

      prev = curr;
      curr = curr->next;
    
  }
  return 0;
}

int
add_proc_to_list(struct proc *p, enum procstate list_type, int cpu_num)
{
  struct proc *curr = 0;
  acquire_list(list_type, cpu_num);
  curr = get_head(list_type, cpu_num);
  if(!curr){ //empty list
    set_head(p, list_type, cpu_num);
    release_list(list_type, cpu_num);
    return 1;
  }
  struct proc *prev = 0;
  while(curr){
    acquire(&curr->link_lock);
    if(prev){
      release(&prev->link_lock);
    }
    else{
      release_list(list_type, cpu_num);
    }
    prev = curr;
    curr = curr->next;
    // release(&prev->link_lock);
  }
  // acquire(&prev->link_lock);
  prev->next = p;
  release(&prev->link_lock);
  // release_list(list_type, cpu_num);
  return 1;
}

void increase_admitted_process_count(int cpu_num){
  struct cpu* c = &cpus[cpu_num];
  uint64 old;
  do{
    old = c->admitted_process_count;
  } while(cas(&c->admitted_process_count, old, old+1));
}
void
decrease_runnable_list_size_of(int cpu_num){
  struct cpu* c = &cpus[cpu_num];
  uint64 old;
  do{
    old = c->proc_list_size;
  } while(cas(&c->proc_list_size, old, old-1));
}

void
increase_runnable_list_size_of(int cpu_num){
  struct cpu* c = &cpus[cpu_num];
  uint64 old;
  do{
    old = c->proc_list_size;
  } while(cas(&c->proc_list_size, old, old+1));
}

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void
proc_mapstacks(pagetable_t kpgtbl) {
  struct proc *p;
  
  for(p = proc; p < &proc[NPROC]; p++) {
    char *pa = kalloc();
    if(pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int) (p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

// initialize the proc table at boot time.
void
procinit(void)
{
  struct proc *p;
  
  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
  initlock(&zombie_list_head_lock, "zombie_list_head_lock");
  initlock(&sleeping_list_head_lock, "sleeping_list_head_lock");
  initlock(&unused_list_head_lock, "unused_list_head_lock");

  struct spinlock* sl;
  for(sl = ready_list_head_locks; sl <&ready_list_head_locks[CPUS]; sl++){
    initlock(sl, "ready_list_head_locks");
  }

  for(p = proc; p < &proc[NPROC]; p++) {
      initlock(&p->lock, "proc");
      initlock(&p->link_lock, "link_lock");
      p->cpu_num = -1;
      p->next = 0;
      p->kstack = KSTACK((int) (p - proc));
      add_proc_to_list(p, UNUSED, 0);
  }
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int
cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu*
mycpu(void) {
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc*
myproc(void) {
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

int
allocpid() {
  int pid;

  do {
    pid = nextpid;
  } while (cas(&nextpid, pid, pid + 1));

  return pid;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;
  p = remove_head(UNUSED, 0);
  if(!p){
    return 0;
  }
  acquire(&p->lock);
  goto found;
  return 0;

found:
  p->pid = allocpid();
  p->state = USED;
  p->next = 0;

  // Allocate a trapframe page.
  if((p->trapframe = (struct trapframe *)kalloc()) == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if(p->pagetable == 0){
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
  if(p->trapframe)
    kfree((void*)p->trapframe);
  p->trapframe = 0;
  if(p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  remove_proc_from_list(p, ZOMBIE);
  p->state = UNUSED;
  add_proc_to_list(p, UNUSED, 0);

}

// Create a user page table for a given process,
// with no user memory, but with trampoline pages.
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if(pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if(mappages(pagetable, TRAMPOLINE, PGSIZE,
              (uint64)trampoline, PTE_R | PTE_X) < 0){
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe just below TRAMPOLINE, for trampoline.S.
  if(mappages(pagetable, TRAPFRAME, PGSIZE,
              (uint64)(p->trapframe), PTE_R | PTE_W) < 0){
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void
proc_freepagetable(pagetable_t pagetable, uint64 sz)
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
  0x00, 0x00, 0x00, 0x00
};

// Set up first user process.
void
userinit(void)
{
  if(!is_initialize){
    struct cpu* c;
    for(c = cpus; c < &cpus[CPUS]; c++){
      c->runnable_list_head = 0;
      c->proc_list_size = 0;
      c->admitted_process_count = 0;
    }
    is_initialize = 1;
  }

  struct proc *p;

  p = allocproc();
  initproc = p;
  
  // allocate one user page and copy init's instructions
  // and data into it.
  uvminit(p->pagetable, initcode, sizeof(initcode));
  p->sz = PGSIZE;

  // prepare for the very first "return" from kernel to user.
  p->trapframe->epc = 0;      // user program counter
  p->trapframe->sp = PGSIZE;  // user stack pointer

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  p->state = RUNNABLE;
  p->cpu_num = 0;
  add_proc_to_list(p, RUNNABLE, p->cpu_num);
  increase_runnable_list_size_of(p->cpu_num);
  set_head(p, RUNNABLE, p->cpu_num);

  release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *p = myproc();

  sz = p->sz;
  if(n > 0){
    if((sz = uvmalloc(p->pagetable, sz, sz + n)) == 0) {
      return -1;
    }
  } else if(n < 0){
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}

int
find_least_used_cpu()
{
  int least_used_cpu_num = 0;
  for(int i = 1; i<CPUS; i++){
    if(cpus[i].admitted_process_count < cpus[least_used_cpu_num].admitted_process_count){
      least_used_cpu_num = i;
    }
  }
  return least_used_cpu_num;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy user memory from parent to child.
  if(uvmcopy(p->pagetable, np->pagetable, p->sz) < 0){
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&np->lock);
  np->state = RUNNABLE;
  int cpu_num = (BLNCFLG)? find_least_used_cpu(): p->cpu_num;
  np->cpu_num = cpu_num;
  add_proc_to_list(np, RUNNABLE, cpu_num);
  increase_admitted_process_count(cpu_num);
  cpus[cpu_num].admitted_process_count ++;
  increase_runnable_list_size_of(cpu_num);
  release(&np->lock);

  return pid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void
reparent(struct proc *p)
{
  struct proc *pp;

  for(pp = proc; pp < &proc[NPROC]; pp++){
    if(pp->parent == p){
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void
exit(int status)
{
  struct proc *p = myproc();

  if(p == initproc)
    panic("init exiting");

  // Close all open files.
  for(int fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd]){
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
  add_proc_to_list(p, ZOMBIE, 0);
  decrease_runnable_list_size_of(p->cpu_num);

  release(&wait_lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(uint64 addr)
{
  struct proc *np;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(np = proc; np < &proc[NPROC]; np++){
      if(np->parent == p){
        // make sure the child isn't still in exit() or swtch().
        acquire(&np->lock);

        havekids = 1;
        if(np->state == ZOMBIE){
          // Found one.
          pid = np->pid;
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&np->xstate,
                                  sizeof(np->xstate)) < 0) {
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
    if(!havekids || p->killed){
      release(&wait_lock);
      extern uint64 cas(volatile void *addr, int expected, int newval);
      return -1;
    }
    
    // Wait for a child to exit.
    sleep(p, &wait_lock);  //DOC: wait-sleep
  }
}

struct proc* 
remove_head(enum procstate list_type, int cpu_num)
{
  acquire_list(list_type, cpu_num);
  struct proc* head = get_head(list_type, cpu_num);
  if(!head){
    release_list(list_type, cpu_num);
  }
  else{
    acquire(&head->link_lock);
    set_head(head->next, list_type, cpu_num);
    head->next = 0;
    release(&head->link_lock);
    release_list(list_type, cpu_num);
  }
  return head;
  // printf("r_h before ac\n");
  // acquire_list(list_type, cpu_num);
  // printf("r_h after ac\n");
  // struct proc *head = get_head(list_type, cpu_num);
  // printf("r_h before rel\n");
  // release_list(list_type, cpu_num);
  // printf("r_h after rel\n");
  // printf("NEVO NEVO NEVO NEVO NEVO NEVO NEVO NEVO \n");
  // printf("%s\n",head->name);
  // printf("%d\n", is_initialize);
  // remove_proc_from_list(head, list_type);
  // return head;
}

struct proc* steal_process(){
  struct proc *res = 0;
  int my_cpu_num = cpuid();
  for(int i = 0; i<CPUS; i++){
    if(my_cpu_num != i){
      res = remove_head(RUNNABLE,i);
      if(res){
        acquire(&res->link_lock);
        res->cpu_num = my_cpu_num;
        release(&res->link_lock);
        decrease_runnable_list_size_of(i);
        increase_admitted_process_count(my_cpu_num);
        break;
      }
    }
  }
  return res;
}
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
void
scheduler(void)
{
  struct cpu *c = mycpu();
  struct proc *curr; 
  c->proc=0;
  for(;;){
    // Avoid deadlock by ensuring that devices can interrupt.
    intr_on();
    curr = remove_head(RUNNABLE, cpuid());
    if(!curr){
      // curr = steal_process();
      continue;
    }
    acquire(&curr->lock);
    if(curr->state != RUNNABLE){
      // printf("%d\n",curr->state);
      panic("proc is not RUNNABLE");
    }
    curr->state = RUNNING;
    c->proc = curr;
    swtch(&c->context, &curr->context);
    c->proc = 0;
    release(&curr->lock);
  }
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&p->lock))
    panic("sched p->lock");
  if(mycpu()->noff != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = RUNNABLE;
  add_proc_to_list(p, RUNNABLE, p->cpu_num);
  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void
forkret(void)
{
  static int first = 1;

  // Still holding p->lock from scheduler.
  release(&myproc()->lock);

  if (first) {
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
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.

  acquire(&p->lock);  //DOC: sleeplock1
  

  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;
  decrease_runnable_list_size_of(p->cpu_num);
  add_proc_to_list(p, SLEEPING, 0);
  
  release(lk);

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  release(&p->lock);
  acquire(lk);
}

// Wake up all processes sleeping on chan.
// Must be called without any p->lock.
void
wakeup(void *chan)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    if(p != myproc()){
      acquire(&p->lock);
      if(p->state == SLEEPING && p->chan == chan) {
        remove_proc_from_list(p, SLEEPING);
        p->state = RUNNABLE;
        int cpu_num = (BLNCFLG)? find_least_used_cpu(): p->cpu_num; 
        add_proc_to_list(p, RUNNABLE, cpu_num);
        increase_admitted_process_count(cpu_num);
        increase_runnable_list_size_of(cpu_num);
      }
      release(&p->lock);
    }
  }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int
kill(int pid)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid){
      p->killed = 1;
      if(p->state == SLEEPING){
        remove_proc_from_list(p, SLEEPING);
        // Wake process from sleep().
        p->state = RUNNABLE;
        add_proc_to_list(p, RUNNABLE, p->cpu_num);
        increase_runnable_list_size_of(p->cpu_num);
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
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if(user_dst){
    return copyout(p->pagetable, dst, src, len);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if(user_src){
    return copyin(p->pagetable, dst, src, len);
  } else {
    memmove(dst, (char*)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  struct proc *p;
  char *state;

  printf("\n");
  for(p = proc; p < &proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%d %s %s", p->pid, state, p->name);
    printf("\n");
  }
}


int 
range_check(int num, int min, int max)
{
  if(!(num>=min && num<=max)){
    return -1;
  }
  return 0;
}


int 
set_cpu(int cpu_num)
{
  if(range_check(cpu_num,0,NCPU)<0){
    return -1;
  }
  //remove proccess from current cpu runnable list
  decrease_runnable_list_size_of(myproc()->cpu_num);
  //set process's cpu num to new cpu runnable list
  myproc()->cpu_num = cpu_num;
  //increase new cpu runnable list
  increase_runnable_list_size_of(cpu_num);
  //process won’t keep running on the current CPU
  //as it no longer belong to its list
  yield();
  return cpu_num;
}

int 
get_cpu()
{
  return myproc()->cpu_num;
}

int cpu_process_count(int cpu_num){
  return cpus[cpu_num].admitted_process_count;
}
