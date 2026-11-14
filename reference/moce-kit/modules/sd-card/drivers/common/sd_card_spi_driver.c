#include "sd_card_spi_driver.h"
#include <string.h>

/* Device protocol only. These helpers terminate at public BSP declarations. */
static BspStatus exchange(SdCard *c, const uint8_t *tx, uint8_t *rx, size_t n) {
    return bsp_spi_transfer(c->binding.spi, tx, rx, n, c->binding.transfer_timeout_ms);
}

static BspStatus receive(SdCard *c, uint8_t *data, size_t n) {
    uint8_t dummy[32];
    memset(dummy, 0xff, sizeof(dummy));
    while (n) {
        size_t part = n < sizeof(dummy) ? n : sizeof(dummy);
        BspStatus s = exchange(c, dummy, data, part);
        if (s != BSP_OK) return s;
        if (data) data += part;
        n -= part;
    }
    return BSP_OK;
}

static BspStatus finish(SdCard *c, BspStatus status) {
    BspStatus cleanup = bsp_gpio_write(c->binding.cs, true);
    /* Release clocks are sent only when deselection succeeded. On a faulted
     * SPI the public call rejects; no recovery or register repair is attempted. */
    if (cleanup == BSP_OK) cleanup = receive(c, NULL, 1);
    c->diagnostic.cleanup_status = cleanup;
    if (status == BSP_OK) status = cleanup;
    c->diagnostic.status = status;
    if (status != BSP_OK) c->ready = false;
    return status;
}

static uint8_t command_crc(const uint8_t *p) {
    uint8_t crc = 0;
    for (size_t i = 0; i < 5; ++i) {
        uint8_t byte = p[i];
        for (unsigned j = 0; j < 8; ++j) {
            crc = (uint8_t)(crc << 1);
            if ((byte ^ crc) & 0x80u) crc ^= 0x09u;
            byte = (uint8_t)(byte << 1);
        }
    }
    return (uint8_t)((crc << 1) | 1u);
}

static uint16_t data_crc(const uint8_t *p, size_t n) {
    uint16_t crc = 0;
    for (size_t i = 0; i < n; ++i) {
        crc ^= (uint16_t)((uint16_t)p[i] << 8);
        for (unsigned j = 0; j < 8; ++j)
            crc = (uint16_t)((crc & 0x8000u) ? ((uint32_t)crc << 1) ^ 0x1021u : (uint32_t)crc << 1);
    }
    return crc;
}

static BspStatus wait_ready(SdCard *c) {
    uint32_t start = bsp_time_ms();
    /* Byte guard also bounds a non-progressing timebase between BSP calls.
     * A working timebase is still mandatory inside each blocking BSP call. */
    for (uint32_t i = 0; i < 65536u; ++i) {
        uint8_t byte;
        BspStatus s = receive(c, &byte, 1);
        if (s != BSP_OK) return s;
        if (byte == 0xffu) return BSP_OK;
        if ((uint32_t)(bsp_time_ms() - start) >= c->binding.ready_timeout_ms) break;
    }
    return BSP_TIMEOUT;
}

/* Leaves CS low for response tail/data. Every caller must finish(). */
static BspStatus command(SdCard *c, uint8_t index, uint32_t argument) {
    c->diagnostic.command = index;
    c->diagnostic.r1 = 0xffu;
    c->diagnostic.token = 0xffu;
    BspStatus s = bsp_gpio_write(c->binding.cs, true);
    if (s == BSP_OK) s = receive(c, NULL, 1);
    if (s == BSP_OK) s = bsp_gpio_write(c->binding.cs, false);
    if (s == BSP_OK) s = wait_ready(c);
    if (s != BSP_OK) return s;
    uint8_t frame[6] = {(uint8_t)(0x40u | index), (uint8_t)(argument >> 24),
        (uint8_t)(argument >> 16), (uint8_t)(argument >> 8), (uint8_t)argument, 0};
    frame[5] = command_crc(frame);
    s = exchange(c, frame, NULL, sizeof(frame));
    if (s != BSP_OK) return s;
    for (unsigned i = 0; i < 8; ++i) {
        s = receive(c, &c->diagnostic.r1, 1);
        if (s != BSP_OK) return s;
        if (!(c->diagnostic.r1 & 0x80u)) return BSP_OK;
    }
    return BSP_TIMEOUT;
}

