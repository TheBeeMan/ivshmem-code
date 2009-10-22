#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <semaphore.h>
#include "ftp.h"
#include "ivshmem.h"

int main(int argc, char ** argv){

    int ivfd, ffd;
    int receiver;
    void * memptr;
    char * copyto;
    int idx, sent, total;
    struct stat st;
    int dbg;

    sem_t *full, *empty;

    if (argc != 4){
        printf("USAGE: ftp_send <ivshmem_device> <file> <receiver>\n");
        exit(-1);
    }

    if(stat(argv[2], &st) != 0) {
        printf("file does not exist\n");
        exit(-1);
    }
    total = st.st_size;

    if((ffd = open(argv[2], O_RDONLY)) == -1) {
        printf("could not open file\n");
        exit(-1);
    }

    ivfd = open(argv[1], O_RDWR);

    receiver = atoi(argv[3]);

    if ((memptr = mmap(NULL, 16 * CHUNK_SZ, PROT_READ|PROT_WRITE, MAP_SHARED, ivfd, 0)) == MAP_FAILED) {
        printf("mmap failed (0x%p)\n", memptr);
        close(ivfd);
        close(ffd);
        exit(-1);
    }

    copyto = (char *)BUF_LOC;

    /* Initialize the semaphores */
    full = (sem_t *)FULL_LOC;
    empty = (sem_t *)EMPTY_LOC;
    if(sem_init(full, 1, 0) != 0) {
        printf("couldn't initialize full semaphore\n");
        exit(-1);
    }
    if(sem_init(empty, 1, 15) != 0) {
        printf("couldn't initialize empty semaphore\n");
        exit(-1);
    }
    msync(memptr, CHUNK_SZ, MS_SYNC);

    /* Send the file size */
    printf("[SEND] sending size %d to receiver %d\n", total, receiver);
    memcpy((void*)copyto, (void*)&total, sizeof(int));
    msync(copyto, sizeof(int), MS_SYNC);
    ivshmem_send(ivfd, WAIT_EVENT_IRQ, receiver);
    /* Wait to know the reciever got the size */
    printf("[SEND] waiting for receiver to ack size\n");
    ivshmem_send(ivfd, WAIT_EVENT, receiver);
    printf("[SEND] ack!\n");

    for(idx = sent = 0; sent < total; idx = NEXT(idx)) {
        printf("[SEND] waiting for available block\n");
        sem_getvalue(empty, &dbg);
        printf("[SEND] empty is %d\n", dbg);
        if(dbg > 15) {
            printf("[SEND] empty is over 15! wtf!\n");
            exit(-1);
        }
        sem_wait(empty);
        msync(empty, sizeof(sem_t), MS_SYNC);
        printf("[SEND] sending bytes in block %d\n", idx);
        read(ffd, copyto + OFFSET(idx), CHUNK_SZ);
        msync(copyto + OFFSET(idx), CHUNK_SZ, MS_SYNC);
        sent += CHUNK_SZ;
        printf("[SEND] notifying, sent size now %d\n", sent);
        sem_post(full);
        msync(full, sizeof(sem_t), MS_SYNC);
    }

    munmap(memptr, 16*CHUNK_SZ);
    close(ffd);
    close(ivfd);
}
