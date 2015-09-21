/*
 * SecureDigital flash drive on SPI port.
 *
 * These cards are known to work:
 * 1) NCP SD 256Mb       - type 1, 249856 kbytes,  244 Mbytes
 * 2) Patriot SD 2Gb     - type 2, 1902592 kbytes, 1858 Mbytes
 * 3) Wintec microSD 2Gb - type 2, 1969152 kbytes, 1923 Mbytes
 * 4) Transcend SDHC 4Gb - type 3, 3905536 kbytes, 3814 Mbytes
 * 5) Verbatim SD 2Gb    - type 2, 1927168 kbytes, 1882 Mbytes
 * 6) SanDisk SDHC 4Gb   - type 3, 3931136 kbytes, 3833 Mbytes
 *
 * Copyright (C) 2010 Serge Vakulenko, <serge@vak.ru>
 *
 * Permission to use, copy, modify, and distribute this software
 * and its documentation for any purpose and without fee is hereby
 * granted, provided that the above copyright notice appear in all
 * copies and that both that the copyright notice and this
 * permission notice and warranty disclaimer appear in supporting
 * documentation, and that the name of the author not be used in
 * advertising or publicity pertaining to distribution of the
 * software without specific, written prior permission.
 *
 * The author disclaim all warranties with regard to this
 * software, including all implied warranties of merchantability
 * and fitness.  In no event shall the author be liable for any
 * special, indirect or consequential damages or any damages
 * whatsoever resulting from loss of use, data or profits, whether
 * in an action of contract, negligence or other tortious action,
 * arising out of or in connection with the use or performance of
 * this software.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/buf.h>
#include <sys/errno.h>
#include <sys/dk.h>
#include <sys/rdisk.h>
#include <sys/spi.h>
#include <sys/debug.h>
#include <sys/kconfig.h>

/*
 * Two SD/MMC disks on SPI.
 * Signals for SPI1:
 *      D0  - SDO1
 *      D10 - SCK1
 *      C4  - SDI1
 */
#define NSD             2
#define SECTSIZE        512
#define SPI_ENHANCED            /* use SPI fifo */
#ifndef SD0_MHZ
#define SD0_MHZ          13      /* speed 13.33 MHz */
#endif
#ifndef SD1_MHZ
#define SD1_MHZ          13      /* speed 13.33 MHz */
#endif

#define TIMO_WAIT_WDONE 400000
#define TIMO_WAIT_WIDLE 200000
#define TIMO_WAIT_CMD   100000
#define TIMO_WAIT_WDATA 30000
#define TIMO_READ       90000
#define TIMO_SEND_OP    8000
#define TIMO_CMD        7000
#define TIMO_SEND_CSD   6000
#define TIMO_WAIT_WSTOP 5000

int sd_type[NSD];               /* Card type */
struct spiio sd_io[NSD];        /* Data for SPI driver */
int sd_dkn = -1;                /* Statistics slot number */

int sd_timo_cmd;                /* Max timeouts, for sysctl */
int sd_timo_send_op;
int sd_timo_send_csd;
int sd_timo_read;
int sd_timo_wait_cmd;
int sd_timo_wait_wdata;
int sd_timo_wait_wdone;
int sd_timo_wait_wstop;
int sd_timo_wait_widle;

/*
 * Definitions for MMC/SDC commands.
 */
#define CMD_GO_IDLE             0       /* CMD0 */
#define CMD_SEND_OP_MMC         1       /* CMD1 (MMC) */
#define CMD_SEND_IF_COND        8
#define CMD_SEND_CSD            9
#define CMD_SEND_CID            10
#define CMD_STOP                12
#define CMD_SEND_STATUS         13      /* CMD13 */
#define CMD_SET_BLEN            16
#define CMD_READ_SINGLE         17
#define CMD_READ_MULTIPLE       18
#define CMD_SET_BCOUNT          23      /* (MMC) */
#define CMD_SET_WBECNT          23      /* ACMD23 (SDC) */
#define CMD_WRITE_SINGLE        24
#define CMD_WRITE_MULTIPLE      25
#define CMD_SEND_OP_SDC         41      /* ACMD41 (SDC) */
#define CMD_APP                 55      /* CMD55 */
#define CMD_READ_OCR            58

