/** @file
  Virtual memory map and HOB handoff for Xiaomi Redmi Note 3 (Kenzo) MSM8956.
  Carveouts reconciled from kenzo-dump/iomem.txt and extracted_dtb_0.dtb.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <PiPei.h>
#include <Uefi.h>
#include <Library/ArmLib.h>
#include <Library/ArmPlatformLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/HobLib.h>
#include <Library/PcdLib.h>

extern volatile UINT64  mIncomingFdtAddress;

// Must match UniXNURamDiskDxe.c mEntryArgHobGuid
STATIC CONST EFI_GUID  mEntryArgHobGuid = {
  0xDAEE82E6, 0x8090, 0x4E72, { 0xBF, 0xB9, 0x78, 0xC1, 0x3C, 0xD4, 0xE3, 0x59 }
};

STATIC ARM_MEMORY_REGION_DESCRIPTOR  mKenzoMemoryTable[] = {
  // RPM Message RAM (0x00060000, 32 KiB) for RPM-SMD regulator votes (microSD LDO11/LDO12)
  {
    0x00060000,
    0x00060000,
    0x00010000,
    ARM_MEMORY_REGION_ATTRIBUTE_DEVICE
  },
  // Peripheral MMIO space: TLMM (0x01000000), GCC (0x01800000), MDP5 (0x01A00000),
  // SDHCI (0x07824900/0x07864900), UART (0x078B0000), GICv2 (0x0B000000). Page 0 is left unmapped.
  {
    0x01000000,
    0x01000000,
    0x0F000000,
    ARM_MEMORY_REGION_ATTRIBUTE_DEVICE
  },
  // LK kernel/tags/ramdisk load window (48 MiB)
  {
    0x80000000,
    0x80000000,
    0x03000000,
    ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK
  },
  // Continuous splash framebuffer (40 MiB, NT35532 1080p) - Normal Non-Cacheable for MDP5 DMA
  {
    0x83000000,
    0x83000000,
    0x02800000,
    ARM_MEMORY_REGION_ATTRIBUTE_UNCACHED_UNBUFFERED
  },
  // Low RAM chunk (11 MiB)
  {
    0x85800000,
    0x85800000,
    0x00B00000,
    ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK
  },
  // SMEM (1 MiB at 0x86300000, inside other_ext_region) - Non-Cacheable for RPM-SMD
  {
    0x86300000,
    0x86300000,
    0x00100000,
    ARM_MEMORY_REGION_ATTRIBUTE_UNCACHED_UNBUFFERED
  },
  // Note: 0x86400000..0x867FFFFF (remaining other_ext_region 4M) is unmapped
  // Interstitial RAM 1 (4 MiB)
  {
    0x86800000,
    0x86800000,
    0x00400000,
    ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK
  },
  // Note: 0x86C00000..0x8D8FFFFF (modem_region 86M + reloc_region 23M) is unmapped
  // Interstitial RAM 2 (4 MiB)
  {
    0x8D900000,
    0x8D900000,
    0x00400000,
    ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK
  },
  // Note: 0x8DD00000..0x8F5FFFFF (tzapp_region / QSEECom 25M) is unmapped
  // High contiguous System RAM (1802 MiB: 0x8F600000..0xFFFFFFFF)
  {
    0x8F600000,
    0x8F600000,
    0x70A00000,
    ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK
  },
  // Terminator
  {
    0,
    0,
    0,
    0
  }
};

VOID
ArmPlatformGetVirtualMemoryMap (
  OUT ARM_MEMORY_REGION_DESCRIPTOR  **VirtualMemoryMap
  )
{
  UINT64                       FdtAddress;
  EFI_RESOURCE_ATTRIBUTE_TYPE  SystemAttributes;
  EFI_RESOURCE_ATTRIBUTE_TYPE  ReservedAttributes;

  ASSERT (VirtualMemoryMap != NULL);

  // 1. Publish incoming x0 (FDT address) for UniXNURamDiskDxe
  FdtAddress = mIncomingFdtAddress;
  BuildGuidDataHob (&mEntryArgHobGuid, &FdtAddress, sizeof (UINT64));

  SystemAttributes =
    EFI_RESOURCE_ATTRIBUTE_PRESENT |
    EFI_RESOURCE_ATTRIBUTE_INITIALIZED |
    EFI_RESOURCE_ATTRIBUTE_WRITE_COMBINEABLE |
    EFI_RESOURCE_ATTRIBUTE_WRITE_THROUGH_CACHEABLE |
    EFI_RESOURCE_ATTRIBUTE_WRITE_BACK_CACHEABLE |
    EFI_RESOURCE_ATTRIBUTE_TESTED;

  ReservedAttributes =
    EFI_RESOURCE_ATTRIBUTE_PRESENT |
    EFI_RESOURCE_ATTRIBUTE_INITIALIZED;

  // 2. Declare DRAM resource descriptors so MemoryPeim does NOT declare the
  // entire 2 GiB range as usable System Memory (which would expose Modem/TZ).
  // LK kernel/tags/ramdisk load window (reserved so DXE Core doesn't clobber FDT/initrd)
  BuildResourceDescriptorV2 (
    EFI_RESOURCE_SYSTEM_MEMORY,
    SystemAttributes,
    0x80000000,
    0x03000000,
    EFI_MEMORY_WB,
    NULL
    );
  BuildMemoryAllocationHob (0x80000000, 0x03000000, EfiReservedMemoryType);

  // Continuous splash framebuffer (40 MiB)
  BuildResourceDescriptorV2 (
    EFI_RESOURCE_MEMORY_RESERVED,
    ReservedAttributes,
    0x83000000,
    0x02800000,
    EFI_MEMORY_WT,
    NULL
    );

  // Low RAM chunk (11 MiB)
  BuildResourceDescriptorV2 (
    EFI_RESOURCE_SYSTEM_MEMORY,
    SystemAttributes,
    0x85800000,
    0x00B00000,
    EFI_MEMORY_WB,
    NULL
    );

  // Other Ext Region (5 MiB)
  BuildResourceDescriptorV2 (
    EFI_RESOURCE_MEMORY_RESERVED,
    ReservedAttributes,
    0x86300000,
    0x00500000,
    EFI_MEMORY_UC,
    NULL
    );

  // Interstitial RAM 1 (4 MiB)
  BuildResourceDescriptorV2 (
    EFI_RESOURCE_SYSTEM_MEMORY,
    SystemAttributes,
    0x86800000,
    0x00400000,
    EFI_MEMORY_WB,
    NULL
    );

  // Modem Firmware (86 MiB)
  BuildResourceDescriptorV2 (
    EFI_RESOURCE_MEMORY_RESERVED,
    ReservedAttributes,
    0x86C00000,
    0x05600000,
    EFI_MEMORY_UC,
    NULL
    );

  // MBA / Relocation Region (23 MiB)
  BuildResourceDescriptorV2 (
    EFI_RESOURCE_MEMORY_RESERVED,
    ReservedAttributes,
    0x8C200000,
    0x01700000,
    EFI_MEMORY_UC,
    NULL
    );

  // Interstitial RAM 2 (4 MiB)
  BuildResourceDescriptorV2 (
    EFI_RESOURCE_SYSTEM_MEMORY,
    SystemAttributes,
    0x8D900000,
    0x00400000,
    EFI_MEMORY_WB,
    NULL
    );

  // QSEECom / TZApps (25 MiB)
  BuildResourceDescriptorV2 (
    EFI_RESOURCE_MEMORY_RESERVED,
    ReservedAttributes,
    0x8DD00000,
    0x01900000,
    EFI_MEMORY_UC,
    NULL
    );

  // High contiguous System RAM (1802 MiB: 0x8F600000..0xFFFFFFFF)
  // MemoryPeim will locate PcdSystemMemoryBase (0x8F600000) here and split
  // off the Firmware Device (0x9FC00000..0x9FF00000) as EfiBootServicesData.
  BuildResourceDescriptorV2 (
    EFI_RESOURCE_SYSTEM_MEMORY,
    SystemAttributes,
    0x8F600000,
    0x70A00000,
    EFI_MEMORY_WB,
    NULL
    );

  // Protect high-RAM initrd/ramdisk window (0xA0000000..0xAFFFFFFF, 256 MiB)
  // loaded by LittleKernel aboot (ramdisk_offset=0x20000000) so DXE/DarwinBoot
  // never overwrite the UniXNU FAT payload before ExitBootServices.
  BuildMemoryAllocationHob (0xA0000000, 0x10000000, EfiBootServicesData);

  // Optional ramoops carveout (4 MiB at 0xDFC00000)
  BuildMemoryAllocationHob (0xDFC00000, 0x00400000, EfiReservedMemoryType);

  *VirtualMemoryMap = mKenzoMemoryTable;
}
