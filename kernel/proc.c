#include "types.h"
#include "defs.h"
#include "param.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"
#include "elf.h"
#include "stat.h"

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

struct spinlock glock; //used for growproc
void ginit(void)
{
  initlock(&glock, "glock");
}

static struct proc *initproc;

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
}

//condition variable system call
//adds caller to queue
void cv_waitK(cond_t *list, lock_t *tl)
{//cond_t list not used!
  acquire(&ptable.lock);

  //release lock
  fetch_and_add((uint*)(&(tl->turn)), 1);

  //put to sleep
  //  proc->state = SLEEPING;
  //release(&ptable.lock);
  cprintf("current proc:%s\n", proc->name);
  procdump();
  sleep(proc, &ptable.lock);
  //sched();
}

//wake up a thread from queue
void cv_signalK(cond_t *list)
{
  //wake up first in list
  cond_t *temp = list;
  list = list->next;
  //get proc corresponding to temp's pid
  acquire(&ptable.lock);
  struct proc *p;
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    {
      if(p->pid == temp->pid)
	break;
    }
  p->state = RUNNABLE;
  release(&ptable.lock);
  //sched();
}

// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;
  char *sp;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;
  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;
  release(&ptable.lock);

  // Allocate kernel stack if possible.
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;
  
  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe*)sp;
  
  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;

  return p;
}

// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];
  
  p = allocproc();
  acquire(&ptable.lock);
  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0;  // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  p->state = RUNNABLE;
  release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  //update size of heap for all threads
  acquire(&glock);
  if(proc->ispt == 1)
    sz = (proc->parent)->sz;
  else
    sz = proc->sz;
  if(n > 0){
    if((sz = allocuvm(proc->pgdir, sz, sz + n)) == 0)
      {release(&glock); return -1;}
  } else if(n < 0){
    if((sz = deallocuvm(proc->pgdir, sz, sz + n)) == 0)
      {release(&glock); 
	return -1;}
  }
  ///////////////////////////////////debug
  cprintf("in growproc: name:%s, sz:%d, size request:%d \n", proc->name, sz, n);
  if(proc->ispt == 1) //defer to parent's sz
    (proc->parent)->sz = sz;
  else
    proc->sz = sz;

  switchuvm(proc);
  release(&glock);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void)
{
  int i, pid;
  struct proc *np;

  // Allocate process.
  if((np = allocproc()) == 0)
    return -1;

  // Copy process state from p.
  if((np->pgdir = copyuvm(proc->pgdir, proc->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  np->sz = proc->sz;
  np->parent = proc;
  *np->tf = *proc->tf;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(proc->ofile[i])
      np->ofile[i] = filedup(proc->ofile[i]);
  np->cwd = idup(proc->cwd);
 
  pid = np->pid;
  np->state = RUNNABLE;
  safestrcpy(np->name, proc->name, sizeof(proc->name));
  return pid;
}

//clone creates a new thread and executes from *fcn
//uses stack in the heap that's been passed
int clone(void(*fcn)(void*), void *arg, void *stack)
{
  //uint sp; //sp for stack pointer
  int i, pid;
  struct proc *np;
  pde_t *pgdir;
  if((uint)stack%PGSIZE != 0) //make sure it's page-aligned
    return -1;
  if((uint)(stack + PGSIZE) > proc->sz)  //didn't give me a whole page
    return -1;
  if ((np = allocproc())==0)  //allocate process, kalloc's for kstack
    return -1;
  //the process state of child is same as that of parent proc
  //such as pgdir
  //  np->pgdir = proc->pgdir;
  //  pgdir = proc->pgdir;
  //np->sz = proc->sz;
  
  struct proc *temp;
  for(temp = proc; temp->ispt == 1; temp = temp->parent)
    ;
  np->parent = temp; //should be an ancestral process!
  np->pgdir = temp->pgdir;
  pgdir = temp->pgdir;

  *np->tf = *proc->tf;
  np->ispt = 1; //set ispt to thread
  np->tf->eax = 0; //clear eax so clone returns 0 in child (??)

  for(i = 0; i < NOFILE; i++) //copy file des
    if(proc->ofile[i])
      np->ofile[i] = filedup(proc->ofile[i]);
  np->cwd = idup(proc->cwd);

  pid = np->pid;
  np->state = RUNNABLE;
  safestrcpy(np->name, proc->name, sizeof(proc->name)); //same name?
  //specify np's userstack as stack
  //use exec
  stack = ((char*)stack + 4096); //stack grows backwards
  //arg = (int*)arg;

  stack = stack - sizeof(void*);
  *(uint*)stack = (uint)arg;
  stack = stack - sizeof(void*);
  *(uint*)stack = 0xffffffff;
  
  //void *ret, *ar;
  // ret = sp - 2*sizeof(void*);
  //*(uint *)ret =  0xffffffff;
  //ar = sp - sizeof(void*);
  //*(uint *)ar = (uint)arg;

  //  memmove((void*)np->tf->esp, sp - PGSIZE, PGSIZE);
  //////////////////////
  np->tf->eip = (uint)fcn; //set instruction pointer to fcn
  np->tf->esp = (uint)stack;
  //  np->tf->esp = sp - 2*sizeof(void*);
  //switchuvm(np); //switch to child
  return pid; //parent
}

int join(int pid)
{
  struct proc *p, *parent;
  int havekids, pid2;
  acquire(&ptable.lock); 
  for(;;) //scan through ptable looking for zombie children
      {    havekids=0;	   
	if(proc->ispt == 1)
	  parent = proc->parent;
	else
	  parent = proc;
      for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
	{
	  if((p->pid == pid && p->ispt == 0))//can't join on parent process/wait on thread for another process
	    {release(&ptable.lock);
	    return -1;}
	  if((pid == -1 && p->parent == parent) || p->pid == pid) 
	    { havekids = 1;
      //      cprintf("parent a thread or process?: %d\n", (p->parent)->ispt);
	      if (p->killed == 1)
		{p->state == ZOMBIE;
		  pid2 = p->pid;
		  release(&ptable.lock);
		  return pid2;
		  } 
	      if(p->state == ZOMBIE) //child finished executing
		{  //found a fitting child thread
		  pid2 = p->pid;
		  kfree(p->kstack);
		  p->kstack = 0;
		  p->state = UNUSED;
		  p->pid = 0;
		  p->parent = 0;
		  p->name[0] = 0;
		  p->killed = 0;
		  release(&ptable.lock);
		  return pid2;
		}
	    }
	}
      if(!havekids || proc->killed) //don't have the thread we looked for ///////// p->killed??
	{
	  release(&ptable.lock);
	  return -1;
	}
      sleep(proc, &ptable.lock); //wait for children to exit
      }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  struct proc *p;
  int fd;

  if(proc == initproc)
    panic("init exiting");
  //process exits, kill child threads and 
  //wait (join) for them to exit.  
 
  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(proc->ofile[fd]){
      fileclose(proc->ofile[fd]);
      proc->ofile[fd] = 0;
    }
  }
  iput(proc->cwd);
  proc->cwd = 0;

  acquire(&ptable.lock);
  // Parent might be sleeping in wait().

  if(proc->ispt == 0)
    {
      for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
	{if(p->parent == proc && (p->parent)->state != ZOMBIE) //clean up directly if zombie
	    { //kill all children threads but not processes
	      if(p->ispt == 1)  //ispt == 1 means thread
		{p->killed = 1;
		  if(p->state == SLEEPING)
		    p->state = RUNNABLE;		
		  wakeup1(p->parent);	    
		  release(&ptable.lock);
		  join(p->pid);
		  acquire(&ptable.lock);
		  //  	      p->state = UNUSED; ////////
		}  else //p is a process, pass child processes to init
		{if(p->state == SLEEPING)
		    p->state = RUNNABLE;
		  p->parent = initproc;
		  if(p->state == ZOMBIE) //wake up init if done
		    wakeup1(initproc);
		}
	    }
	  else if(p->parent == proc && (p->parent)->state == ZOMBIE) //clean up directly
	    {     if(p->ispt == 1)  //ispt == 1 means thread
		{
		  kfree(p->kstack);
		  p->kstack = 0;
		  p->state = UNUSED;
		  p->pid = 0;
		  p->parent = 0;
		  p->name[0] = 0;
		  p->killed = 0;
		}  else //p is a process, pass child processes to init
		{if(p->state == SLEEPING)
		    p->state = RUNNABLE;
		  p->parent = initproc;
		  if(p->state == ZOMBIE) //wake up init if done
		    wakeup1(initproc);
		}
	    }
	}
      wakeup1(proc->parent); //wake up  
      // Jump into the scheduler, never to return.
      proc->state = ZOMBIE;
    } else //proc is a thread, exits, other threads untouched
    {//wakeup1(p->parent);
      proc->killed=1;
      if(proc->state == SLEEPING)
	proc->state = RUNNABLE;
      //      proc->state == ZOMBIE;
      wakeup1(proc->parent);
      release(&ptable.lock);
      join(proc->pid);
      acquire(&ptable.lock);
      proc->state = UNUSED; ///////////UNUSED?
    }
  //  cprintf("%s\n", proc->state);
  cprintf("In exit:\n");
  procdump();
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  struct proc *p;
  int havekids, pid;
  if (proc->ispt == 1)
    return -1;
  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for zombie children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != proc || p->ispt == 1) //continue loop if a thread
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->state = UNUSED;
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || proc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(proc, &ptable.lock);  //DOC: wait-sleep
  }
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;

  for(;;){
    // Enable interrupts on this processor.
    sti();

    // Loop over process table looking for process to run.
    acquire(&ptable.lock);
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->state != RUNNABLE)
        continue;

      // Switch to chosen process.  It is the process's job
      // to release ptable.lock and then reacquire it
      // before jumping back to us.
      proc = p;
      switchuvm(p);
      p->state = RUNNING;
      swtch(&cpu->scheduler, proc->context);
      switchkvm();

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      proc = 0;
    }
    release(&ptable.lock);

  }
}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state.
void
sched(void)
{
  int intena;

  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(cpu->ncli != 1)
    panic("sched locks");
  if(proc->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = cpu->intena;
  swtch(&proc->context, cpu->scheduler);
  cpu->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  acquire(&ptable.lock);  //DOC: yieldlock
  proc->state = RUNNABLE;
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);
  
  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  if(proc == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: sleeplock1
    release(lk);
  }

  // Go to sleep.
  proc->chan = chan;
  proc->state = SLEEPING;
  sched();

  // Tidy up.
  proc->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == SLEEPING && p->chan == chan)
      p->state = RUNNABLE;
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
  struct proc *p;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING)
        p->state = RUNNABLE;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [EMBRYO]    "embryo",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  int i;
  struct proc *p;
  char *state;
  uint pc[10];
  
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %d %s %s %d", p->pid, p->ispt, state, p->name, p->killed);
    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}


