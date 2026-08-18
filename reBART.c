// SPDX-License-Identifier: MIT
// Copyright (c) 2026 roschank
//
// Attribution: ReBAR capability register numbers are facts from the PCIE
// base specification. The ReBarState NVRAM variable name/GUID is used for interop
// with xCuri0/ReBarUEFI(MIT). The Turing BAR-size strap method is re-implemented
// from terminatorul/NvStrapsReBar(MIT). Built as an EDK2 driver (BSD-2-Clause-Patent)
//
// reBART - Resizable BAR support for Intel 8 series chipset (Lynx Point/Haswell) motherboards

#include <PiDxe.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/IoLib.h>
#include <Library/DebugLib.h>
#include <Library/BaseLib.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/S3SaveState.h>


static UINT64 g_ceil = 0x1000000000ULL;
#define CEIL        g_ceil
#define HOST_BDF    0
#define TOUUD_REG   0xA8
#define NV_VENDOR   0x10DE

// RTC older than this = CMOS cleared skip bump on release
#define FW_YEAR     2026

#define BDF(b, d, f)    (((UINT32)(b) << 20) | ((UINT32)(d) << 15) | ((UINT32)(f) << 12))

// PCIE resizable BAR capability
#define REBAR_ID        0x15
#define REBAR_CAP       4
#define REBAR_CTRL      8
#define REBAR_BAR_IDX   0x7
#define REBAR_NBAR_MASK 0xE0
#define REBAR_NBAR_SH   5
#define REBAR_SIZE_MASK 0x1F00
#define REBAR_SIZE_SH   8
#define EXTCAP_BASE     0x100

#define PCI_CMD         0x04
#define CMD_MEM         0x02


// the turing BAR1 size straps
#define STRAPS_OFF      0x101000
#define STRAP0          0x00
#define STRAP1          0x0C
#define BAR1_P1_SH      14
#define BAR1_P1_MASK    (0x3 << BAR1_P1_SH)
#define BAR1_P2_SH      20
#define BAR1_P2_MASK    (0x7 << BAR1_P2_SH)
#define STRAP_COMMIT    (0x1u << 31)


// ReBarUEFI's ReBarState name/GUID so ReBarState sets it for us too
static CHAR16    g_rebar_var_name[] = L"ReBarState";
static EFI_GUID g_rebar_guid = {0xa3c5b77a, 0xc88f, 0x4a93, {0xbf, 0x1c, 0x4a, 0x92, 0xa3, 0x2c, 0x65, 0xce}};

static EFI_GUID g_ready_to_boot_guid = {0x7ce88fb3, 0x4bd7, 0x4679, {0x87, 0xa8, 0xa8, 0xd8, 0xde, 0xe5, 0x0d, 0x2b}};
static EFI_GUID g_gop_guid = {0x9042a9de, 0x23dc, 0x4a38, {0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a}};

static UINT64    g_ecam = 0;
static BOOLEAN   g_ran_already = FALSE;
static UINT8     g_want_n = 0;
static UINT8     g_want_orig = 0;
static UINT8     g_got_n = 0;
static UINT64    g_old_vbar = 0;
static UINT64    g_new_vbar = 0;

static UINT64    g_turing_bar0 = 0;
static UINT32    g_turing_s0 = 0, g_turing_s1 = 0;
static BOOLEAN   g_turing_strapped = FALSE;

// config space through ECAM and plain MMIO
#define CFG32(off)  (*(volatile UINT32 *)(UINTN)(g_ecam + (UINT64)(off)))
#define CFG16(off)  (*(volatile UINT16 *)(UINTN)(g_ecam + (UINT64)(off)))
#define CFG64(off)  (*(volatile UINT64 *)(UINTN)(g_ecam + (UINT64)(off)))
#define MM32(addr)  (*(volatile UINT32 *)(UINTN)(addr))

static UINT16 rtc_year(void)
{
    EFI_TIME t;

    if (EFI_ERROR(gRT->GetTime(&t, NULL))) return FW_YEAR;

    return t.Year;
}

