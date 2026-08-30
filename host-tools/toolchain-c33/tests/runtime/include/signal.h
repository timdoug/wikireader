/* <signal.h> for compile-only tests on the C33 DejaGnu board.  */

#ifndef C33_TEST_SIGNAL_H
#define C33_TEST_SIGNAL_H

typedef int sig_atomic_t;
typedef void (*__c33_sighandler_t) (int);

#define SIG_DFL ((__c33_sighandler_t) 0)
#define SIG_IGN ((__c33_sighandler_t) 1)
#define SIG_ERR ((__c33_sighandler_t) -1)

#define SIGABRT 1
#define SIGFPE  2
#define SIGILL  3
#define SIGINT  4
#define SIGSEGV 5
#define SIGTERM 6

__c33_sighandler_t signal (int, __c33_sighandler_t);
int raise (int);

#endif
