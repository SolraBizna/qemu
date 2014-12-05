/*
 * QEMU IDE Emulation: MacIO support.
 *
 * Copyright (c) 2003 Fabrice Bellard
 * Copyright (c) 2006 Openedhand Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include "hw/hw.h"
#include "hw/ppc/mac.h"
#include "hw/ppc/mac_dbdma.h"
#include "sysemu/block-backend.h"
#include "sysemu/dma.h"

#include <hw/ide/internal.h>

/* debug MACIO */
 #define DEBUG_MACIO

#ifdef DEBUG_MACIO
static const int debug_macio = 1;
#else
static const int debug_macio = 0;
#endif

#define MACIO_DPRINTF(fmt, ...) do { \
        if (debug_macio) { \
            printf(fmt , ## __VA_ARGS__); \
        } \
    } while (0)


/***********************************************************/
/* MacIO based PowerPC IDE */

#define MACIO_PAGE_SIZE 4096

struct MACIODMAState {
    DBDMA_io *io;
    uint64_t offset;
};

typedef struct MACIODMAState MACIODMAState;

static void pmac_ide_atapi_transfer_cb(void *opaque, int ret);

static void pmac_dma_read(BlockBackend *blk,
                         int64_t sector_num, int nb_sectors,
                         void (*cb)(void *opaque, int ret), void *opaque)
{
    DBDMA_io *io = opaque;
    MACIOIDEState *m = io->opaque;
    IDEState *s = idebus_active_if(&m->bus);
    dma_addr_t dma_addr, dma_len;
    void *mem;
    int nsector, remainder;

    dma_addr = io->addr;
    dma_len = io->len;
    mem = dma_memory_map(&address_space_memory, dma_addr, &dma_len, DMA_DIRECTION_FROM_DEVICE);
    
    //nsector = ((io->len + 0x1ff) >> 9);
    nsector = nb_sectors;
    remainder = (nsector << 9) - io->len;
    
    qemu_iovec_init(&io->iov, 4096);
    qemu_iovec_add(&io->iov, mem, io->len);
    MACIO_DPRINTF("--- main transfer: %x\n", io->len);
    
    if (remainder) {
        MACIO_DPRINTF("--- remainder: %x\n", remainder);
        qemu_iovec_add(&io->iov, &io->remainder + 0x200 - remainder, remainder);
    }

    s->io_buffer_size -= io->len;
    s->io_buffer_index += io->len;
    
    io->len = 0;
    
    m->aiocb = blk_aio_readv(blk, sector_num, &io->iov, nsector,
                              cb, io);
}

static void pmac_ide_atapi_transfer_cb(void *opaque, int ret)
{
    DBDMA_io *io = opaque;
    MACIOIDEState *m = io->opaque;
    IDEState *s = idebus_active_if(&m->bus);
    int64_t sector_num;
    int nsector, remainder;
    
    MACIO_DPRINTF("s is %p\n", s);
    MACIO_DPRINTF("io_buffer_index: %x\n", s->io_buffer_index);
    MACIO_DPRINTF("io_buffer_size: %x   packet_transfer_size: %x\n", s->io_buffer_size, s->packet_transfer_size);
    MACIO_DPRINTF("lba: %x\n", s->lba);
    MACIO_DPRINTF("io_addr: %" HWADDR_PRIx "  io_len: %x\n", io->addr, io->len);
    
    if (ret < 0) {
         MACIO_DPRINTF("THERE WAS AN ERROR!  %d\n", ret);
         ide_atapi_io_error(s, ret);
         goto done;
    }
    
    if (!m->dma_active) {
        MACIO_DPRINTF("waiting for data (%#x - %#x - %x)\n",
                      s->nsector, io->len, s->status);
        /* data not ready yet, wait for the channel to get restarted */
        io->processing = false;
        return;
    }
    
    if (s->io_buffer_size <= 0) {
         ide_atapi_cmd_ok(s);
         m->dma_active = false;
         goto done;
    }    

    if (io->len == 0) {
        MACIO_DPRINTF("End of DMA transfer\n");
        goto done;
    }    
    
    if (s->lba == -1) {
        /* Non-block ATAPI transfer - just copy to RAM */
        s->io_buffer_size = MIN(s->io_buffer_size, io->len);
        cpu_physical_memory_write(io->addr, s->io_buffer, s->io_buffer_size);
        ide_atapi_cmd_ok(s);
        m->dma_active = false;
        goto done;
    }    
        
    /* Calculate number of sectors */
    sector_num = (int64_t)(s->lba << 2) + (s->io_buffer_index >> 9);
    nsector = (io->len + 0x1ff) >> 9;
    remainder = io->len & 0x1ff;
    
    //qemu_iovec_reset(&io->iov);

    MACIO_DPRINTF("nsector: %d   remainder: %x\n", nsector, remainder);
    //MACIO_DPRINTF("DMA addr: " DMA_ADDR_FMT " - " DMA_ADDR_FMT "  mem: %p\n", dma_addr, dma_len, mem); 
    MACIO_DPRINTF("sector: %lx   %lx\n", sector_num, io->iov.size / 512);
    
    pmac_dma_read(s->blk, sector_num, nsector,
                              pmac_ide_atapi_transfer_cb, io);
    return;
    
done:
    MACIO_DPRINTF("done DMA\n\n");
    io->dma_end(opaque);

    return;
}

