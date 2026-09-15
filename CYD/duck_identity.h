#pragma once
/*
 * Per-board identity for CYD WiCyS ducks.
 * Change DUCK_SERIAL before flashing the next unit (1, 2, 3, …).
 * This board is Yellow-Duck-1 (serial 1).
 */
#ifndef DUCK_SERIAL
#define DUCK_SERIAL 8
#endif

#define DUCK_SERIAL_MAX 8

#if (DUCK_SERIAL < 1) || (DUCK_SERIAL > DUCK_SERIAL_MAX)
#error DUCK_SERIAL must be 1..8
#endif

#define DUCK_IX (DUCK_SERIAL - 1)