static BOOLEAN resolve_ecam(void)
{
    UINT32 lo, hi;
    UINT64 pex, mask;

    IoWrite32(0xCF8, 0x80000000 | 0x60);  lo = IoRead32(0xCFC);
    IoWrite32(0xCF8, 0x80000000 | 0x64);  hi = IoRead32(0xCFC);

    if (!(lo & 1)) return FALSE;

    pex = ((UINT64)hi << 32) | lo;

    switch ((pex >> 1) & 3) {   // size: 0=256M 1=128M 2/3=64M
    case 0:  mask = ~0xFFFFFFFULL;  break;
    case 1:  mask = ~0x7FFFFFFULL;  break;
    default: mask = ~0x3FFFFFFULL;  break;
    }

    g_ecam = pex & mask & 0x7FFFFFFFFFULL;

    if (!g_ecam) return FALSE;
    IoWrite32(0xCF8, 0x80000000);

    if (CFG32(0) != IoRead32(0xCFC)) {
        g_ecam = 0;
        return FALSE;
    }
    return TRUE;
}

// grab the first display class card bus 0 root port
static BOOLEAN find_discrete_gpu(UINT32 *gpu_out, UINT32 *bridge_out)
{
    UINT32 dev, fn, nfn, bridge, sec, gpu;

    for (dev = 0; dev < 32; dev++) {
        if (CFG32(BDF(0, dev, 0)) == 0xFFFFFFFF) continue;

        nfn = (CFG32(BDF(0, dev, 0) + 0x0C) >> 16) & 0x80 ? 8 : 1;

        for (fn = 0; fn < nfn; fn++) {
            bridge = BDF(0, dev, fn);

            if (CFG32(bridge) == 0xFFFFFFFF) continue;
            if (((CFG32(bridge + 0x0C) >> 16) & 0x7F) != 1) continue; // not a bridge

            sec = (CFG32(bridge + 0x18) >> 8) & 0xFF; // secondary bus
            if (!sec) continue;

            gpu = BDF(sec, 0, 0);

            if (CFG32(gpu) == 0xFFFFFFFF) continue;
            if (((CFG32(gpu + 0x08) >> 24) & 0xFF) != 0x03) continue;  // class 0x03 = display

            *gpu_out = gpu;
            *bridge_out = bridge;
            return TRUE;
        }
    }
    return FALSE;
}

static UINT32 find_rebar_cap(UINT32 bdf)
{
    UINT32 off = EXTCAP_BASE, hdr;
    int hops;

    for (hops = 0; hops < 64; hops++) {
        if (off < EXTCAP_BASE || off >= 0x1000) break;

        hdr = CFG32(bdf + off);

        if (hdr == 0 || hdr == 0xFFFFFFFF) break;
        if ((hdr & 0xFFFF) == REBAR_ID) return off;

        off = (hdr >> 20) & 0xFFF;
    }
    return 0;
}

static BOOLEAN is_known_turing(UINT32 ven_dev)
{
    UINT16 dev = ven_dev >> 16;

    if ((ven_dev & 0xFFFF) != NV_VENDOR) return FALSE;

    return (dev >= 0x1E00 && dev <= 0x1FFF) || (dev >= 0x2180 && dev <= 0x21FF);
}

