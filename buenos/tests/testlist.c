#include "tests/lib.h"

#define FILESIZE 16

const char *dir = "[halibut]";

int main()
{
    int i;
    int n = syscall_filecount(dir);
    char buffer[FILESIZE];

    printf("amount of files in %s: %d\n", dir, n);

    for (i = 0; i < n; ++i)
    {
        syscall_file(dir, i, buffer);
        printf("%s\n", buffer);
    }
    syscall_halt();

    return 0;
}