static void pmac_ide_transfer_cb(void *opaque, int ret)
{
    DBDMA_io *io = opaque;
    MACIOIDEState *m = io->opaque;
    IDEState *s = idebus_active_if(&m->bus);
    int n = 0;
    int64_t sector_num;
    int unaligned;

    if (ret < 0) {
        MACIO_DPRINTF("DMA error\n");
        m->aiocb = NULL;
        qemu_sglist_destroy(&s->sg);
        ide_dma_error(s);
        io->remainder_len = 0;
        goto done;
    }

    if (--io->requests) {
        /* More requests still in flight */
        return;
    }

    if (!m->dma_active) {
        MACIO_DPRINTF("waiting for data (%#x - %#x - %x)\n",
                      s->nsector, io->len, s->status);
        /* data not ready yet, wait for the channel to get restarted */
        io->processing = false;
        return;
    }

    sector_num = ide_get_sector(s);
    MACIO_DPRINTF("io_buffer_size = %#x\n", s->io_buffer_size);
    if (s->io_buffer_size > 0) {
        m->aiocb = NULL;
        qemu_sglist_destroy(&s->sg);
        n = (s->io_buffer_size + 0x1ff) >> 9;
        sector_num += n;
        ide_set_sector(s, sector_num);
        s->nsector -= n;
    }

    if (io->finish_remain_read) {
        /* Finish a stale read from the last iteration */
        io->finish_remain_read = false;
        cpu_physical_memory_write(io->finish_addr, io->remainder,
                                  io->finish_len);
    }

    MACIO_DPRINTF("remainder: %d io->len: %d nsector: %d "
                  "sector_num: %" PRId64 "\n",
                  io->remainder_len, io->len, s->nsector, sector_num);
    if (io->remainder_len && io->len) {
        /* guest wants the rest of its previous transfer */
        int remainder_len = MIN(io->remainder_len, io->len);
        uint8_t *p = &io->remainder[0x200 - remainder_len];

        MACIO_DPRINTF("copying remainder %d bytes at %#" HWADDR_PRIx "\n",
                      remainder_len, io->addr);

        switch (s->dma_cmd) {
        case IDE_DMA_READ:
            cpu_physical_memory_write(io->addr, p, remainder_len);
            break;
        case IDE_DMA_WRITE:
            cpu_physical_memory_read(io->addr, p, remainder_len);
            break;
        case IDE_DMA_TRIM:
            break;
        }
        io->addr += remainder_len;
        io->len -= remainder_len;
        io->remainder_len -= remainder_len;

        if (s->dma_cmd == IDE_DMA_WRITE && !io->remainder_len) {
            io->requests++;
            qemu_iovec_reset(&io->iov);
            qemu_iovec_add(&io->iov, io->remainder, 0x200);

            m->aiocb = blk_aio_writev(s->blk, sector_num - 1, &io->iov, 1,
                                      pmac_ide_transfer_cb, io);
        }
    }

    if (s->nsector == 0 && !io->remainder_len) {
        MACIO_DPRINTF("end of transfer\n");
        s->status = READY_STAT | SEEK_STAT;
        ide_set_irq(s->bus);
        m->dma_active = false;
        io->remainder_len = 0;
    }

    if (io->len == 0) {
        MACIO_DPRINTF("end of DMA\n");
        goto done;
    }

    /* launch next transfer */

    s->io_buffer_index = 0;
    s->io_buffer_size = MIN(io->len, s->nsector * 512);

    /* handle unaligned accesses first, get them over with and only do the
       remaining bulk transfer using our async DMA helpers */
    unaligned = io->len & 0x1ff;
    if (unaligned) {
        int nsector = io->len >> 9;

        MACIO_DPRINTF("precopying unaligned %d bytes to %#" HWADDR_PRIx "\n",
                      unaligned, io->addr + io->len - unaligned);

        switch (s->dma_cmd) {
        case IDE_DMA_READ:
            io->requests++;
            io->finish_addr = io->addr + io->len - unaligned;
            io->finish_len = unaligned;
            io->finish_remain_read = true;
            qemu_iovec_reset(&io->iov);
            qemu_iovec_add(&io->iov, io->remainder, 0x200);

            m->aiocb = blk_aio_readv(s->blk, sector_num + nsector, &io->iov, 1,
                                     pmac_ide_transfer_cb, io);
            break;
        case IDE_DMA_WRITE:
            /* cache the contents in our io struct */
            cpu_physical_memory_read(io->addr + io->len - unaligned,
                                     io->remainder + io->remainder_len,
                                     unaligned);
            break;
        case IDE_DMA_TRIM:
            break;
        }
    }

    MACIO_DPRINTF("io->len = %#x\n", io->len);

    qemu_sglist_init(&s->sg, DEVICE(m), io->len / MACIO_PAGE_SIZE + 1,
                     &address_space_memory);
    qemu_sglist_add(&s->sg, io->addr, io->len);
    io->addr += io->len + unaligned;
    io->remainder_len = (0x200 - unaligned) & 0x1ff;
    MACIO_DPRINTF("set remainder to: %d\n", io->remainder_len);

    /* Only subsector reads happening */
    if (!io->len) {
        if (!io->requests) {
            io->requests++;
            pmac_ide_transfer_cb(opaque, ret);
        }
        return;
    }

    io->len = 0;

    MACIO_DPRINTF("sector_num=%" PRId64 " n=%d, nsector=%d, cmd_cmd=%d\n",
                  sector_num, n, s->nsector, s->dma_cmd);

    switch (s->dma_cmd) {
    case IDE_DMA_READ:
        m->aiocb = dma_blk_read(s->blk, &s->sg, sector_num,
                                pmac_ide_transfer_cb, io);
        break;
    case IDE_DMA_WRITE:
        m->aiocb = dma_blk_write(s->blk, &s->sg, sector_num,
                                 pmac_ide_transfer_cb, io);
        break;
    case IDE_DMA_TRIM:
        m->aiocb = dma_blk_io(s->blk, &s->sg, sector_num,
                              ide_issue_trim, pmac_ide_transfer_cb, io,
                              DMA_DIRECTION_TO_DEVICE);
        break;
    }

    io->requests++;
    return;

done:
    if (s->dma_cmd == IDE_DMA_READ || s->dma_cmd == IDE_DMA_WRITE) {
        block_acct_done(blk_get_stats(s->blk), &s->acct);
    }
    io->dma_end(io);
}