static BOOLEAN bump_turing_straps(UINT32 gpu, UINT64 touud)
{
    UINT32 bar0_lo = CFG32(gpu + 0x10);
    UINT32 s0, s1, v0, v1, sav_lo, sav_hi, code;
    UINT64 bar0, mask, size, mb;
    UINT16 cmd;

    if (!is_known_turing(CFG32(gpu))) return FALSE;

    if ((bar0_lo & 1) || !(bar0_lo & 0xFFFFFFF0)) return FALSE;

    bar0 = bar0_lo & 0xFFFFFFF0;

    cmd = CFG16(gpu + PCI_CMD);
    CFG16(gpu + PCI_CMD) = cmd | CMD_MEM;

    s0 = MM32(bar0 + STRAPS_OFF + STRAP0);
    s1 = MM32(bar0 + STRAPS_OFF + STRAP1);

    if (s0 == 0xFFFFFFFF || s1 == 0xFFFFFFFF) {
        CFG16(gpu + PCI_CMD) = cmd;
        return FALSE;
    }

    v0 = (s0 & ~BAR1_P1_MASK) | (3 << BAR1_P1_SH) | STRAP_COMMIT;
    v1 = (s1 & ~BAR1_P2_MASK) | (7 << BAR1_P2_SH) | STRAP_COMMIT;

    MM32(bar0 + STRAPS_OFF + STRAP0) = v0;
    MM32(bar0 + STRAPS_OFF + STRAP1) = v1;

    CFG16(gpu + PCI_CMD) = cmd & ~CMD_MEM;
    sav_lo = CFG32(gpu + 0x14);
    sav_hi = CFG32(gpu + 0x18);

    CFG32(gpu + 0x14) = 0xFFFFFFFF;
    CFG32(gpu + 0x18) = 0xFFFFFFFF;

    mask = ((UINT64)CFG32(gpu + 0x18) << 32) | (CFG32(gpu + 0x14) & 0xFFFFFFF0);
    size = ~mask + 1;

    CFG32(gpu + 0x14) = sav_lo;
    CFG32(gpu + 0x18) = sav_hi;

    if (size <= 0x10000000ULL || size >= CEIL || CEIL - size < touud) {
        CFG16(gpu + PCI_CMD) = cmd | CMD_MEM;
        MM32(bar0 + STRAPS_OFF + STRAP0) = s0;
        MM32(bar0 + STRAPS_OFF + STRAP1) = s1;
        CFG16(gpu + PCI_CMD) = cmd;
        return FALSE;
    }

    CFG16(gpu + PCI_CMD) = cmd | CMD_MEM; //it took leave decode on

    mb = size >> 20;

    for (code = 0; mb > 1; mb >>= 1)
        code++;

    g_got_n = code;
    g_turing_bar0 = bar0;
    g_turing_s0 = v0;
    g_turing_s1 = v1;
    g_turing_strapped = TRUE;

    return TRUE;
}

static void resolve_ceil(void)
{
    UINT32 max_ext = 0, pa = 0;

    AsmCpuid(0x80000000, &max_ext, NULL, NULL, NULL);

    if (max_ext >= 0x80000008) {
        AsmCpuid(0x80000008, &pa, NULL, NULL, NULL);
        pa &= 0xFF;
        if (pa < 36) pa = 36;
        if (pa > 39) pa = 39;
        g_ceil = 1ULL << pa;
    }
}

