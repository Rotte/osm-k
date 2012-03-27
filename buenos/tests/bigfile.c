#include "tests/lib.h"

/* This program will create a huge file and copy theraven.txt multiple times
 * to the destination system. */

#define FROMFILESYSTEM "arkimedes"
#define TOFILESYSTEM "halibut"
const char* srcfile="["FROMFILESYSTEM"]theraven.txt";
const char* destfile="["TOFILESYSTEM"]tofile";
#define BUFFERSIZE 2000
#define BIGFILESIZE 270000

char buffer[BUFFERSIZE];

int copyfile(const char* to, const char* from, int offset) {
    printf("tofd %p\n", to);
  int tofd = syscall_open(to);
  printf("fromfd %p\n", from);
  int fromfd = syscall_open(from);
  int rd, wrt;
  int totalread = 0, totalwritten = 0;
  printf("Got file descriptors %d and %d:\n", tofd, fromfd);
  syscall_seek(tofd, offset);
  while ((rd = syscall_read(fromfd, buffer, BUFFERSIZE))) {
    totalread += rd;
    printf("read %d bytes", totalread);
    wrt = syscall_write(tofd, buffer, rd);
    totalwritten += wrt;
    printf("copied %d bytes\n", totalwritten);
  }
  printf("\n");
  syscall_close(fromfd);
  syscall_close(tofd);

  return totalwritten;
}

int main() {
//    printf("Creating: %d\n", syscall_create(destfile, 0));
    printf("Creating: %d\n", syscall_create(destfile, BIGFILESIZE));
    int offset = 0;
    while (offset < BIGFILESIZE)
        offset += copyfile(destfile, srcfile, offset);
    int a = 0;

    int fd = syscall_open(destfile);
    while (a < BIGFILESIZE)
    {
        a += syscall_read(fd, buffer, BUFFERSIZE);
        printf("%c %c %c\n", buffer[0], buffer[100], buffer[1000]);
    }
    syscall_close(fd);

    printf("Test done.\n");
    return 0;
}
