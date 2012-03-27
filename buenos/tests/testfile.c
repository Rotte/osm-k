#include "tests/lib.h"

static const char file1[] = "[disk1]ffile";

void fail(const char *msg)
{
    printf(msg);
    syscall_exit(0);
}

int main()
{
    char buff[20];
    syscall_delete(file1);
    if (syscall_create(file1, 100) < 0)
        fail("Couldn't create file :(\n");
    int fd = syscall_open(file1);
    if (fd < 0)
        fail("Couldn't open file :(\n");
    printf("Wrote %d bytes\n", syscall_write(fd, "Teststring!\n", 11));

    if (syscall_seek(fd, 0) < 0)
        fail("Couldn't seek :(\n");

    printf("Read %d bytes\n", syscall_read(fd, buff, 20));
    printf(buff);

    syscall_halt();
    return 0;
}
