/** @file
  Decompresses an LZMA-packed ("KLZM") FAT32 initrd from LittleKernel's
  0x82000000..0x83000000 16 MiB window into high DRAM (0xA0000000) and
  updates /chosen/linux,initrd-{start,end} in-place before UniXNURamDiskDxe.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <PiDxe.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/FdtLib.h>
#include <Library/HobLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/TimerLib.h>

#define KENZO_KLZM_MAGIC          0x4D5A4C4BU  // "KLZM"
#define KENZO_HIGH_INITRD_BASE    0xA0000000ULL
#define KENZO_HIGH_INITRD_MAX     0x10000000U  // 256 MiB protected window

// MSM8956 GCC SDCC1 (eMMC @ 0x07824900) and SDCC2 (microSD @ 0x07864900) clocks
// Verified directly against Kenzo stock Linux kernel (gcc-msm8976).
#define GCC_SDCC1_APPS_CMD_RCGR   0x01842004ULL
#define GCC_SDCC1_APPS_CFG_RCGR   0x01842008ULL
#define GCC_SDCC1_APPS_CBCR       0x01842018ULL
#define GCC_SDCC1_AHB_CBCR        0x0184201CULL
#define GCC_SDCC1_ICE_CORE_CBCR   0x0185D014ULL

#define GCC_SDCC2_APPS_CMD_RCGR   0x01843004ULL
#define GCC_SDCC2_APPS_CFG_RCGR   0x01843008ULL
#define GCC_SDCC2_APPS_CBCR       0x01843018ULL
#define GCC_SDCC2_AHB_CBCR        0x0184301CULL

#define SDCC1_CORE_BASE           0x07824000ULL
#define SDCC1_HC_BASE             0x07824900ULL
#define SDCC2_CORE_BASE           0x07864000ULL
#define SDCC2_HC_BASE             0x07864900ULL

STATIC
VOID
EnableGccBranch (
  IN UINT64  CbcrAddr
  )
{
  UINT32  Val;
  UINTN   Spin;

  Val = MmioRead32 (CbcrAddr);
  if ((Val & BIT0) == 0) {
    MmioWrite32 (CbcrAddr, Val | BIT0);
  }

  for (Spin = 0; Spin < 50000; Spin++) {
    if ((MmioRead32 (CbcrAddr) & BIT31) == 0) {
      break;
    }
  }
}

STATIC
VOID
EnableGccSdcc (
  IN CONST CHAR8  *Name,
  IN UINT64       CmdRcgr,
  IN UINT64       CfgRcgr,
  IN UINT64       AppsCbcr,
  IN UINT64       AhbCbcr
  )
{
  UINT32  AhbBefore;
  UINT32  AppsBefore;
  UINTN   Spin;

  AhbBefore  = MmioRead32 (AhbCbcr);
  AppsBefore = MmioRead32 (AppsCbcr);

  EnableGccBranch (AhbCbcr);
  EnableGccBranch (AppsCbcr);

  // If APPS branch still reports CLK_OFF, switch RCG cleanly to 19.2 MHz XO (src=0, div=0).
  if ((MmioRead32 (AppsCbcr) & BIT31) != 0) {
    MmioWrite32 (CfgRcgr, 0);
    MmioWrite32 (CmdRcgr, MmioRead32 (CmdRcgr) | BIT0);
    for (Spin = 0; Spin < 50000; Spin++) {
      if ((MmioRead32 (CmdRcgr) & BIT0) == 0) {
        break;
      }
    }
    EnableGccBranch (AppsCbcr);
  }

  DEBUG ((
    DEBUG_ERROR,
    "KENZO_SDCC %a: AHB %08x->%08x APPS %08x->%08x RCG=%08x/%08x\n",
    Name,
    AhbBefore,
    MmioRead32 (AhbCbcr),
    AppsBefore,
    MmioRead32 (AppsCbcr),
    MmioRead32 (CmdRcgr),
    MmioRead32 (CfgRcgr)
    ));
}

STATIC
BOOLEAN
SdhciSimpleCmd (
  IN  UINT64  Hc,
  IN  UINT32  Cmd,
  IN  UINT32  Arg,
  IN  UINT16  Flags,
  OUT UINT32  *RespOut
  )
{
  UINTN   Spin;
  UINT16  Norm;
  UINT16  Err;

  MmioWrite32 (Hc + 0x38, 0);
  MmioWrite32 (Hc + 0x34, 0xFFFFFFFFU);
  MmioWrite32 (Hc + 0x30, 0xFFFFFFFFU);
  MmioWrite8  (Hc + 0x2E, 0x0EU);
  for (Spin = 0; Spin < 10000; Spin++) {
    if ((MmioRead32 (Hc + 0x24) & 0x3U) == 0) {
      break;
    }
    MicroSecondDelay (10);
  }
  MmioWrite32 (Hc + 0x08, Arg);
  MmioWrite16 (Hc + 0x0C, 0);
  MmioWrite16 (Hc + 0x0E, (UINT16)(((Cmd & 0x3FU) << 8) | Flags));
  Norm = 0;
  Err  = 0;
  for (Spin = 0; Spin < 20000; Spin++) {
    Norm = MmioRead16 (Hc + 0x30);
    if ((Norm & 0x8000U) != 0) {
      Err = MmioRead16 (Hc + 0x32);
      break;
    }
    if ((Norm & 0x0001U) != 0) {
      break;
    }
    MicroSecondDelay (10);
  }
  *RespOut = MmioRead32 (Hc + 0x10);
  MmioWrite32 (Hc + 0x30, 0xFFFFFFFFU);
  if ((Err != 0) || ((Norm & 0x0001U) == 0)) {
    MmioWrite8 (Hc + 0x2F, 0x06U);
    for (Spin = 0; Spin < 5000; Spin++) {
      if ((MmioRead8 (Hc + 0x2F) & 0x06U) == 0) {
        break;
      }
      MicroSecondDelay (10);
    }
    DEBUG ((DEBUG_ERROR, "  CMD%u fail norm=%04x err=%04x ps=%08x resp=%08x\n", Cmd, Norm, Err, MmioRead32 (Hc + 0x24), *RespOut));
    return FALSE;
  }
  return TRUE;
}

STATIC
VOID
ProbeSdccController (
  IN CONST CHAR8  *Name,
  IN UINT64       Core,
  IN UINT64       Hc,
  IN BOOLEAN      IsEmmc
  )
{
  UINT32  HcModeBefore;
  UINT32  HcModeAfter;
  UINT32  Pwrctl;
  UINT32  Resp;

  DEBUG ((
    DEBUG_ERROR,
    "PROBE %a HC: CAP=%08x PS=%08x CLK=%04x PWR=%02x HC1=%02x HC2=%04x\n",
    Name,
    MmioRead32 (Hc + 0x40),
    MmioRead32 (Hc + 0x24),
    MmioRead16 (Hc + 0x2C),
    MmioRead8  (Hc + 0x29),
    MmioRead8  (Hc + 0x28),
    MmioRead16 (Hc + 0x3E)
    ));
  DEBUG ((DEBUG_ERROR, "PROBE %a reading core_mem 0x%lx...\n", Name, Core));
  HcModeBefore = MmioRead32 (Core + 0x78);
  Pwrctl       = MmioRead32 (Core + 0xDC);
  if (Pwrctl != 0) {
    MmioWrite32 (Core + 0xE0, Pwrctl);
    MmioWrite32 (Core + 0xE8, 0x3U);
  }
  MmioWrite32 (Core + 0x78, 0x1U);
  MmioWrite32 (Core + 0x78, 0x2001U);
  HcModeAfter = MmioRead32 (Core + 0x78);
  DEBUG ((DEBUG_ERROR, "PROBE %a CORE: HC_MODE %08x->%08x PWRCTL=%08x MCI_CLK=%08x\n",
          Name, HcModeBefore, HcModeAfter, Pwrctl, MmioRead32 (Core + 0x04)));

  if (IsEmmc) {
    Resp = 0;
    if (!SdhciSimpleCmd (Hc, 13, 2U << 16, 0x1AU, &Resp)) {
      if (SdhciSimpleCmd (Hc, 5, 2U << 16, 0x1BU, &Resp)) {
        DEBUG ((DEBUG_ERROR, "  eMMC CMD5 AWAKE OK resp=%08x\n", Resp));
        MicroSecondDelay (5000);
        SdhciSimpleCmd (Hc, 13, 2U << 16, 0x1AU, &Resp);
      }
    }
    DEBUG ((DEBUG_ERROR, "  eMMC CMD13 resp=%08x (state=%u)\n", Resp, (Resp >> 9) & 0xFU));
    if (((Resp >> 9) & 0xFU) == 3U) {
      if (SdhciSimpleCmd (Hc, 7, 2U << 16, 0x1BU, &Resp)) {
        SdhciSimpleCmd (Hc, 13, 2U << 16, 0x1AU, &Resp);
        DEBUG ((DEBUG_ERROR, "  eMMC after CMD7 resp=%08x (state=%u)\n", Resp, (Resp >> 9) & 0xFU));
      }
    }
  } else {
    // MicroSD (SDCC2): check if CMD0 + CMD8(0x1AA) responds
    Resp = 0;
    SdhciSimpleCmd (Hc, 0, 0, 0x00U, &Resp);
    MicroSecondDelay (5000);
    if (SdhciSimpleCmd (Hc, 8, 0x1AAU, 0x1AU, &Resp)) {
      DEBUG ((DEBUG_ERROR, "  SDCC2 microSD CMD8 OK! resp=%08x\n", Resp));
    }
  }
}

STATIC
VOID
KenzoEnableSdccClocks (
  VOID
  )
{
  EnableGccSdcc ("SDCC1", GCC_SDCC1_APPS_CMD_RCGR, GCC_SDCC1_APPS_CFG_RCGR, GCC_SDCC1_APPS_CBCR, GCC_SDCC1_AHB_CBCR);
  EnableGccBranch (GCC_SDCC1_ICE_CORE_CBCR);
  EnableGccSdcc ("SDCC2", GCC_SDCC2_APPS_CMD_RCGR, GCC_SDCC2_APPS_CFG_RCGR, GCC_SDCC2_APPS_CBCR, GCC_SDCC2_AHB_CBCR);
}

// Must match KenzoPlatformMem.c / UniXNURamDiskDxe.c
STATIC CONST EFI_GUID  mEntryArgHobGuid = {
  0xDAEE82E6, 0x8090, 0x4E72, { 0xBF, 0xB9, 0x78, 0xC1, 0x3C, 0xD4, 0xE3, 0x59 }
};

RETURN_STATUS
EFIAPI
LzmaUefiDecompressGetInfo (
  IN  CONST VOID  *Source,
  IN  UINT32      SourceSize,
  OUT UINT32      *DestinationSize,
  OUT UINT32      *ScratchSize
  );

RETURN_STATUS
EFIAPI
LzmaUefiDecompress (
  IN CONST VOID  *Source,
  IN UINTN       SourceSize,
  IN OUT VOID    *Destination,
  IN OUT VOID    *Scratch
  );

STATIC
UINT64
ReadCells (
  IN CONST VOID  *Data,
  IN UINT32      Cells
  )
{
  if (Cells == 2) {
    return Fdt64ToCpu (ReadUnaligned64 (Data));
  }

  return Fdt32ToCpu (ReadUnaligned32 (Data));
}

STATIC
VOID
WriteCells (
  OUT VOID    *Data,
  IN  UINT32  Cells,
  IN  UINT64  Value
  )
{
  if (Cells == 2) {
    WriteUnaligned64 (Data, CpuToFdt64 (Value));
  } else {
    WriteUnaligned32 (Data, CpuToFdt32 ((UINT32)Value));
  }
}

EFI_STATUS
EFIAPI
KenzoInitrdDecompressEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_HOB_GUID_TYPE  *Hob;
  UINT64             DtbAddr;
  VOID               *Fdt;
  INT32              Chosen;
  CONST VOID         *StartProp;
  CONST VOID         *EndProp;
  INT32              StartLen;
  INT32              EndLen;
  UINT64             Start;
  UINT64             End;
  UINTN              CompSize;
  UINT32             DestSize;
  UINT32             ScratchSize;
  VOID               *Scratch;
  RETURN_STATUS      RetStatus;

  KenzoEnableSdccClocks ();

  Hob = GetFirstGuidHob (&mEntryArgHobGuid);
  if ((Hob == NULL) || (GET_GUID_HOB_DATA_SIZE (Hob) < sizeof (UINT64))) {
    return EFI_SUCCESS;
  }

  DtbAddr = ReadUnaligned64 ((CONST UINT64 *)GET_GUID_HOB_DATA (Hob));
  if ((DtbAddr == 0) || ((DtbAddr & 3) != 0)) {
    return EFI_SUCCESS;
  }

  Fdt = (VOID *)(UINTN)DtbAddr;
  if (FdtCheckHeader (Fdt) != 0) {
    return EFI_SUCCESS;
  }

  Chosen = FdtPathOffset (Fdt, "/chosen");
  if (Chosen < 0) {
    return EFI_SUCCESS;
  }

  StartProp = FdtGetProp (Fdt, Chosen, "linux,initrd-start", &StartLen);
  EndProp   = FdtGetProp (Fdt, Chosen, "linux,initrd-end", &EndLen);
  if ((StartProp == NULL) || (EndProp == NULL) ||
      ((StartLen != 4) && (StartLen != 8)) || ((EndLen != 4) && (EndLen != 8)))
  {
    return EFI_SUCCESS;
  }

  Start = ReadCells (StartProp, StartLen / 4);
  End   = ReadCells (EndProp, EndLen / 4);
  if ((End <= Start) || ((End - Start) < 18) ||
      (ReadUnaligned32 ((CONST UINT32 *)(UINTN)Start) != KENZO_KLZM_MAGIC))
  {
    // Diagnostic UEFI-only boot (no KLZM ramdisk): probe SDCC1 & SDCC2 and hold on screen.
    ProbeSdccController ("SDCC1", SDCC1_CORE_BASE, SDCC1_HC_BASE, TRUE);
    ProbeSdccController ("SDCC2", SDCC2_CORE_BASE, SDCC2_HC_BASE, FALSE);
    DEBUG ((DEBUG_ERROR, "KENZO_SDCC: diagnostic probe complete (holding screen)\n"));
    for (;;) {
      MicroSecondDelay (1000000);
    }
  }

  CompSize = (UINTN)(End - Start - 4);
  RetStatus = LzmaUefiDecompressGetInfo (
                (CONST VOID *)(UINTN)(Start + 4),
                (UINT32)CompSize,
                &DestSize,
                &ScratchSize
                );
  if (RETURN_ERROR (RetStatus) || (DestSize == 0) || (DestSize > KENZO_HIGH_INITRD_MAX)) {
    DEBUG ((DEBUG_ERROR, "KENZO_INITRD: bad LZMA info %r (DestSize=0x%x)\n", RetStatus, DestSize));
    return EFI_SUCCESS;
  }

  Scratch = AllocatePool (ScratchSize);
  if (Scratch == NULL) {
    DEBUG ((DEBUG_ERROR, "KENZO_INITRD: OOM for scratch 0x%x\n", ScratchSize));
    return EFI_SUCCESS;
  }

  RetStatus = LzmaUefiDecompress (
                (CONST VOID *)(UINTN)(Start + 4),
                CompSize,
                (VOID *)(UINTN)KENZO_HIGH_INITRD_BASE,
                Scratch
                );
  FreePool (Scratch);

  if (RETURN_ERROR (RetStatus)) {
    DEBUG ((DEBUG_ERROR, "KENZO_INITRD: LZMA decompress failed: %r\n", RetStatus));
    return EFI_SUCCESS;
  }

  // Update /chosen/linux,initrd-{start,end} in-place for UniXNURamDiskDxe
  WriteCells ((VOID *)StartProp, StartLen / 4, KENZO_HIGH_INITRD_BASE);
  WriteCells ((VOID *)EndProp, EndLen / 4, KENZO_HIGH_INITRD_BASE + DestSize);

  return EFI_SUCCESS;
}