static BspStatus read_packet(SdCard *c, uint8_t *data, size_t n) {
    uint32_t start = bsp_time_ms();
    BspStatus s;
    bool found = false;
    for (uint32_t i = 0; i < 65536u; ++i) {
        s = receive(c, &c->diagnostic.token, 1);
        if (s != BSP_OK) return s;
        if (c->diagnostic.token == 0xfeu) { found = true; break; }
        if (c->diagnostic.token != 0xffu) return BSP_IO;
        if ((uint32_t)(bsp_time_ms() - start) >= c->binding.ready_timeout_ms) break;
    }
    if (!found) return BSP_TIMEOUT;
    s = receive(c, data, n);
    uint8_t crc[2];
    if (s == BSP_OK) s = receive(c, crc, 2);
    if (s == BSP_OK && data_crc(data, n) != (uint16_t)((uint16_t)crc[0] << 8 | crc[1])) {
        c->diagnostic.crc_mismatch = true;
        s = BSP_IO;
    }
    return s;
}

static BspStatus require_port(SdCard *c, BspPort port) {
    uint32_t missing = 0;
    BspStatus s = bsp_port_require(port, BSP_CAP_BLOCKING, &missing);
    if (s != BSP_OK) {
        c->diagnostic.missing_port = port;
        c->diagnostic.missing_caps = missing;
        c->diagnostic.status = s;
    }
    return s;
}

