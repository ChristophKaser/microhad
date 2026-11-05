#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>
extern int unsafe_test(unsigned *);
extern int safe_test(unsigned *);
extern int crash_test(unsigned *);
__asm__(
".text\n.set noreorder\n"
".globl unsafe_test\nunsafe_test:\n"
"addiu $sp,$sp,-24\nsw $ra,20($sp)\naddiu $a1,$zero,1\n"
"jal delay_target\nlw $a1,0($a0)\n"
"lw $ra,20($sp)\nnop\njr $ra\naddiu $sp,$sp,24\n"
".globl safe_test\nsafe_test:\n"
"addiu $sp,$sp,-24\nsw $ra,20($sp)\naddiu $a1,$zero,1\n"
"lw $a1,0($a0)\njal delay_target\nnop\n"
"lw $ra,20($sp)\nnop\njr $ra\naddiu $sp,$sp,24\n"
".globl crash_test\ncrash_test:\naddiu $sp,$sp,-24\nsw $ra,20($sp)\naddiu $a1,$zero,1\njal crash_target\nlw $a1,0($a0)\nlw $ra,20($sp)\nnop\njr $ra\naddiu $sp,$sp,24\n"
"crash_target:\nbeqz $a1,zero_path\nnop\nlhu $v1,18($a1)\njr $ra\nnop\n"
"delay_target:\nbeqz $a1,zero_path\nnop\naddiu $v0,$zero,1\njr $ra\nnop\n"
"zero_path:\naddiu $v0,$zero,0\njr $ra\nnop\n.set reorder\n");
/* The dangerous read runs only in a child, with no production snapshot handler. */
static void caught(int sig)
{
    _exit(sig == SIGSEGV ? 77 : 78);
}

int main(void)
{
    unsigned zero = 0;
    int unsafe = unsafe_test(&zero);
    int safe = safe_test(&zero);
    int status;
    pid_t child;
    printf("null-pointer branch: call-slot-load=%d separated-load=%d (correct result=0)\n", unsafe, safe);
    fflush(stdout);
    child = fork();
    if (child < 0) return 2;
    if (child == 0) {
        signal(SIGSEGV, caught);
        signal(SIGBUS, caught);
        crash_test(&zero);
        _exit(0);
    }
    if (waitpid(child, &status, 0) != child) return 2;
    printf("isolated exact-read child status=%d (77=SIGSEGV caught)\n",
           WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    return unsafe != 1 || safe != 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 77;
}
