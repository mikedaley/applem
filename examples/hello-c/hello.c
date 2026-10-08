/*
 * hello.c - A first C program: print a line, wait for a key
 *
 * cl65 links it with the Apple II runtime into an AppleSingle file, which
 * ApplEm starts from a ProDOS disk it makes for it.
 */

#include <stdio.h>
#include <conio.h>

int main(void)
{
    clrscr();
    printf("Hello from cc65!\n\n");
    printf("Press a key to quit.\n");
    cgetc();
    return 0;
}