BspStatus sd_card_init(SdCard *c, const SdCardBinding *binding) {
    if (!c || !binding) return BSP_INVALID_ARG;
    /* Copy first: binding may point to c->binding on an explicit reinit. */
    SdCardBinding b = *binding;
    if (b.spi == BSP_PORT_NONE || b.cs == BSP_PORT_NONE || b.spi == b.cs ||
        (b.spi & 0xff00u) != BSP_PORT(BSP_SPI, 0) ||
        (b.cs & 0xff00u) != BSP_PORT(BSP_GPIO, 0) ||
        !b.transfer_timeout_ms || b.transfer_timeout_ms > 1000u ||
        !b.ready_timeout_ms || b.ready_timeout_ms > 1000u ||
        !b.init_timeout_ms || b.init_timeout_ms > 5000u) return BSP_INVALID_ARG;
    memset(c, 0, sizeof(*c));
    c->binding = b;
    c->diagnostic = (SdCardDiagnostic){.stage="capabilities", .missing_port=BSP_PORT_NONE,
        .command=0xffu, .r1=0xffu, .token=0xffu};
    BspStatus s = require_port(c, b.spi);
    if (s == BSP_OK) s = require_port(c, b.cs);
    if (s != BSP_OK) return s; /* No peripheral effects on missing capability. */

    c->diagnostic.stage = "power-clock";
    s = bsp_gpio_write(b.cs, true);
    if (s == BSP_OK) s = bsp_delay_ms(2);
    if (s == BSP_OK) s = receive(c, NULL, 10); /* 80 clocks, CS and MOSI high. */
    if (s != BSP_OK) return finish(c, s);

    c->diagnostic.stage = "CMD0";
    s = command(c, 0, 0);
    if (s == BSP_OK && c->diagnostic.r1 != 1) s = BSP_IO;
    s = finish(c, s);
    if (s != BSP_OK) return s;

    c->diagnostic.stage = "CMD8";
    s = command(c, 8, 0x1aau);
    bool version2 = false;
    if (s == BSP_OK) {
        if (c->diagnostic.r1 == 1) {
            uint8_t r7[4];
            s = receive(c, r7, sizeof(r7));
            if (s == BSP_OK && (r7[0] || r7[1] || r7[2] != 1 || r7[3] != 0xaau)) s = BSP_UNSUPPORTED;
            version2 = true;
        } else if (c->diagnostic.r1 != 5) s = BSP_IO;
    }
    s = finish(c, s);
    if (s != BSP_OK) return s;

    uint32_t start = bsp_time_ms();
    bool initialized = false;
    for (unsigned attempt = 0; attempt < 5000; ++attempt) {
        c->diagnostic.stage = "CMD55";
        s = command(c, 55, 0);
        if (s == BSP_OK && c->diagnostic.r1 > 1) s =
            (c->diagnostic.r1 == 5) ? BSP_UNSUPPORTED : BSP_IO;
        s = finish(c, s);
        if (s != BSP_OK) return s;
        c->diagnostic.stage = "ACMD41";
        s = command(c, 41, version2 ? 0x40000000u : 0);
        if (s == BSP_OK && c->diagnostic.r1 > 1) s = BSP_IO;
        s = finish(c, s);
        if (s != BSP_OK) return s;
        if (c->diagnostic.r1 == 0) { initialized = true; break; }
        if ((uint32_t)(bsp_time_ms() - start) >= b.init_timeout_ms) break;
        s = bsp_delay_ms(1);
        if (s != BSP_OK) { c->diagnostic.status = s; return s; }
    }
    if (!initialized) { c->diagnostic.status = BSP_TIMEOUT; return BSP_TIMEOUT; }

    c->diagnostic.stage = "CMD58";
    uint8_t ocr[4];
    s = command(c, 58, 0);
    if (s == BSP_OK && c->diagnostic.r1 != 0) s = BSP_IO;
    if (s == BSP_OK) s = receive(c, ocr, sizeof(ocr));
    if (s == BSP_OK) {
        /* Power-up complete; OCR 3.2-3.4V window includes the Board's 3.3V. */
        memcpy(c->ocr, ocr, sizeof(ocr));
        if (!(ocr[0] & 0x80u) || !(ocr[1] & 0x30u)) s = BSP_UNSUPPORTED;
        c->block_addressed = version2 && (ocr[0] & 0x40u) != 0;
    }
    s = finish(c, s);
    if (s != BSP_OK) return s;

    if (!c->block_addressed) {
        c->diagnostic.stage = "CMD16";
        s = command(c, 16, SD_CARD_SECTOR_SIZE);
        if (s == BSP_OK && c->diagnostic.r1 != 0) s = BSP_IO;
        s = finish(c, s);
        if (s != BSP_OK) return s;
    }

    c->diagnostic.stage = "CMD9";
    uint8_t csd[16];
    s = command(c, 9, 0);
    if (s == BSP_OK && c->diagnostic.r1 != 0) s = BSP_IO;
    if (s == BSP_OK) s = read_packet(c, csd, sizeof(csd));
    s = finish(c, s);
    if (s != BSP_OK) return s;
    unsigned structure = csd[0] >> 6;
    memcpy(c->csd, csd, sizeof(csd));
    c->write_protected = (csd[14] & 0x30u) != 0;
    if (structure == 1 && c->block_addressed) {
        uint32_t size = ((uint32_t)(csd[7] & 0x3fu) << 16) | ((uint32_t)csd[8] << 8) | csd[9];
        c->sector_count = ((uint64_t)size + 1u) * 1024u;
    } else if (structure == 0 && !c->block_addressed) {
        unsigned length = csd[5] & 15u;
        unsigned multiplier = ((csd[9] & 3u) << 1) | (csd[10] >> 7);
        uint32_t size = ((uint32_t)(csd[6] & 3u) << 10) | ((uint32_t)csd[7] << 2) | (csd[8] >> 6);
        if (length < 9 || length > 11) s = BSP_UNSUPPORTED;
        else c->sector_count = ((uint64_t)size + 1u) << (multiplier + 2u + length - 9u);
    } else s = BSP_UNSUPPORTED;
    if (!c->sector_count || c->sector_count > UINT32_MAX ||
        (!c->block_addressed && c->sector_count > 0x800000u)) s = BSP_UNSUPPORTED;
    if (s == BSP_OK) {
        c->diagnostic.stage = "CMD10";
        s = command(c, 10, 0);
        if (s == BSP_OK && c->diagnostic.r1 != 0) {
            s = BSP_IO;
        }
        if (s == BSP_OK) {
            s = read_packet(c, c->cid, sizeof(c->cid));
        }
        s = finish(c, s);
    }
    c->diagnostic.status = s;
    c->ready = s == BSP_OK;
    if (c->ready) c->diagnostic.stage = "ready";
    return s;
}

static BspStatus card_status(SdCard *c) {
    BspStatus s = command(c, 13, 0);
    c->diagnostic.r2 = 0xffu;
    if (s == BSP_OK) {
        s = receive(c, &c->diagnostic.r2, 1);
    }
    if (s == BSP_OK && (c->diagnostic.r1 || c->diagnostic.r2)) {
        s = BSP_IO;
    }
    return finish(c, s);
}

