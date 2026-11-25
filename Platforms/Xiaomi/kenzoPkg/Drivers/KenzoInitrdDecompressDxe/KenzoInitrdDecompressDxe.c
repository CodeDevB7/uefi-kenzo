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
#include <Library/MemoryAllocationLib.h>

#define KENZO_KLZM_MAGIC          0x4D5A4C4BU  // "KLZM"
#define KENZO_HIGH_INITRD_BASE    0xA0000000ULL
#define KENZO_HIGH_INITRD_MAX     0x10000000U  // 256 MiB protected window

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
