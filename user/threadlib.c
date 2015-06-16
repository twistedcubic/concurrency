#include "types.h"
#include "user.h"
#include "stat.h"
#include "x86.h"

struct node *root;

int thread_create(void (*start_routine)(void*), void *arg)
{
  root = (struct node*)malloc(sizeof(struct node));
  root->pid = 0;
  root->mem = NULL;
  root->next = NULL;

  struct node *temp;
  for(temp = root; temp->next != NULL; temp = temp->next);
  if(temp != root)
    temp->next = (struct node*)malloc(sizeof(struct node));
  else if(temp->mem == NULL) //first node
    temp->next = root;

  //allocate stack
  void *stack = malloc(8192); //4096 is page size, malloc returns beginning of allocated block. Don't forget to free!

  (temp->next)->mem = (void*)stack;
  //make page-aligned
  if((uint)stack%4096 )
    //stack = ((char*)stack + 4096) - ((char*)stack + 4096)%4096;
    stack = stack + (4096 - (uint)stack % 4096);
  //  (((sz)+PGSIZE-1) & ~(PGSIZE-1))
  
  //invoke clone
  int ret;  
  ret = clone(start_routine, arg, stack);
  if(ret < 0 || ret == 0)
    printf(1, "return of clone: %d\n", ret);
  (temp->next)->pid = ret;
  (temp->next)->next = NULL;
  //use exit() at the end of start routine!
  return ret;
}

//calls the join system call
int thread_join(int pid)
{
  int ret;
  ret = join(pid);
  if (ret==-1)
    return -1;
  //free memory
  struct node *prev; //previous node preceding the one with pid
  struct node *temp;// = (struct node*)malloc(sizeof(node));
  for(temp = root; temp->next != NULL; temp = temp->next)
    {
      if(temp->pid == ret)
	{
	  prev->next =temp->next;	  
	  free(temp->mem);
	  free(temp);
	  break;
	}
      prev=temp;
    }
  return ret;
}

//ticket lock
void lock_init(lock_t *tl)
{
  tl->ticket = 0; //next available number to hand out
  tl->turn = 0; //whose turn is it?
}

void lock_acquire(lock_t *tl)
{
  int myturn = fetch_and_add((uint*)&(tl->ticket), 1); 
  while(myturn != tl->turn)
    ;
  //fetch_and_add is atomic
}

void lock_release(lock_t *tl)
{
  fetch_and_add((uint*)&(tl->turn), 1);
}

//condition variables
//cond_t structure     list of waiters
//linked list
//list points to first node of linked list
void cv_wait(cond_t *list, lock_t *tl)
{
  cond_t *temp;// *prev;
  //traverse through the linked list/queue to find end of queue        
  for(temp = list; temp->next!=NULL; temp=temp->next)
    ;
    //prev = temp;
  if(temp == list) //first in queue     
    {
      list = (cond_t*)malloc(sizeof(cond_t)); //queue to waiting list 
      list->pid = getpid();
      list->next = NULL;
    }else //list was not empty 
    {  temp->next = (cond_t*)malloc(sizeof(cond_t)); //queue to waiting list 
      (temp->next)->pid = getpid();
      //temp->next = temp;
      (temp->next)->next = NULL;
    }

  cv_waitK(list, tl);//system call to kernel to sleep, release locks
  ////////////////////////

  //queue to the waiting list
  ///
  //release lock
  //put thread to sleep
  ///
  //acquire lock??
}

void cv_signal(cond_t *list)
{
  //choose thread from waiting list
  //wake up that thread
  cv_signalK(list);

}