BspStatus sd_card_write_sector(SdCard *c, uint32_t lba, const uint8_t data[SD_CARD_SECTOR_SIZE]) {
    if (!c || !data) {
        return BSP_INVALID_ARG;
    }
    if (!c->ready) {
        return BSP_NOT_READY;
    }
    if (!c->binding.allow_write || c->write_protected) {
        return BSP_UNSUPPORTED;
    }
    if ((uint64_t)lba >= c->sector_count) {
        return BSP_INVALID_ARG;
    }
    c->diagnostic.stage = "CMD24";
    uint32_t address = c->block_addressed ? lba : lba * SD_CARD_SECTOR_SIZE;
    BspStatus s = command(c, 24, address);
    if (s == BSP_OK && c->diagnostic.r1 != 0) {
        s = BSP_IO;
    }
    /* One idle byte before start token. CS remains asserted through programming. */
    const uint8_t start[] = {0xffu, 0xfeu};
    if (s == BSP_OK) {
        s = exchange(c, start, NULL, sizeof(start));
    }
    if (s == BSP_OK) {
        s = exchange(c, data, NULL, SD_CARD_SECTOR_SIZE);
    }
    uint16_t crc = data_crc(data, SD_CARD_SECTOR_SIZE);
    const uint8_t tail[] = {(uint8_t)(crc >> 8), (uint8_t)crc};
    if (s == BSP_OK) {
        s = exchange(c, tail, NULL, sizeof(tail));
    }
    if (s == BSP_OK) {
        /* Allow response latency, but never clock an unbounded stream. */
        for (unsigned i = 0; i < 8; ++i) {
            s = receive(c, &c->diagnostic.token, 1);
            if (s != BSP_OK || c->diagnostic.token != 0xffu) {
                break;
            }
        }
        if (s == BSP_OK && (c->diagnostic.token & 0x1fu) != 0x05u) {
            s = BSP_IO;
        }
    }
    if (s == BSP_OK) {
        s = wait_ready(c);
    }
    s = finish(c, s);
    if (s == BSP_OK) {
        c->diagnostic.stage = "CMD13";
        s = card_status(c);
    }
    if (s == BSP_OK && c->sectors_written != UINT32_MAX) {
        ++c->sectors_written;
    }
    return s;
}

static BspStatus check_range(SdCard *c, uint32_t lba, uint32_t count,
                             const void *data, uint32_t *completed) {
    if (completed) {
        *completed = 0;
    }
    if (!c || !data || !completed || !count || count > 128u) {
        return BSP_INVALID_ARG;
    }
    if (!c->ready) {
        return BSP_NOT_READY;
    }
    if ((uint64_t)lba + count > c->sector_count) {
        return BSP_INVALID_ARG;
    }
    return BSP_OK;
}


SdCardState sd_card_get_state(const SdCard *c) {
    if (!c) { return SD_CARD_OFFLINE; }
    if (c->ready) { return SD_CARD_READY; }
    return c->diagnostic.status == BSP_OK ? SD_CARD_OFFLINE : SD_CARD_FAULT;
}

/* CMD12 is sent while CS remains asserted. Do not wait_ready before it:
 * a continuous read can be streaming data. Skip its mandatory stuff byte. */
static BspStatus stop_read(SdCard *c) {
    uint8_t frame[6] = {0x4c, 0, 0, 0, 0, 0};
    frame[5] = command_crc(frame);
    BspStatus s = exchange(c, frame, NULL, 6);
    if (s == BSP_OK) { s = receive(c, NULL, 1); }
    uint8_t r1 = 0xff;
    for (unsigned i = 0; s == BSP_OK && i < 8; ++i) {
        s = receive(c, &r1, 1);
        if (!(r1 & 0x80u)) { break; }
    }
    if (s == BSP_OK && r1) { s = (r1 & 0x80u) ? BSP_TIMEOUT : BSP_IO; }
    if (s == BSP_OK) { s = wait_ready(c); }
    return s;
}