static void do_resize(void)
{
    UINT32 gpu, bridge, cap, vbar = 0, b;
    UINT64 touud, top, cur, win_lo;
    UINT16 cmd;

    if (!resolve_ecam()) return;

    resolve_ceil(); // ceil from CPUID MAXPHYADDR

    if (!find_discrete_gpu(&gpu, &bridge)) return; // no discrete gpu nothing to do

    touud = CFG64(HOST_BDF + TOUUD_REG) & 0x7FFFFFFFFFULL;
    cap = find_rebar_cap(gpu);

    if (cap) {
        UINT32 nbars = (CFG32(gpu + cap + REBAR_CTRL) & REBAR_NBAR_MASK) >> REBAR_NBAR_SH;
        UINT32 ctrl = 0, cap_reg = 0, idx = 0xFF, best_bit = 0, sizes, n, i;

        if (nbars == 0 || nbars > 6) nbars = 1;

        // keep the resizable bar with the biggest max
        for (i = 0; i < nbars; i++) {
            UINT32 c  = CFG32(gpu + cap + REBAR_CTRL + i * 8);
            UINT32 cp = CFG32(gpu + cap + REBAR_CAP + i * 8);
            UINT32 sz = (cp & 0x00FFFFF0) >> 4, hi;

            if (!sz) continue;

            hi = 19;
            while (hi > 0 && !((sz >> hi) & 1))
                hi--;

            if (!ctrl || hi > best_bit) {
                best_bit = hi;
                ctrl = cap + REBAR_CTRL + i * 8;
                cap_reg = cp;
                idx = c & REBAR_BAR_IDX;
            }
        }

        if (!ctrl || idx > 5) return;

        vbar = 0x10 + idx * 4;

        if ((CFG32(gpu + vbar) & 0xF) != 0xC) return; // must be 64 bit prefetchable

        g_old_vbar = ((UINT64)CFG32(gpu + vbar + 4) << 32) | (CFG32(gpu + vbar) & 0xFFFFFFF0);

        sizes = (cap_reg & 0x00FFFFF0) >> 4;
        n = (g_want_n == 0xFF) ? 19 : g_want_n;

        if (n > 19) n = 19;

        while (n > 0) {  // top anchored has to clear TOUUD
            UINT64 szb = 1ULL << (n + 20);
            if (szb < CEIL && CEIL - szb >= touud) break;
            n--;
        }

        while (n > 0 && !((sizes >> n) & 1))
            n--;
        if (!n) return;

        g_got_n = n;
        top = CEIL - (1ULL << (n + 20));

        cmd = CFG16(gpu + PCI_CMD);
        CFG16(gpu + PCI_CMD) = cmd & ~CMD_MEM;
        CFG32(gpu + ctrl) = (CFG32(gpu + ctrl) & ~REBAR_SIZE_MASK) | ((n << REBAR_SIZE_SH) & REBAR_SIZE_MASK);
    }
    else {
        if (!bump_turing_straps(gpu, touud)) return;

        vbar = 0x14;
        if ((CFG32(gpu + vbar) & 0xF) != 0xC) return;

        //low nibble of a 64 bit pref bar is 0xC mask it off
        g_old_vbar = ((UINT64)CFG32(gpu + vbar + 4) << 32) | (CFG32(gpu + vbar) & 0xFFFFFFF0);
        top = CEIL - (1ULL << (g_got_n + 20));
        cmd = CFG16(gpu + PCI_CMD);
        CFG16(gpu + PCI_CMD) = cmd & ~CMD_MEM;
    }

    g_new_vbar = top;
    CFG32(gpu + vbar)     = (UINT32)((top & 0xFFFFFFF0ULL) | 0xC);
    CFG32(gpu + vbar + 4) = (UINT32)(top >> 32);

    cur = top;
    win_lo = top;

    for (b = 0; b < 6;) {
        UINT32 off = 0x10 + b * 4;
        UINT32 lo = CFG32(gpu + off);
        BOOLEAN is_64 = !(lo & 1) && (lo & 6) == 4;

        if ((lo & 0xF) == 0xC && off != vbar) {
            UINT32 sav_lo = lo, sav_hi = CFG32(gpu + off + 4);
            UINT64 mask, size;
            BOOLEAN placed = FALSE;

            CFG32(gpu + off) = 0xFFFFFFFF;
            CFG32(gpu + off + 4) = 0xFFFFFFFF;
            mask = ((UINT64)CFG32(gpu + off + 4) << 32) | (CFG32(gpu + off) & 0xFFFFFFF0);
            size = ~mask + 1;

            if (size && size < cur) {
                UINT64 base = (cur - size) & ~(size - 1);	// align down under cur
                if (base >= touud && base < cur) {
                    CFG32(gpu + off)     = (UINT32)((base & 0xFFFFFFF0ULL) | 0xC);
                    CFG32(gpu + off + 4) = (UINT32)(base >> 32);
                    cur = base;
                    win_lo = base;
                    placed = TRUE;
                }
            }
            if (!placed) {	// doesn't fit up high leave it where it was
                CFG32(gpu + off) = sav_lo;
                CFG32(gpu + off + 4) = sav_hi;
            }
        }
        b += is_64 ? 2 : 1;
    }

    // open the PEG bridge prefetch window from win_lo up to 64GB-1
    CFG16(bridge + 0x24) = ((win_lo >> 16) & 0xFFF0) | 1;
    CFG16(bridge + 0x26) = (((CEIL - 1) >> 16) & 0xFFF0) | 1;
    CFG32(bridge + 0x28) = win_lo >> 32;
    CFG32(bridge + 0x2C) = (CEIL - 1) >> 32;

    // decode back on and make sure the bridge forwards memory
    CFG16(gpu + PCI_CMD) = cmd | CMD_MEM;
    CFG16(bridge + PCI_CMD) = CFG16(bridge + PCI_CMD) | CMD_MEM;
}