#define DATA_START_BLOCK        0xFE    /* start data for single block */
#define STOP_TRAN_TOKEN         0xFD    /* stop token for write multiple */
#define WRITE_MULTIPLE_TOKEN    0xFC    /* start data for write multiple */

// Add extra clocks after a deselect
void sd_deselect(struct spiio *io)
{
    spi_deselect(io);
    spi_transfer(io, 0xFF);
}

/*
 * Wait while busy, up to 300 msec.
 */

static void spi_wait_ready (int unit, int limit, int *maxcount)
{
    int i;
    struct spiio *io = &sd_io[unit];

    spi_transfer(io, 0xFF);
    for (i=0; i<limit; i++)
    {
        if (spi_transfer(io, 0xFF) == 0xFF)
        {
            if (*maxcount < i)
                *maxcount = i;
            return;
        }
    }
    printf ("sd%d: wait_ready(%d) failed\n",unit, limit);
}

/*
 * Send a command and address to SD media.
 * Return response:
 *   FF - timeout
 *   00 - command accepted
 *   01 - command received, card in idle state
 *
 * Other codes:
 *   bit 0 = Idle state
 *   bit 1 = Erase Reset
 *   bit 2 = Illegal command
 *   bit 3 = Communication CRC error
 *   bit 4 = Erase sequence error
 *   bit 5 = Address error
 *   bit 6 = Parameter error
 *   bit 7 = Always 0
 */
static int card_cmd(unsigned int unit, unsigned int cmd, unsigned int addr)
{
    int i, reply;
    struct spiio *io = &sd_io[unit];

    /* Wait for not busy, up to 300 msec. */
    if (cmd != CMD_GO_IDLE)
        spi_wait_ready(unit, TIMO_WAIT_CMD, &sd_timo_wait_cmd);

    /* Send a comand packet (6 bytes). */
    spi_transfer(io, cmd | 0x40);
    spi_transfer(io, addr >> 24);
    spi_transfer(io, addr >> 16);
    spi_transfer(io, addr >> 8);
    spi_transfer(io, addr);

    /* Send cmd checksum for CMD_GO_IDLE.
     * For all other commands, CRC is ignored. */
    if (cmd == CMD_GO_IDLE)
        spi_transfer(io, 0x95);
    else if (cmd == CMD_SEND_IF_COND)
        spi_transfer(io, 0x87);
    else
        spi_transfer(io, 0xFF);

    /* Wait for a response. */
    for (i=0; i<TIMO_CMD; i++)
    {
        reply = spi_transfer(io, 0xFF);
        if (! (reply & 0x80))
        {
            if (sd_timo_cmd < i)
                sd_timo_cmd = i;
            return reply;
        }
    }
    if (cmd != CMD_GO_IDLE)
    {
        printf ("sd%d: card_cmd timeout, cmd=%02x, addr=%08x, reply=%02x\n",
            unit,cmd, addr, reply);
    }
    return reply;
}

/*
 * Initialize a card.
 * Return nonzero if successful.
 */
