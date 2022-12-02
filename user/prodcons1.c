#include "kernel/types.h"
#include "user/user.h"
#include "kernel/stat.h"

#define BSIZE 10
#define MAX 10

typedef struct {
    int buf[BSIZE];
    int nextin;
    int nextout;
    int num_produced;
    int num_consumed;
    int total;
} buffer_t;

buffer_t *buffer;

void *producer()
{
    while(1) {
        if (buffer->num_produced >= MAX) {
	    exit(0);
	}
	buffer->num_produced++;
	buffer->buf[buffer->nextin++] = buffer->num_produced;
	buffer->nextin %= BSIZE;
    }
}

void *consumer()
{
    while(1) {
        if (buffer->num_consumed >= MAX) {
	    exit(0);
	}
	buffer->total += buffer->buf[buffer->nextout++];
	buffer->nextout %= BSIZE;
	buffer->num_consumed++;
    }
}

int main(int argc, char *argv[])
{
    buffer = (buffer_t *) mmap(NULL, sizeof(buffer_t),
		               PROT_READ | PROT_WRITE,
			       MAP_ANONYMOUS | MAP_SHARED, -1, 0);
    buffer->nextin = 0;
    buffer->nextout = 0;
    buffer->num_produced = 0;
    buffer->num_consumed = 0;
    buffer->total = 0;
    if (!fork())
        producer();
    else {
	wait(0);
    }
    if (!fork())
        consumer();
    else {
	wait(0);
    }
    printf("total = %d\n", buffer->total);
    exit(0);
}