#ifndef MOCE_BOARD_SELECTION_H
#define MOCE_BOARD_SELECTION_H
/* No separately verified sheild exists for this mainboard revision yet. */
#if !defined(MOCE_SHEILD_NONE) || defined(MOCE_SHEILD_EXPANSION_20260922)
#error "This Board requires explicit MOCE_SHEILD_NONE=1; no compatible sheild is declared"
#endif
#endif