int card_init(int unit)
{
    int i, reply;
    unsigned char response[4];
    int timeout = 4;
    struct spiio *io = &sd_io[unit];

    /* Slow speed: 250 kHz */
    spi_brg(io, 250);

    sd_type[unit] = 0;

    do {
        /* Unselect the card. */
        sd_deselect(io);

        /* Send 80 clock cycles for start up. */
        for (i=0; i<10; i++)
            spi_transfer(io, 0xFF);

        /* Select the card and send a single GO_IDLE command. */
        spi_select(io);
        timeout--;
        reply = card_cmd(unit, CMD_GO_IDLE, 0);

    } while ((reply != 0x01) && (timeout != 0));

    sd_deselect(io);
    if (reply != 1)
    {
        /* It must return Idle. */
        return 0;
    }

    /* Check SD version. */
    spi_select(io);
    reply = card_cmd(unit, CMD_SEND_IF_COND, 0x1AA);
    if (reply & 4)
    {
        /* Illegal command: card type 1. */
        sd_deselect(io);
        sd_type[unit] = 1;
    } else {
        response[0] = spi_transfer(io, 0xFF);
        response[1] = spi_transfer(io, 0xFF);
        response[2] = spi_transfer(io, 0xFF);
        response[3] = spi_transfer(io, 0xFF);
        sd_deselect(io);
        if (response[3] != 0xAA)
        {
            printf ("sd%d: cannot detect card type, response=%02x-%02x-%02x-%02x\n",
                unit, response[0], response[1], response[2], response[3]);
            return 0;
        }
        sd_type[unit] = 2;
    }

    /* Send repeatedly SEND_OP until Idle terminates. */
    for (i=0; ; i++)
    {
        spi_select(io);
        card_cmd(unit,CMD_APP, 0);
        reply = card_cmd(unit,CMD_SEND_OP_SDC,
                        (sd_type[unit] == 2) ? 0x40000000 : 0);
        spi_select(io);
        if (reply == 0)
            break;
        if (i >= TIMO_SEND_OP)
        {
            /* Init timed out. */
            printf ("card_init: SEND_OP timed out, reply = %d\n", reply);
            return 0;
        }
    }
    if (sd_timo_send_op < i)
        sd_timo_send_op = i;

    /* If SD2 read OCR register to check for SDHC card. */
    if (sd_type[unit] == 2)
    {
        spi_select(io);
        reply = card_cmd(unit, CMD_READ_OCR, 0);
        if (reply != 0)
        {
            sd_deselect(io);
            printf ("sd%d: READ_OCR failed, reply=%02x\n", unit, reply);
            return 0;
        }
        response[0] = spi_transfer(io, 0xFF);
        response[1] = spi_transfer(io, 0xFF);
        response[2] = spi_transfer(io, 0xFF);
        response[3] = spi_transfer(io, 0xFF);
        sd_deselect(io);
        if ((response[0] & 0xC0) == 0xC0)
        {
            sd_type[unit] = 3;
        }
    }

    /* Fast speed. */
    spi_brg(io, SD0_MHZ * 1000);
    return 1;
}

/*
 * Get number of sectors on the disk.
 * Return nonzero if successful.
 */
int sdsize(int unit)
{
    unsigned char csd [16];
    unsigned csize, n;
    int reply, i;
    int nsectors;
    struct spiio *io = &sd_io[unit];

    spi_select(io);
    reply = card_cmd(unit,CMD_SEND_CSD, 0);
    if (reply != 0)
    {
        /* Command rejected. */
        sd_deselect(io);
        return 0;
    }
    /* Wait for a response. */
    for (i=0; ; i++)
    {
        reply = spi_transfer(io, 0xFF);
        if (reply == DATA_START_BLOCK)
            break;
        if (i >= TIMO_SEND_CSD)
        {
            /* Command timed out. */
            sd_deselect(io);
            printf ("sd%d: card_size: SEND_CSD timed out, reply = %d\n",
                unit, reply);
            return 0;
        }
    }
    if (sd_timo_send_csd < i)
        sd_timo_send_csd = i;

    /* Read data. */
    for (i=0; i<sizeof(csd); i++)
    {
        csd [i] = spi_transfer(io, 0xFF);
    }
    /* Ignore CRC. */
    spi_transfer(io, 0xFF);
    spi_transfer(io, 0xFF);

    /* Disable the card. */
    sd_deselect(io);

    /* CSD register has different structure
     * depending upon protocol version. */
    switch (csd[0] >> 6)
    {
        case 1:                 /* SDC ver 2.00 */
            csize = csd[9] + (csd[8] << 8) + 1;
            nsectors = csize << 10;
            break;
        case 0:                 /* SDC ver 1.XX or MMC. */
            n = (csd[5] & 15) + ((csd[10] & 128) >> 7) + ((csd[9] & 3) << 1) + 2;
            csize = (csd[8] >> 6) + (csd[7] << 2) + ((csd[6] & 3) << 10) + 1;
            nsectors = csize << (n - 9);
            break;
        default:                /* Unknown version. */
            return 0;
    }
    return nsectors>>1;
}

/*
 * Read a block of data.
 * Return nonzero if successful.
 */
