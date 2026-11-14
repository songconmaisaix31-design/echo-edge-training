/* AI library source: ecw6700, schematic-20261009-2adc.
 * mainboard/ owns fixed resources and onboard standard interfaces.
 * This header composes the build-selected mainboard and optional passive sheild.
 * Use one Board include directory and explicit selection from board_selection.h
 * consistently in every translation unit. Matching BSP/SDK headers are required.
 * Only actual connectors of the selected assembly are application resources;
 * unassigned interconnect pins are not standalone user ports.
 * Getters do not initialize hardware or prove operation/hardware validation.
 */
#ifndef MOCE_SELECTED_BOARD_H
#define MOCE_SELECTED_BOARD_H
/* Select exactly one physical Board directory in the build include path. */
#include "board_ports.h"
#include "mainboard/board_radio.h"
#include "mainboard/board_timer.h"
#include "board_platform.h"
#endif
