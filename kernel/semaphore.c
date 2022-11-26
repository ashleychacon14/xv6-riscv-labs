#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

struct semtab semtable;

void
seminit(void){
    initlock(&semtable.lock, "semtable");
    for (int i = 0; i < NSEM; i++)
        initlock(&semtable.sem[i].lock, "sem");
}

int //check return type
semalloc(){
    acquire(&semtable.lock);
    for(int i = 0; i < NSEM; i++){
        if (semtable.sem[i].valid == 0){
            semtable.sem[i].valid = 1;
            release(&semtable.lock);
            return i;
        }
    }
    release(&semtable.lock);
    return -1;
}

void   // check return type
semdealloc(int idx){
    // semaphore index as an argument and invalidatesthat entry in the semaphore table
    acquire(&semtable.lock);
    semtable.sem[idx].valid = 0;
    release(&semtable.lock);
}