static void pmac_ide_transfer(DBDMA_io *io)
{
    MACIOIDEState *m = io->opaque;
    IDEState *s = idebus_active_if(&m->bus);

    MACIO_DPRINTF("\n");

    //s->io_buffer_size = 0;
    if (s->drive_kind == IDE_CD) {

        /* Handle non-block ATAPI DMA transfers */
        if (s->lba == -1 && 0 == 1) {
            s->io_buffer_size = MIN(io->len, s->packet_transfer_size);
            block_acct_start(blk_get_stats(s->blk), &s->acct, s->io_buffer_size,
                             BLOCK_ACCT_READ);
            MACIO_DPRINTF("non-block ATAPI DMA transfer size: %d\n",
                          s->io_buffer_size);

            /* Copy ATAPI buffer directly to RAM and finish */
            cpu_physical_memory_write(io->addr, s->io_buffer,
                                      s->io_buffer_size);
            ide_atapi_cmd_ok(s);
            m->dma_active = false;

            MACIO_DPRINTF("end of non-block ATAPI DMA transfer\n");
            block_acct_done(blk_get_stats(s->blk), &s->acct);
            io->dma_end(io);
            return;
        }

        //block_acct_start(blk_get_stats(s->blk), &s->acct, io->len,
        //                 BLOCK_ACCT_READ);
        
        MACIO_DPRINTF("#### do DMA transfer\n");
        pmac_ide_atapi_transfer_cb(io, 0);
        return;
    }  else {
        s->io_buffer_size = 0;
    }

    switch (s->dma_cmd) {
    case IDE_DMA_READ:
        block_acct_start(blk_get_stats(s->blk), &s->acct, io->len,
                         BLOCK_ACCT_READ);
        break;
    case IDE_DMA_WRITE:
        block_acct_start(blk_get_stats(s->blk), &s->acct, io->len,
                         BLOCK_ACCT_WRITE);
        break;
    default:
        break;
    }

    io->requests++;
    pmac_ide_transfer_cb(io, 0);
}

