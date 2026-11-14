#ifndef BSP_ECW6700_STORAGE_H
#define BSP_ECW6700_STORAGE_H
#include "bsp_gpio_event_storage.h"
typedef struct
{
    BspStatus status;
    int64_t native_status;
    bool fault;
} Ecw6700IoResult;
typedef struct
{
    const Ecw6700Port *port;
    bool enabled;
    BspRequest *request;
    uint64_t generation;
    Ecw6700IoResult outcome;
} Ecw6700I2CStorage;
typedef struct
{
    const Ecw6700Port *port;
    uint8_t *tx_storage, *rx_storage, *receive;
    size_t capacity, count;
    BspRequest *request;
    uint64_t generation;
    Ecw6700IoResult outcome;
    bool fault_reported;
} Ecw6700SPIStorage;
/* Optional project-owned UART background RX state and ring.
 * Set bytes/capacity before platform init; zero all runtime members. */
typedef struct
{
    uint8_t *bytes;
    size_t capacity;
    const Ecw6700Port *port;
    unsigned head, tail, queued;
    uint32_t dropped, errors, native_error;
    bool active;
} Ecw6700UARTRxStorage;

typedef struct
{
    const Ecw6700Port *port;
    Ecw6700UARTRxStorage *background;
    uint8_t *storage, *receive;
    size_t capacity, count;
    BspRequest *request;
    uint64_t generation;
    Ecw6700IoResult outcome;
    bool fault_reported;
} Ecw6700UARTStorage;
typedef struct
{
    BspPort port;
    BspAudioState state;
    uint16_t *tx, *rx;
    size_t n;
    BspAudioRefill notify;
    void *user;
    bool duplex, stopping, in_callback;
} Ecw6700I2SStorage;
/* Project-owned storage. Configure id and optional GPIO event queue before init.
 * All storage must remain alive while platform/IRQs can access it. */
typedef struct Ecw6700RuntimePort
{
    BspPort id;
    BspGpioEventQueue *gpio_events;
    union
    {
        Ecw6700I2CStorage *i2c;
        Ecw6700SPIStorage *spi;
        Ecw6700UARTStorage *uart;
        Ecw6700I2SStorage *i2s;
        void *memory;
    } protocol;
    /* Private runtime metadata: do not read/write after platform init. */
    const Ecw6700Port *port;
    const BspPortInfo *info;
    size_t index;
    bool held, ready, fault, busy;
    BspError error;
} Ecw6700RuntimePort;
#endif
