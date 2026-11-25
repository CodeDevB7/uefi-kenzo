/** @file
  ArmPlatformLib core routines for Xiaomi Redmi Note 3 (Kenzo) MSM8956.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/ArmLib.h>
#include <Library/ArmPlatformLib.h>
#include <Ppi/ArmMpCoreInfo.h>

// Stored in .data (non-zero init) so it is preserved across SEC startup
// before HobConstructor / PeilessSec runs.
volatile UINT64  mIncomingFdtAddress = 1;

// Verified from live Kenzo /proc/device-tree:
// CPUs 0-3 (Cortex-A53) and 0x100, 0x103 (Cortex-A72; 0x101/0x102 are NAK).
STATIC ARM_CORE_INFO  mKenzoMpCoreInfoTable[] = {
  { 0x000, 0, 0, 0, (UINT64)0xFFFFFFFF },
  { 0x001, 0, 0, 0, (UINT64)0xFFFFFFFF },
  { 0x002, 0, 0, 0, (UINT64)0xFFFFFFFF },
  { 0x003, 0, 0, 0, (UINT64)0xFFFFFFFF },
  { 0x100, 0, 0, 0, (UINT64)0xFFFFFFFF },
  { 0x103, 0, 0, 0, (UINT64)0xFFFFFFFF }
};

EFI_BOOT_MODE
ArmPlatformGetBootMode (
  VOID
  )
{
  return BOOT_WITH_FULL_CONFIGURATION;
}

RETURN_STATUS
ArmPlatformInitialize (
  IN UINTN  MpId
  )
{
  // Bring-up visual breadcrumb Band 1 (RED, rows 0..80 in 24-bit RGB888 at 0x83200000)
  // Proves PeilessSec CEntryPoint was reached.
  volatile UINT8  *Fb = (volatile UINT8 *)(UINTN)0x83200000ULL;
  UINTN           Idx;
  for (Idx = 0; Idx < (1080 * 80 * 3); Idx += 3) {
    Fb[Idx + 0] = 0x00;
    Fb[Idx + 1] = 0x00;
    Fb[Idx + 2] = 0xFF;
  }

  return RETURN_SUCCESS;
}

EFI_STATUS
PrePeiCoreGetMpCoreInfo (
  OUT UINTN          *CoreCount,
  OUT ARM_CORE_INFO  **ArmCoreTable
  )
{
  if (ArmIsMpCore ()) {
    *CoreCount    = sizeof (mKenzoMpCoreInfoTable) / sizeof (ARM_CORE_INFO);
    *ArmCoreTable = mKenzoMpCoreInfoTable;
    return EFI_SUCCESS;
  }

  return EFI_UNSUPPORTED;
}

STATIC ARM_MP_CORE_INFO_PPI  mMpCoreInfoPpi = {
  PrePeiCoreGetMpCoreInfo
};

STATIC EFI_PEI_PPI_DESCRIPTOR  mPlatformPpiTable[] = {
  {
    EFI_PEI_PPI_DESCRIPTOR_PPI,
    &gArmMpCoreInfoPpiGuid,
    &mMpCoreInfoPpi
  }
};

VOID
ArmPlatformGetPlatformPpiList (
  OUT UINTN                   *PpiListSize,
  OUT EFI_PEI_PPI_DESCRIPTOR  **PpiList
  )
{
  if (ArmIsMpCore ()) {
    *PpiListSize = sizeof (mPlatformPpiTable);
    *PpiList     = mPlatformPpiTable;
  } else {
    *PpiListSize = 0;
    *PpiList     = NULL;
  }
}
