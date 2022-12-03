#include "kernel/types.h"
#include "user/user.h"
#include "kernel/stat.h"

int
main(int argc, char *argv[]){
  struct stat filename;
  
  stat(argv[1],&filename);

  printf("Name: %s \n", argv[1]);
  if(filename.type == 1){
    printf("%s","Type: Directory \n");
  }else if(filename.type == 2){
    printf("%s","Type: Regular File \n");
  }else if(filename.type == 3){
    printf("%s","Type: Device \n");
  }
  printf("Size: %d \n",filename.size);
  printf("inode Number: %d \n",filename.ino);
  printf("Links: %d \n",filename.nlink);

  exit(0);
};