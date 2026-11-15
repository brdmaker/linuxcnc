//
//    hm2_soc_ol: HostMot2 low-level driver for FPGAs inside a SoC (Xilinx Zynq, Altera SoCFPGA),
//    accessed through a Linux UIO device. Ported to LinuxCNC 2.9 (uspace) for the EBAZ4205.
//
//    Based on machinekit-hal hm2_soc_ol.c by Michael Brown, Michael Haberler and Devin Hughes,
//    which was derived from hostmot2 code by Sebastian Kuzminsky.
//
//    This program is free software; you can redistribute it and/or modify
//    it under the terms of the GNU General Public License as published by
//    the Free Software Foundation; either version 2 of the License, or
//    (at your option) any later version.
//
//    Differences from the machinekit driver:
//      - LinuxCNC module parameters instead of machinekit instances (newinst).
//      - The FPGA must already be programmed (e.g. by U-Boot "fpga loadb"); mainline Linux has no
//        configfs device tree overlay interface, so overlay-based programming is not supported.
//      - Connector count and width are taken from the firmware IDROM.
//      - No UIO interrupt support (LinuxCNC 2.9 hostmot2 has no irq hook in the llio).
//
//    Device tree node (e.g. hm2 AXI slave at 0x43C00000, 64 KiB):
//        hm2-socfpga0@43c00000 {
//            compatible = "generic-uio";
//            reg = <0x43c00000 0x10000>;
//        };
//    and boot with  uio_pdrv_genirq.of_id=generic-uio  (CONFIG_UIO_PDRV_GENIRQ).
//
//    Usage:  loadrt hostmot2
//            loadrt hm2_soc_ol config="num_encoders=1 num_stepgens=4" uio_name=hm2-socfpga0
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#include <rtapi.h>
#include <rtapi_app.h>
#include <rtapi_string.h>
#include "hal.h"

#define HM2_LLIO_NAME "hm2_soc_ol"
#include "hostmot2-lowlevel.h"
#include "hostmot2.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Michael Brown, Michael Haberler, Devin Hughes; LinuxCNC port for EBAZ4205");
MODULE_DESCRIPTION("Low level driver for HostMot2 in SoC FPGAs via UIO");
MODULE_SUPPORTED_DEVICE("Mesa-HostMot2-SoC");

#define HM2_SOC_MAX_BOARDS 2
#define HM2_SOC_MAX_UIO 32

static char *config[HM2_SOC_MAX_BOARDS];
RTAPI_MP_ARRAY_STRING(config, HM2_SOC_MAX_BOARDS, "config string for each board (see hostmot2(9))");
static char *uio_name[HM2_SOC_MAX_BOARDS] = { "hm2-socfpga0", NULL };
RTAPI_MP_ARRAY_STRING(uio_name, HM2_SOC_MAX_BOARDS, "UIO device name (device tree node name) for each board");
static int num_leds[HM2_SOC_MAX_BOARDS] = { 0, 0 };
RTAPI_MP_ARRAY_INT(num_leds, HM2_SOC_MAX_BOARDS, "number of LEDs the firmware drives on each board");
static int debug = 0;
RTAPI_MP_INT(debug, "print extra debug output");

typedef struct {
    hm2_lowlevel_io_t llio;
    int uio_fd;
    volatile rtapi_u32 *base;
    size_t len;
    char uio_dev[32];
    char connector_names[ANYIO_MAX_IOPORT_CONNECTORS][8];
} hm2_soc_t;

static hm2_soc_t boards[HM2_SOC_MAX_BOARDS];
static int num_boards = 0;
static int comp_id;

static int hm2_soc_read(hm2_lowlevel_io_t *this, rtapi_u32 addr, void *buffer, int size) {
    hm2_soc_t *brd = this->private;
    if (addr + size > brd->len) {
        THIS_ERR("read beyond mapped area: addr %08x size %d\n", addr, size);
        *this->io_error = 1;
        return 0;
    }
    // All HostMot2 accesses should be 32-bit and aligned; hm2_read_idrom does one 16-bit read.
    if ((addr & 3) || (size & 3)) {
        if (!(addr & 1) && size == 2) {
            *(rtapi_u16 *)buffer = *(volatile rtapi_u16 *)((volatile char *)brd->base + addr);
            return 1;
        }
        THIS_ERR("unaligned read: addr %08x size %d\n", addr, size);
        memcpy(buffer, (char *)brd->base + addr, size);
        return 1;
    }
    rtapi_u32 *dst = buffer;
    volatile rtapi_u32 *src = brd->base + addr / 4;
    for (int i = 0; i < size / 4; i++) dst[i] = src[i];
    return 1;
}

