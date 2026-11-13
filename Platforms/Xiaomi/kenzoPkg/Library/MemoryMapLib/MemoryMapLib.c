/** @file
  MemoryMapLib for Xiaomi Redmi Note 3 (Kenzo) MSM8956.
  Carveouts reconciled from device FDT (DTB 0) and /proc/iomem.
**/

#include <Library/MemoryMapLib.h>

STATIC
EFI_MEMORY_REGION_DESCRIPTOR
gMemoryDescriptor[] = {
  // Name, Address, Length, HobOption, ResourceType, ResourceAttribute, MemoryType, ArmAttribute

  // DDR Regions - Low Memory & Display
  {"Kernel Area",       0x80000000, 0x03000000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK_XN},
  {"Display Reserved",  0x83000000, 0x02800000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_THROUGH_XN},
  {"Low RAM Chunk",     0x85800000, 0x00B00000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK_XN},

  // Firmware Carveouts (Must be reserved/removed)
  {"Other Ext Region",  0x86300000, 0x00500000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"Interstitial RAM1", 0x86800000, 0x00400000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK_XN},
  {"Modem Firmware",    0x86C00000, 0x05600000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"Reloc Region MBA",  0x8C200000, 0x01700000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"Interstitial RAM2", 0x8D900000, 0x00400000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK_XN},
  {"QSEECom / TZApps",  0x8DD00000, 0x01900000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},

  // High System Memory (0x8F600000 - 0xFFFFFFFF)
  {"DXE Heap",          0x90000000, 0x0B800000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK_XN},
  {"FV Region",         0x9B800000, 0x00400000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK_XN},
  {"UEFI FD",           0x9FC00000, 0x00300000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK},
  {"SEC Heap",          0x9FF00000, 0x0008C000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK_XN},
  {"MMU PageTables",    0x9FF8D000, 0x00003000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK_XN},
  {"UEFI Stack",        0x9FF90000, 0x00040000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK_XN},
  {"Log Buffer",        0x9FFF0000, 0x0000F000, AddMem, SYS_MEM, SYS_MEM_CAP, RtData, WRITE_BACK_XN},
  {"Info Blk",          0x9FFFF000, 0x00001000, AddMem, SYS_MEM, SYS_MEM_CAP, RtData, WRITE_BACK_XN},
  {"High Free RAM",     0xA0000000, 0x5FC00000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK_XN},

  // Terminator
  {}
};

EFI_STATUS
EFIAPI
GetMemoryElements (
  OUT EFI_MEMORY_REGION_DESCRIPTOR **MemoryDescriptor,
  OUT UINTN                        *MemoryDescriptorCount
  )
{
  if (MemoryDescriptor == NULL || MemoryDescriptorCount == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  *MemoryDescriptor      = gMemoryDescriptor;
  *MemoryDescriptorCount = (sizeof (gMemoryDescriptor) / sizeof (gMemoryDescriptor[0])) - 1;

  return EFI_SUCCESS;
}
