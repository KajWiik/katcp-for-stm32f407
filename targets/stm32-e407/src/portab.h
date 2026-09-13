#ifndef PORTAB_H
#define PORTAB_H

/* LINE_LED is defined identically by both OLIMEX_STM32_E407 board
 * variants, so this doesn't depend on which PHY/board.mk is in use. */
#define PORTAB_LINE_LED1            LINE_LED

#ifdef __cplusplus
extern "C" {
#endif
  void portab_setup(void);
#ifdef __cplusplus
}
#endif

#endif /* PORTAB_H */