int card_read(int unit, unsigned int offset, char *data, unsigned int bcount)
{
    int reply, i;
    struct spiio *io = &sd_io[unit];

    /* Send read-multiple command. */
    spi_select(io);
    if (sd_type[unit] != 3)
        offset <<= 9;
    reply = card_cmd(unit, CMD_READ_MULTIPLE, offset<<1);
    if (reply != 0)
    {
        /* Command rejected. */
        printf ("sd%d: card_read: bad READ_MULTIPLE reply = %d, offset = %08x\n",
            unit, reply, offset<<1);
        sd_deselect(io);
        return 0;
    }

again:
    /* Wait for a response. */
    for (i=0; ; i++)
    {
        int x = spl0();
        reply = spi_transfer(io, 0xFF);
        splx(x);
        if (reply == DATA_START_BLOCK)
            break;
        if (i >= TIMO_READ)
        {
            /* Command timed out. */
            printf ("sd%d: card_read: READ_MULTIPLE timed out, reply = %d\n",
                unit, reply);
            sd_deselect(io);
            return 0;
        }
    }
    if (sd_timo_read < i)
        sd_timo_read = i;

    /* Read data. */
    if (bcount >= SECTSIZE)
    {
        spi_bulk_read_32_be(io, SECTSIZE,data);
        data += SECTSIZE;
    } else {
        spi_bulk_read(io, bcount, (unsigned char *)data);
        data += bcount;
        for (i=bcount; i<SECTSIZE; i++)
            spi_transfer(io, 0xFF);
    }
    /* Ignore CRC. */
    spi_transfer(io, 0xFF);
    spi_transfer(io, 0xFF);

    if (bcount > SECTSIZE)
    {
        /* Next sector. */
        bcount -= SECTSIZE;
        goto again;
    }

    /* Stop a read-multiple sequence. */
    card_cmd(unit, CMD_STOP, 0);
    sd_deselect(io);
    return 1;
}

/*
 * Write a block of data.
 * Return nonzero if successful.
 */
int
card_write (int unit, unsigned offset, char *data, unsigned bcount)
{
    unsigned reply, i;
    struct spiio *io = &sd_io[unit];

    /* Send pre-erase count. */
    spi_select(io);
    card_cmd(unit, CMD_APP, 0);
    reply = card_cmd(unit, CMD_SET_WBECNT, (bcount + SECTSIZE - 1) / SECTSIZE);
    if (reply != 0)
    {
        /* Command rejected. */
        sd_deselect(io);
        printf("sd%d: card_write: bad SET_WBECNT reply = %02x, count = %u\n",
            unit, reply, (bcount + SECTSIZE - 1) / SECTSIZE);
        return 0;
    }

    /* Send write-multiple command. */
    if (sd_type[unit] != 3) offset <<= 9;
    reply = card_cmd(unit, CMD_WRITE_MULTIPLE, offset<<1);
    if (reply != 0)
    {
        /* Command rejected. */
        sd_deselect(io);
        printf("sd%d: card_write: bad WRITE_MULTIPLE reply = %02x\n", unit, reply);
        return 0;
    }
    sd_deselect(io);
again:
    /* Select, wait while busy. */
    spi_select(io);
    spi_wait_ready(unit, TIMO_WAIT_WDATA, &sd_timo_wait_wdata);

    /* Send data. */
    spi_transfer(io, WRITE_MULTIPLE_TOKEN);
    if (bcount >= SECTSIZE)
    {
        spi_bulk_write_32_be(io, SECTSIZE, data);
        data += SECTSIZE;
    } else {
        spi_bulk_write(io, bcount, (unsigned char *)data);
        data += bcount;
        for (i=bcount; i<SECTSIZE; i++)
            spi_transfer(io, 0xFF);
    }
    /* Send dummy CRC. */
    spi_transfer(io, 0xFF);
    spi_transfer(io, 0xFF);

    /* Check if data accepted. */
    reply = spi_transfer(io, 0xFF);
    if ((reply & 0x1f) != 0x05)
    {
        /* Data rejected. */
        sd_deselect(io);
        printf("sd%d: card_write: data rejected, reply = %02x\n", unit,reply);
        return 0;
    }

    /* Wait for write completion. */
    int x = spl0();
    spi_wait_ready(unit, TIMO_WAIT_WDONE, &sd_timo_wait_wdone);
    splx(x);
    sd_deselect(io);

    if (bcount > SECTSIZE)
    {
        /* Next sector. */
        bcount -= SECTSIZE;
        goto again;
    }

    /* Stop a write-multiple sequence. */
    spi_select(io);
    spi_wait_ready(unit, TIMO_WAIT_WSTOP, &sd_timo_wait_wstop);
    spi_transfer(io, STOP_TRAN_TOKEN);
    spi_wait_ready(unit, TIMO_WAIT_WIDLE, &sd_timo_wait_widle);
    sd_deselect(io);
    return 1;
}