static int hm2_soc_write(hm2_lowlevel_io_t *this, rtapi_u32 addr, const void *buffer, int size) {
    hm2_soc_t *brd = this->private;
    if (addr + size > brd->len) {
        THIS_ERR("write beyond mapped area: addr %08x size %d\n", addr, size);
        *this->io_error = 1;
        return 0;
    }
    if ((addr & 3) || (size & 3)) {
        THIS_ERR("unaligned write: addr %08x size %d\n", addr, size);
        memcpy((char *)brd->base + addr, buffer, size);
        return 1;
    }
    const rtapi_u32 *src = buffer;
    volatile rtapi_u32 *dst = brd->base + addr / 4;
    for (int i = 0; i < size / 4; i++) dst[i] = src[i];
    return 1;
}

static int hm2_soc_reset(hm2_lowlevel_io_t *this) {
    (void)this;
    return 0;
}

static int read_sysfs(const char *path, char *buf, size_t len) {
    FILE *f = fopen(path, "r");
    if (!f) return -errno;
    if (!fgets(buf, len, f)) { fclose(f); return -EIO; }
    fclose(f);
    buf[strcspn(buf, "\n")] = 0;
    return 0;
}

// find /dev/uioN whose sysfs name matches, and its map0 size
static int locate_uio(const char *name, char *dev, size_t devlen, size_t *size) {
    char path[96], buf[64];
    for (int i = 0; i < HM2_SOC_MAX_UIO; i++) {
        snprintf(path, sizeof(path), "/sys/class/uio/uio%d/name", i);
        if (read_sysfs(path, buf, sizeof(buf)) < 0) continue;
        // uio_pdrv_genirq names the device after the DT node, with or without the unit address
        size_t n = strlen(name);
        if (strncmp(buf, name, n) != 0 || (buf[n] != 0 && buf[n] != '@')) continue;
        snprintf(path, sizeof(path), "/sys/class/uio/uio%d/maps/map0/size", i);
        if (read_sysfs(path, buf, sizeof(buf)) < 0) return -ENOENT;
        *size = strtoul(buf, NULL, 0);
        snprintf(dev, devlen, "/dev/uio%d", i);
        return 0;
    }
    return -ENODEV;
}

static void hm2_soc_cleanup(hm2_soc_t *brd) {
    if (brd->base) munmap((void *)brd->base, brd->len);
    brd->base = NULL;
    if (brd->uio_fd >= 0) close(brd->uio_fd);
    brd->uio_fd = -1;
}

