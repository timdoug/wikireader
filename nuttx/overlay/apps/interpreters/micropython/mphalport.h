/* The four calls MicroPython makes on the machine underneath it. */

#include <errno.h>
#include <unistd.h>

#define mp_hal_stdio_poll(poll_flags) (0)

/* PEP 475: a system call interrupted by a signal is retried rather than
 * reported.  There is no global interpreter lock here to drop first, so this
 * is the upstream macro with that part left out.
 */

#define MP_HAL_RETRY_SYSCALL(ret, syscall, raise) \
  { \
    for (;;) \
      { \
        ret = syscall; \
        if (ret == -1) \
          { \
            int err = errno; \
            if (err == EINTR) \
              { \
                mp_handle_pending(MP_HANDLE_PENDING_CALLBACKS_AND_EXCEPTIONS); \
                continue; \
              } \
            raise; \
          } \
        break; \
      } \
  }

static inline void mp_hal_set_interrupt_char(char c) { (void)c; }

void mp_hal_delay_ms(mp_uint_t ms);
void mp_hal_delay_us(mp_uint_t us);
mp_uint_t mp_hal_ticks_ms(void);
mp_uint_t mp_hal_ticks_us(void);
mp_uint_t mp_hal_ticks_cpu(void);
uint64_t mp_hal_time_ns(void);