void sd_preinit (int unit)
{
}

/*
 * Detect a card.
 */
int sdinit (int unit, int flag)
{
    struct spiio *io = &sd_io[unit];
    unsigned nsectors;

#ifdef SD0_ENA_PORT
    /* On Duinomite Mega board, pin B13 set low
     * enables a +3.3V power to SD card. */
    if (unit == 0) {
        LAT_CLR(SD0_ENA_PORT) = 1 << SD0_ENA_PIN;
        udelay (1000);
    }
#endif

#ifdef SD1_ENA_PORT
    /* On Duinomite Mega board, pin B13 set low
     * enables a +3.3V power to SD card. */
    if (unit == 1) {
        LAT_CLR(SD1_ENA_PORT) = 1 << SD1_ENA_PIN;
        udelay (1000);
    }
#endif

    if (!card_init(unit))
    {
        printf ("sd%d: no SD/MMC card detected\n", unit);
        return ENODEV;
    }
    if ((nsectors=sdsize(unit))==0)
    {
        printf ("sd%d: cannot get card size\n", unit);
        return ENODEV;
    }
    if (! (flag & S_SILENT))
    {
        printf ("sd%d: type %s, size %u kbytes, speed %u Mbit/sec\n", unit,
            sd_type[unit]==3 ? "SDHC" :
            sd_type[unit]==2 ? "II" : "I",
            nsectors,
            spi_get_brg(io) / 1000);
    }
    DEBUG("sd%d: init done\n",unit);
    return 0;
}

int sddeinit(int unit)
{
#ifdef SD0_ENA_PORT
    /* On Duinomite Mega board, pin B13 set low
     * enables a +3.3V power to SD card. */
    if (unit == 0) {
        LAT_SET(SD0_ENA_PORT) = 1 << SD0_ENA_PIN;
        udelay (1000);
    }
#endif

#ifdef SD1_ENA_PORT
    /* On Duinomite Mega board, pin B13 set low
     * enables a +3.3V power to SD card. */
    if (unit == 1) {
        LAT_SET(SD1_ENA_PORT) = 1 << SD1_ENA_PIN;
        udelay (1000);
    }
#endif
    return 0;
}

int sdopen(int unit, int flags, int mode)
{
    DEBUG("sd%d: open\n",unit);
    return 0;
}

/*
 * Test to see if device is present.
 * Return true if found and initialized ok.
 */
static int
sdprobe(config)
    struct conf_device *config;
{
    int unit = config->dev_unit;
    int cs = config->dev_pins[0];
    struct spiio *io = &sd_io[unit];

    if (unit < 0 || unit >= NSD)
        return 0;
    printf("sd%u: port SPI%d, pin cs=R%c%d\n", unit,
        config->dev_ctlr, gpio_portname(cs), gpio_pinno(cs));

    int port = (cs >> 4) - 1;
    int pin = cs & 15;
    struct gpioreg *base = port + (struct gpioreg*) &TRISA;

    if (spi_setup(io, config->dev_ctlr, (unsigned int*) base, pin) != 0) {
        printf("sd%u: cannot open SPI%u port\n", unit, config->dev_ctlr);
        return 0;
    }

#ifdef SD0_ENA_PORT
    if (unit == 0) {
        /* Enable SD0 phy - pin is assumed to be active low */
        TRIS_CLR(SD0_ENA_PORT) = 1 << SD0_ENA_PIN;
        LAT_CLR(SD0_ENA_PORT) = 1 << SD0_ENA_PIN;
        udelay (1000);
    }
#endif

#ifdef SD1_ENA_PORT
    if (unit == 1) {
        /* Enable SD1 phy - pin is assumed to be active low */
        TRIS_CLR(SD1_ENA_PORT) = 1 << SD1_ENA_PIN;
        LAT_CLR(SD1_ENA_PORT) = 1 << SD1_ENA_PIN;
        udelay (1000);
    }
#endif

    spi_brg(io, SD0_MHZ * 1000);
    spi_set(io, PIC32_SPICON_CKE);
    return 1;
}

struct driver sddriver = {
    "sd", sdprobe,
};