static BspStatus stream_transfer(SdCard *c, uint32_t lba, uint32_t count,
                                uint8_t *read_data, const uint8_t *write_data,
                                uint32_t *completed) {
    bool writing = write_data != NULL;
    if (writing && (!c->binding.allow_write || c->write_protected)) {
        return BSP_UNSUPPORTED;
    }
    c->diagnostic.stage = writing ? "CMD25" : "CMD18";
    c->diagnostic.crc_mismatch = false;
    uint32_t addr = c->block_addressed ? lba : lba * SD_CARD_SECTOR_SIZE;
    BspStatus s = command(c, writing ? 25 : 18, addr);
    if (s == BSP_OK && c->diagnostic.r1) { s = BSP_IO; }
    bool started = s == BSP_OK;
    for (uint32_t i = 0; s == BSP_OK && i < count; ++i) {
        if (!writing) {
            s = read_packet(c, read_data + i * 512u, 512);
        } else {
            const uint8_t *data = write_data + i * 512u;
            const uint8_t token[2] = {0xff, 0xfc};
            s = wait_ready(c);
            if (s == BSP_OK) { s = exchange(c, token, NULL, 2); }
            if (s == BSP_OK) { s = exchange(c, data, NULL, 512); }
            uint16_t crc = data_crc(data, 512);
            uint8_t tail[2] = {(uint8_t)(crc >> 8), (uint8_t)crc};
            if (s == BSP_OK) { s = exchange(c, tail, NULL, 2); }
            for (unsigned j = 0; s == BSP_OK && j < 8; ++j) {
                s = receive(c, &c->diagnostic.token, 1);
                if (c->diagnostic.token != 0xff) { break; }
            }
            if (s == BSP_OK && (c->diagnostic.token & 31u) != 5u) { s = BSP_IO; }
            if (s == BSP_OK) { s = wait_ready(c); }
        }
        if (s == BSP_OK) { ++*completed; }
    }
    BspStatus stop = BSP_OK;
    if (started) {
        if (writing) {
            stop = wait_ready(c);
            const uint8_t token = 0xfd;
            if (stop == BSP_OK) { stop = exchange(c, &token, NULL, 1); }
            /* At least eight clocks before checking the programming busy. */
            if (stop == BSP_OK) { stop = receive(c, NULL, 1); }
            if (stop == BSP_OK) { stop = wait_ready(c); }
        } else { stop = stop_read(c); }
    }
    if (s == BSP_OK) { s = stop; }
    s = finish(c, s);
    if (c->diagnostic.cleanup_status == BSP_OK && stop != BSP_OK) {
        c->diagnostic.cleanup_status = stop;
    }
    if (s == BSP_OK && writing) { s = card_status(c); }
    uint32_t *counter = writing ? &c->sectors_written : &c->sectors_read;
    if (s == BSP_OK) {
        *counter = UINT32_MAX - *counter < count ? UINT32_MAX : *counter + count;
    }
    return s;
}

BspStatus sd_card_read(SdCard *c, uint32_t lba, uint32_t count,
                       uint8_t *data, uint32_t *completed) {
    BspStatus s = check_range(c, lba, count, data, completed);
    if (s != BSP_OK) {
        return s;
    }
    if (c->binding.multiblock && count > 1) {
        return stream_transfer(c, lba, count, data, NULL, completed);
    }
    for (uint32_t i = 0; i < count; ++i) {
        s = sd_card_read_sector(c, lba + i, data + i * SD_CARD_SECTOR_SIZE);
        if (s != BSP_OK) {
            return s;
        }
        ++*completed;
    }
    return BSP_OK;
}

BspStatus sd_card_write(SdCard *c, uint32_t lba, uint32_t count,
                        const uint8_t *data, uint32_t *completed) {
    BspStatus s = check_range(c, lba, count, data, completed);
    if (s != BSP_OK) {
        return s;
    }
    if (c->binding.multiblock && count > 1) {
        return stream_transfer(c, lba, count, NULL, data, completed);
    }
    for (uint32_t i = 0; i < count; ++i) {
        s = sd_card_write_sector(c, lba + i, data + i * SD_CARD_SECTOR_SIZE);
        if (s != BSP_OK) {
            return s;
        }
        ++*completed;
    }
    return BSP_OK;
}

BspStatus sd_card_sync(SdCard *c) {
    if (!c) {
        return BSP_INVALID_ARG;
    }
    if (!c->ready) {
        return BSP_NOT_READY;
    }
    c->diagnostic.stage = "sync-CMD13";
    return card_status(c);
}

BspStatus sd_card_deinit(SdCard *c) {
    if (!c) {
        return BSP_INVALID_ARG;
    }
    if (!c->binding.spi || !c->binding.cs) {
        return BSP_NOT_READY;
    }
    c->diagnostic.stage = "deinit";
    BspStatus s = finish(c, BSP_OK);
    c->ready = false;
    return s;
}

BspStatus sd_card_read_sector(SdCard *c, uint32_t lba, uint8_t data[SD_CARD_SECTOR_SIZE]) {
    if (!c || !data) return BSP_INVALID_ARG;
    if (!c->ready) return BSP_NOT_READY;
    if ((uint64_t)lba >= c->sector_count) return BSP_INVALID_ARG;
    c->diagnostic.stage = "CMD17";
    c->diagnostic.crc_mismatch = false;
    uint32_t address = c->block_addressed ? lba : lba * SD_CARD_SECTOR_SIZE;
    BspStatus s = command(c, 17, address);
    if (s == BSP_OK && c->diagnostic.r1 != 0) s = BSP_IO;
    if (s == BSP_OK) s = read_packet(c, data, SD_CARD_SECTOR_SIZE);
    s = finish(c, s);
    if (s == BSP_OK && c->sectors_read != UINT32_MAX) ++c->sectors_read;
    return s;
}
