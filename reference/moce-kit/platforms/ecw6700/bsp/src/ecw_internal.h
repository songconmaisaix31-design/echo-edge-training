#ifndef ECW_INTERNAL_H
#define ECW_INTERNAL_H
#include "bsp_ecw6700.h"
BspStatus ecw_unsupported(BspPort id, BspKind kind);
BspStatus ecw_reject_request(BspPort id, BspKind kind, BspRequest *request);
#include "ecw_serial_poll.h"
BspStatus ecw_claim_basic(BspPort, BspKind, const Ecw6700Port **);
BspStatus ecw_finish_basic(BspPort, EcwIoResult);
BspStatus ecw_uart_inspect(BspPort, const Ecw6700Port **, bool *ready);
BspStatus ecw_claim_spi_plan(const BspDisplayBinding *, bool, const Ecw6700Port **);
void ecw_unclaim_spi_plan(const Ecw6700Port **);
void ecw_record_async(BspPort id, EcwIoResult result);
#endif