static void EFIAPI on_ready_to_boot(EFI_EVENT evt, void *ctx)
{
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    UINT64 old_fb;

    if (g_ran_already) return;
    g_ran_already = TRUE;

    // no GOP CSM/legacy boot nothing to do
    if (EFI_ERROR(gBS->LocateProtocol(&g_gop_guid, NULL, (void **)&gop)) || !gop || !gop->Mode) {
        gBS->CloseEvent(evt);
        return;
    }
    old_fb = gop->Mode->FrameBufferBase;

    do_resize();

    if (g_got_n && g_new_vbar) {
        UINT64 wb_size = 1ULL << (g_got_n + 20);
        gDS->AddMemorySpace(EfiGcdMemoryTypeMemoryMappedIo, g_new_vbar, wb_size, EFI_MEMORY_WC | EFI_MEMORY_UC);
        gDS->SetMemorySpaceAttributes(g_new_vbar, wb_size, EFI_MEMORY_WC);
    }

    if (g_got_n && old_fb >= g_old_vbar && old_fb - g_old_vbar < 0x100000000ULL) gop->Mode->FrameBufferBase = g_new_vbar + (old_fb - g_old_vbar);

    // write back the size we ended up with if it changed
    if (g_got_n && g_got_n != g_want_orig)
        gRT->SetVariable(g_rebar_var_name, &g_rebar_guid, EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS, sizeof(g_got_n), &g_got_n);

    if (g_turing_strapped) {
        EFI_S3_SAVE_STATE_PROTOCOL *s3 = NULL;
        if (!EFI_ERROR(gBS->LocateProtocol(&gEfiS3SaveStateProtocolGuid, NULL, (void **)&s3)) && s3) {
            s3->Write(s3, EFI_BOOT_SCRIPT_MEM_WRITE_OPCODE, EfiBootScriptWidthUint32, (UINT64)(g_turing_bar0 + STRAPS_OFF + STRAP0), (UINTN)1, &g_turing_s0);
            s3->Write(s3, EFI_BOOT_SCRIPT_MEM_WRITE_OPCODE, EfiBootScriptWidthUint32, (UINT64)(g_turing_bar0 + STRAPS_OFF + STRAP1), (UINTN)1, &g_turing_s1);
        }
    }

    gBS->CloseEvent(evt);
}

EFI_STATUS EFIAPI rebart_entry(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *system_table)
{
    EFI_EVENT evt;
    EFI_STATUS status;
    UINT8 state = 0;
    UINTN sz = sizeof(state);
    UINT32 attrs = 0;

    status = gRT->GetVariable(g_rebar_var_name, &g_rebar_guid, &attrs, &sz, &state);

    if (EFI_ERROR(status)) state = 0;
    g_want_orig = state;

    // RTC older than the build CMOS probably got cleared sit this one out
    if (state && rtc_year() < FW_YEAR) {
        state = 0;
    }

    if (!state) return EFI_SUCCESS;

    g_want_n = state; // 0xFF = card max else 2^N MB
    gBS->CreateEventEx(EVT_NOTIFY_SIGNAL, TPL_CALLBACK, on_ready_to_boot, NULL, &g_ready_to_boot_guid, &evt);

    return EFI_SUCCESS;
}
