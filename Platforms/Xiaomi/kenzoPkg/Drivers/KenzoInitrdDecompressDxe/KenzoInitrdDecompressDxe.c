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

#define KENZO_KLZM_MAGIC          0x4D5A4C4BU  // "KLZM"
#define KENZO_HIGH_INITRD_BASE    0xA0000000ULL
#define KENZO_HIGH_INITRD_MAX     0x10000000U  // 256 MiB protected window

// MSM8956 GCC SDCC1 (eMMC @ 0x07824900) and SDCC2 (microSD @ 0x07864900) clocks
#define GCC_SDCC1_APPS_CMD_RCGR   0x01842004ULL
#define GCC_SDCC1_APPS_CFG_RCGR   0x01842008ULL
#define GCC_SDCC1_APPS_CBCR       0x01842018ULL
#define GCC_SDCC1_AHB_CBCR        0x0184201CULL
#define GCC_SDCC1_ICE_CORE_CBCR   0x0185D014ULL

#define GCC_SDCC2_APPS_CMD_RCGR   0x01843004ULL
#define GCC_SDCC2_APPS_CFG_RCGR   0x01843008ULL
#define GCC_SDCC2_APPS_CBCR       0x01843018ULL
#define GCC_SDCC2_AHB_CBCR        0x0184301CULL

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
    "KENZO_SDCC %a: AHB 0x%08x->0x%08x APPS 0x%08x->0x%08x RCG=0x%08x/0x%08x\n",
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
  if ((End <= Start) || ((End - Start) < 18)) {
    return EFI_SUCCESS;
  }

  // Only decompress if prefixed with "KLZM" magic
  if (ReadUnaligned32 ((CONST UINT32 *)(UINTN)Start) != KENZO_KLZM_MAGIC) {
    return EFI_SUCCESS;
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