static void pmac_ide_flush(DBDMA_io *io)
{
    MACIOIDEState *m = io->opaque;

    if (m->aiocb) {
        blk_drain_all();
    }
}

/* PowerMac IDE memory IO */
static void pmac_ide_writeb (void *opaque,
                             hwaddr addr, uint32_t val)
{
    MACIOIDEState *d = opaque;

    addr = (addr & 0xFFF) >> 4;
    switch (addr) {
    case 1 ... 7:
        ide_ioport_write(&d->bus, addr, val);
        break;
    case 8:
    case 22:
        ide_cmd_write(&d->bus, 0, val);
        break;
    default:
        break;
    }
}

static uint32_t pmac_ide_readb (void *opaque,hwaddr addr)
{
    uint8_t retval;
    MACIOIDEState *d = opaque;

    addr = (addr & 0xFFF) >> 4;
    switch (addr) {
    case 1 ... 7:
        retval = ide_ioport_read(&d->bus, addr);
        break;
    case 8:
    case 22:
        retval = ide_status_read(&d->bus, 0);
        break;
    default:
        retval = 0xFF;
        break;
    }
    return retval;
}

static void pmac_ide_writew (void *opaque,
                             hwaddr addr, uint32_t val)
{
    MACIOIDEState *d = opaque;

    addr = (addr & 0xFFF) >> 4;
    val = bswap16(val);
    if (addr == 0) {
        ide_data_writew(&d->bus, 0, val);
    }
}

static uint32_t pmac_ide_readw (void *opaque,hwaddr addr)
{
    uint16_t retval;
    MACIOIDEState *d = opaque;

    addr = (addr & 0xFFF) >> 4;
    if (addr == 0) {
        retval = ide_data_readw(&d->bus, 0);
    } else {
        retval = 0xFFFF;
    }
    retval = bswap16(retval);
    return retval;
}

static void pmac_ide_writel (void *opaque,
                             hwaddr addr, uint32_t val)
{
    MACIOIDEState *d = opaque;

    addr = (addr & 0xFFF) >> 4;
    val = bswap32(val);
    if (addr == 0) {
        ide_data_writel(&d->bus, 0, val);
    }
}

static uint32_t pmac_ide_readl (void *opaque,hwaddr addr)
{
    uint32_t retval;
    MACIOIDEState *d = opaque;

    addr = (addr & 0xFFF) >> 4;
    if (addr == 0) {
        retval = ide_data_readl(&d->bus, 0);
    } else {
        retval = 0xFFFFFFFF;
    }
    retval = bswap32(retval);
    return retval;
}

