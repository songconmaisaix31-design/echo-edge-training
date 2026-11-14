#ifndef MOCE_SD_CARD_H
#define MOCE_SD_CARD_H

#include "bsp.h"

#define SD_CARD_SECTOR_SIZE 512u

/* SDSC/SDHC/SDXC SPI-mode driver. No MMC, SDUC or automatic hotplug.
 * The caller first initializes a verified Board SPI (8-bit, mode 0, MSB first,
 * 100..400 kHz), active-low output CS and a running BSP timebase.
 * One main/thread owner exclusively uses SPI and CS for the whole operation.
 * Never mix this binding with another device/async transaction on that bus.
 * All transfers are blocking public BSP calls; all buffers return on return,
 * including errors (partially received data must be discarded).
 * No automatic BSP recovery, low-level fallback, or transfer retry.
 * Start with a zero-initialized SdCard. Fields are observation only after calls.
 */
typedef struct {
    BspPort spi, cs;
    uint32_t transfer_timeout_ms; /* Per BSP call, not an end-to-end deadline. */
    uint32_t ready_timeout_ms;
    uint32_t init_timeout_ms;
    bool allow_write; /* Explicit opt-in; false rejects writes before bus effects. */
    bool multiblock; /* Opt-in CMD18/CMD25; false retains single-sector path. */
} SdCardBinding;

typedef struct {
    const char *stage;
    BspStatus status, cleanup_status;
    BspPort missing_port;
    uint32_t missing_caps;
    uint8_t command, r1, token, r2;
    bool crc_mismatch;
} SdCardDiagnostic;

typedef struct {
    SdCardBinding binding;
    SdCardDiagnostic diagnostic;
    uint64_t sector_count;
    uint32_t sectors_read, sectors_written;
    uint8_t csd[16], cid[16], ocr[4];
    bool block_addressed, ready, write_protected;
} SdCard;

/* A successful init includes OCR/CSD checks; errors leave ready=false.
 * Explicit reinit is allowed only after any BSP fault is resolved by its owner.
 * Invalid binding and missing caps are rejected before device commands. */
typedef enum { SD_CARD_OFFLINE, SD_CARD_READY, SD_CARD_FAULT } SdCardState;
SdCardState sd_card_get_state(const SdCard *card);

BspStatus sd_card_init(SdCard *card, const SdCardBinding *binding);
BspStatus sd_card_read_sector(SdCard *card, uint32_t lba,
                              uint8_t data[SD_CARD_SECTOR_SIZE]);

/* CMD24, CRC16, data-response, bounded busy wait and CMD13 status.
 * On any transport/write error, ready is cleared. The attempted sector may
 * already have changed: never blindly retry a failed write. No atomicity claim.
 * This API must not be used while a filesystem is mounted over the card. */
BspStatus sd_card_write_sector(SdCard *card, uint32_t lba,
                               const uint8_t data[SD_CARD_SECTOR_SIZE]);
/* Count is 1..128. Whole range is checked before side effects. completed counts
 * confirmed sectors only. All buffers return on return; discard the current
 * incomplete read sector on error. Uses CMD18/25 only when binding.multiblock=true. A stop/status failure invalidates
 * the operation even if completed==count; writes are not atomic. */
BspStatus sd_card_read(SdCard *card, uint32_t lba, uint32_t count,
                       uint8_t *data, uint32_t *completed);
BspStatus sd_card_write(SdCard *card, uint32_t lba, uint32_t count,
                        const uint8_t *data, uint32_t *completed);
BspStatus sd_card_sync(SdCard *card);
/* Unmount/close files first. Releases CS, never deinitializes caller-owned BSP.
 * Failure means resource cleanup is unconfirmed; owner must resolve the fault. */
BspStatus sd_card_deinit(SdCard *card);

#endif
