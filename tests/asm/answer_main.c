/* Test harness for tests/asm/answer.s (compiled by the system C compiler;
 * only answer.o comes from the Luma assembler). */
#include <stdio.h>
#include <string.h>

int answer(void);
const char *answer_msg(void);

int main(void) {
    int a = answer();
    const char *m = answer_msg();
    printf("answer=%d msg=%s\n", a, m);
    return (a == 42 && strcmp(m, "from lasm") == 0) ? 0 : 1;
}
