extern void far_fn(void);
void caller(void) { far_fn(); }
void (*fp)(void);
void indirect(void) { fp(); }
