/* Freestanding setjmp for the kernel sets: GCC's builtins, which need no
 * library. Only ftgrays uses it, to unwind from a full cell pool. */
#ifndef AMT_KSET_SETJMP_H
#define AMT_KSET_SETJMP_H
typedef void *jmp_buf[5];
#define setjmp(b) __builtin_setjmp(b)
#define longjmp(b, v) __builtin_longjmp((b), 1)
#endif
