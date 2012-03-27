#include "tests/lib.h"

/* This program copies srcfile to destfile, then prints the destfile
   By default, these are [arkimedes]theraven.txt and [halibut]tofile.
   Assumes that all operations succeed. */

#define FROMFILESYSTEM "arkimedes"
#define TOFILESYSTEM "halibut"
const char* srcfile="["FROMFILESYSTEM"]theraven.txt";
const char* destfile="["TOFILESYSTEM"]tofile";
#define BUFFERSIZE 2000

#define TEST_VOID(f) (puts(#f"\n"), f)
#define TEST_VALUE(h,f) (puts(#f": "), printf(h"\n", f))

void showfile(const char* file) {
  char buffer[BUFFERSIZE];
  int fd = syscall_open(file);
  int rd, i;
  printf("Got file descriptor %d:\n", fd);
  while ((rd = syscall_read(fd, buffer, BUFFERSIZE))) {
    for (i = 0; i < rd; i++) {
      putc(buffer[i]);
    }
  }
  syscall_close(fd);
  printf("OK, done reading.\n");
}

void writetofile(const char* file, int offset, const char* contents) {
  int fd = syscall_open(file);
  printf("Got file descriptor %d:\n", fd);
  syscall_seek(fd, offset);
  syscall_write(fd, contents, strlen(contents));
  syscall_close(fd);
  printf("OK, done writing.\n");
}

void clearline() {
  int i;
  putc('\r');
  for (i = 0; i < 40; i++) {
    putc(' ');
  }
  putc('\r');
}

void copyfile(const char* to, const char* from) {
  char buffer[BUFFERSIZE];
  int tofd = syscall_open(to);
  int fromfd = syscall_open(from);
  int rd, wrt;
  int totalread = 0, totalwritten = 0;
  printf("Got file descriptors %d and %d:\n", tofd, fromfd);
  while ((rd = syscall_read(fromfd, buffer, BUFFERSIZE))) {
    totalread += rd;
    clearline();
    printf("read %d bytes", totalread);
    wrt = syscall_write(tofd, buffer, rd);
    totalwritten += wrt;
    clearline();
    printf("copied %d bytes", totalwritten);
  }
  printf("\n");
  syscall_close(fromfd);
  syscall_close(tofd);
}

int main() {
  TEST_VALUE("%d", syscall_create(destfile, 3000));
  TEST_VOID(copyfile(destfile, srcfile));
  TEST_VOID(showfile(destfile));
  TEST_VALUE("%d", syscall_delete(destfile));
  printf("Test done.\n");
  return 0;
}