static int hm2_soc_probe(int n) {
    hm2_soc_t *brd = &boards[n];
    hm2_lowlevel_io_t *this = &brd->llio;
    int r;

    memset(brd, 0, sizeof(*brd));
    brd->uio_fd = -1;
    r = locate_uio(uio_name[n], brd->uio_dev, sizeof(brd->uio_dev), &brd->len);
    if (r < 0) {
        LL_ERR("no UIO device named '%s' (check device tree and uio_pdrv_genirq.of_id)\n", uio_name[n]);
        return r;
    }
    if (brd->len < 0x1000) {
        LL_ERR("%s: map0 size %zu too small\n", brd->uio_dev, brd->len);
        return -EINVAL;
    }
    brd->uio_fd = open(brd->uio_dev, O_RDWR | O_SYNC);
    if (brd->uio_fd < 0) {
        LL_ERR("cannot open %s: %s\n", brd->uio_dev, strerror(errno));
        return -errno;
    }
    void *p = mmap(NULL, brd->len, PROT_READ | PROT_WRITE, MAP_SHARED, brd->uio_fd, 0);
    if (p == MAP_FAILED) {
        LL_ERR("mmap %s failed: %s\n", brd->uio_dev, strerror(errno));
        hm2_soc_cleanup(brd);
        return -errno;
    }
    brd->base = p;

    rtapi_u32 cookie = brd->base[HM2_ADDR_IOCOOKIE / 4];
    if (cookie != HM2_IOCOOKIE) {
        LL_ERR("%s: invalid IO cookie %08x (expected %08x); is the FPGA programmed?\n",
               brd->uio_dev, cookie, HM2_IOCOOKIE);
        hm2_soc_cleanup(brd);
        return -ENODEV;
    }

    // connector layout from the IDROM, so any HostMot2 firmware for this SoC works
    rtapi_u32 idrom_addr = brd->base[HM2_ADDR_IDROM_OFFSET / 4];
    if (idrom_addr + sizeof(hm2_idrom_t) > brd->len || (idrom_addr & 3)) {
        LL_ERR("%s: bad IDROM offset %08x\n", brd->uio_dev, idrom_addr);
        hm2_soc_cleanup(brd);
        return -EINVAL;
    }
    hm2_idrom_t idrom;
    rtapi_u32 *dst = (rtapi_u32 *)&idrom;
    for (size_t i = 0; i < sizeof(idrom) / 4; i++) dst[i] = brd->base[idrom_addr / 4 + i];
    if (idrom.io_ports < 1 || idrom.io_ports > ANYIO_MAX_IOPORT_CONNECTORS) {
        LL_ERR("%s: IDROM reports %d IO ports\n", brd->uio_dev, idrom.io_ports);
        hm2_soc_cleanup(brd);
        return -EINVAL;
    }

    this->num_ioport_connectors = idrom.io_ports;
    this->pins_per_connector = idrom.port_width;
    for (int i = 0; i < idrom.io_ports; i++) {
        snprintf(brd->connector_names[i], sizeof(brd->connector_names[i]), "P%d", i + 1);
        this->ioport_connector_name[i] = brd->connector_names[i];
    }
    this->num_leds = num_leds[n];
    this->fpga_part_number = NULL;       // no programming from LinuxCNC
    this->comp_id = comp_id;
    this->read = hm2_soc_read;
    this->write = hm2_soc_write;
    this->reset = hm2_soc_reset;
    this->threadsafe = 1;
    this->private = brd;
    rtapi_snprintf(this->name, sizeof(this->name), "hm2_%.4s.%d", (char *)idrom.board_name + 4, n);
    for (char *c = this->name; *c; c++) if (*c >= 'A' && *c <= 'Z') *c += 'a' - 'A';

    if (debug)
        LL_PRINT("%s: %zu bytes, board '%.8s', %d ports x %d pins, clock %u Hz\n", brd->uio_dev, brd->len,
                 (char *)idrom.board_name, idrom.io_ports, idrom.port_width, idrom.clock_low);

    r = hm2_register(this, config[n] ? config[n] : "");
    if (r != 0) {
        LL_ERR("hm2_register failed for %s: %d\n", brd->uio_dev, r);
        hm2_soc_cleanup(brd);
        return r;
    }
    LL_PRINT("initialized %s on %s\n", this->name, brd->uio_dev);
    return 0;
}

int rtapi_app_main(void) {
    comp_id = hal_init(HM2_LLIO_NAME);
    if (comp_id < 0) return comp_id;
    for (int n = 0; n < HM2_SOC_MAX_BOARDS; n++) {
        if (!uio_name[n] || !uio_name[n][0]) continue;
        int r = hm2_soc_probe(n);
        if (r) {
            for (int i = 0; i < num_boards; i++) { hm2_unregister(&boards[i].llio); hm2_soc_cleanup(&boards[i]); }
            hal_exit(comp_id);
            return r;
        }
        num_boards++;
    }
    if (num_boards == 0) {
        LL_ERR("no boards configured\n");
        hal_exit(comp_id);
        return -ENODEV;
    }
    hal_ready(comp_id);
    return 0;
}

void rtapi_app_exit(void) {
    for (int i = 0; i < num_boards; i++) {
        hm2_unregister(&boards[i].llio);
        hm2_soc_cleanup(&boards[i]);
    }
    hal_exit(comp_id);
}