static const MemoryRegionOps pmac_ide_ops = {
    .old_mmio = {
        .write = {
            pmac_ide_writeb,
            pmac_ide_writew,
            pmac_ide_writel,
        },
        .read = {
            pmac_ide_readb,
            pmac_ide_readw,
            pmac_ide_readl,
        },
    },
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static const VMStateDescription vmstate_pmac = {
    .name = "ide",
    .version_id = 3,
    .minimum_version_id = 0,
    .fields = (VMStateField[]) {
        VMSTATE_IDE_BUS(bus, MACIOIDEState),
        VMSTATE_IDE_DRIVES(bus.ifs, MACIOIDEState),
        VMSTATE_BOOL(dma_active, MACIOIDEState),
        VMSTATE_END_OF_LIST()
    }
};

static void macio_ide_reset(DeviceState *dev)
{
    MACIOIDEState *d = MACIO_IDE(dev);

    ide_bus_reset(&d->bus);
}

static int ide_nop_int(IDEDMA *dma, int x)
{
    return 0;
}

static int32_t ide_nop_int32(IDEDMA *dma, int x)
{
    return 0;
}

static void ide_nop_restart(void *opaque, int x, RunState y)
{
}

static void ide_dbdma_start(IDEDMA *dma, IDEState *s,
                            BlockCompletionFunc *cb)
{
    MACIOIDEState *m = container_of(dma, MACIOIDEState, dma);

    printf("buffer_size: %x   buffer_index: %x\n", s->io_buffer_size, s->io_buffer_index);
    printf("lba: %x    sector: %x\n", s->lba, s->packet_transfer_size);
    
    if (s->drive_kind == IDE_CD) {
        s->io_buffer_index = 0;
        s->io_buffer_size = s->packet_transfer_size;
    }
    
    MACIO_DPRINTF("\n");
    m->dma_active = true;
    DBDMA_kick(m->dbdma);
}

static const IDEDMAOps dbdma_ops = {
    .start_dma      = ide_dbdma_start,
    .prepare_buf    = ide_nop_int32,
    .rw_buf         = ide_nop_int,
    .set_unit       = ide_nop_int,
    .restart_cb     = ide_nop_restart,
};

static void macio_ide_realizefn(DeviceState *dev, Error **errp)
{
    MACIOIDEState *s = MACIO_IDE(dev);

    ide_init2(&s->bus, s->irq);

    /* Register DMA callbacks */
    s->dma.ops = &dbdma_ops;
    s->bus.dma = &s->dma;
}

static void macio_ide_initfn(Object *obj)
{
    SysBusDevice *d = SYS_BUS_DEVICE(obj);
    MACIOIDEState *s = MACIO_IDE(obj);

    ide_bus_new(&s->bus, sizeof(s->bus), DEVICE(obj), 0, 2);
    memory_region_init_io(&s->mem, obj, &pmac_ide_ops, s, "pmac-ide", 0x1000);
    sysbus_init_mmio(d, &s->mem);
    sysbus_init_irq(d, &s->irq);
    sysbus_init_irq(d, &s->dma_irq);
}

static void macio_ide_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = macio_ide_realizefn;
    dc->reset = macio_ide_reset;
    dc->vmsd = &vmstate_pmac;
}

static const TypeInfo macio_ide_type_info = {
    .name = TYPE_MACIO_IDE,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MACIOIDEState),
    .instance_init = macio_ide_initfn,
    .class_init = macio_ide_class_init,
};

static void macio_ide_register_types(void)
{
    type_register_static(&macio_ide_type_info);
}

/* hd_table must contain 2 block drivers */
void macio_ide_init_drives(MACIOIDEState *s, DriveInfo **hd_table)
{
    int i;

    for (i = 0; i < 2; i++) {
        if (hd_table[i]) {
            ide_create_drive(&s->bus, i, hd_table[i]);
        }
    }
}

void macio_ide_register_dma(MACIOIDEState *s, void *dbdma, int channel)
{
    s->dbdma = dbdma;
    DBDMA_register_channel(dbdma, channel, s->dma_irq,
                           pmac_ide_transfer, pmac_ide_flush, s);
}

type_init(macio_ide_register_types)
