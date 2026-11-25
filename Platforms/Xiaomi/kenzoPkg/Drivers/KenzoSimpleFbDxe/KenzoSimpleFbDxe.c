/** @file
  Simple Framebuffer GOP driver for Xiaomi Redmi Note 3 (Kenzo) MSM8956.

  Reads the active MDP5 continuous splash configuration left by LittleKernel
  (aboot), reconfigures the active SSPP pipe to 32-bit BGRX8888 (required by
  DarwinBootVideoFromGop), clears the framebuffer, and installs GOP.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <PiDxe.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/FrameBufferBltLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/DevicePath.h>
#include <Protocol/GraphicsOutput.h>

// Verified MSM8956 / MSM8952 MDSS / MDP5 register addresses
#define MDP_GDSCR                     0x0184D078
#define GDSC_POWER_ON_BIT             BIT31

#define MDP_CTL_0_FLUSH               0x01A02018

#define MDP_VP_0_VIG_0_BASE           0x01A05000
#define MDP_VP_0_RGB_0_BASE           0x01A15000
#define MDP_VP_0_DMA_0_BASE           0x01A25000

#define PIPE_SSPP_SRC_IMG_SIZE        0x04
#define PIPE_SSPP_SRC0_ADDR           0x14
#define PIPE_SSPP_SRC_YSTRIDE         0x24
#define PIPE_SSPP_SRC_FORMAT          0x30
#define PIPE_SSPP_SRC_UNPACK_PATTERN  0x34

// MDP5 SSPP 32-bit BGRX8888 (PixelBlueGreenRedReserved8BitPerColor in memory: B, G, R, X)
#define MDP5_SRC_FORMAT_XRGB8888      0x000236FF
#define MDP5_SRC_UNPACK_BGRX          0x03020001

// Kenzo continuous splash carveout bounds and fallback
#define KENZO_SPLASH_MIN              0x83000000ULL
#define KENZO_SPLASH_MAX              0x85800000ULL
#define KENZO_FB_FALLBACK_ADDR        0x83200000ULL
#define KENZO_FB_FALLBACK_WIDTH       1080
#define KENZO_FB_FALLBACK_HEIGHT      1920

typedef struct {
  UINT32    Base;
  UINT32    FlushBit;
} MDP_PIPE_DESC;

STATIC CONST MDP_PIPE_DESC  mMdpPipes[] = {
  { MDP_VP_0_VIG_0_BASE, BIT0  },
  { MDP_VP_0_RGB_0_BASE, BIT3  },
  { MDP_VP_0_DMA_0_BASE, BIT11 }
};

#pragma pack(1)
typedef struct {
  VENDOR_DEVICE_PATH          Vendor;
  EFI_DEVICE_PATH_PROTOCOL    End;
} KENZO_FB_DEVICE_PATH;
#pragma pack()

STATIC KENZO_FB_DEVICE_PATH  mFbDevicePath = {
  {
    {
      HARDWARE_DEVICE_PATH,
      HW_VENDOR_DP,
      { (UINT8)sizeof (VENDOR_DEVICE_PATH), (UINT8)(sizeof (VENDOR_DEVICE_PATH) >> 8) }
    },
    { 0x4D9C1B30, 0xA1E4, 0x4F7C, { 0x9A, 0x2B, 0x7E, 0x3B, 0x2E, 0x1F, 0x04, 0xA1 } }
  },
  {
    END_DEVICE_PATH_TYPE,
    END_ENTIRE_DEVICE_PATH_SUBTYPE,
    { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 }
  }
};

STATIC EFI_GRAPHICS_OUTPUT_MODE_INFORMATION  mModeInfo;
STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE      mGopMode = {
  1,
  0,
  &mModeInfo,
  sizeof (EFI_GRAPHICS_OUTPUT_MODE_INFORMATION),
  0,
  0
};
STATIC FRAME_BUFFER_CONFIGURE               *mBltConfigure = NULL;

STATIC
EFI_STATUS
EFIAPI
KenzoGopQueryMode (
  IN  EFI_GRAPHICS_OUTPUT_PROTOCOL          *This,
  IN  UINT32                                ModeNumber,
  OUT UINTN                                 *SizeOfInfo,
  OUT EFI_GRAPHICS_OUTPUT_MODE_INFORMATION  **Info
  )
{
  if ((This == NULL) || (SizeOfInfo == NULL) || (Info == NULL) || (ModeNumber >= This->Mode->MaxMode)) {
    return EFI_INVALID_PARAMETER;
  }

  *SizeOfInfo = sizeof (EFI_GRAPHICS_OUTPUT_MODE_INFORMATION);
  *Info       = AllocateCopyPool (*SizeOfInfo, &mModeInfo);
  if (*Info == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
KenzoGopSetMode (
  IN EFI_GRAPHICS_OUTPUT_PROTOCOL  *This,
  IN UINT32                        ModeNumber
  )
{
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Black;

  if ((This == NULL) || (ModeNumber >= This->Mode->MaxMode)) {
    return EFI_UNSUPPORTED;
  }

  if (mBltConfigure != NULL) {
    ZeroMem (&Black, sizeof (Black));
    FrameBufferBlt (
      mBltConfigure,
      &Black,
      EfiBltVideoFill,
      0,
      0,
      0,
      0,
      mModeInfo.HorizontalResolution,
      mModeInfo.VerticalResolution,
      0
      );
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
KenzoGopBlt (
  IN EFI_GRAPHICS_OUTPUT_PROTOCOL       *This,
  IN OUT EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *BltBuffer OPTIONAL,
  IN EFI_GRAPHICS_OUTPUT_BLT_OPERATION  BltOperation,
  IN UINTN                              SourceX,
  IN UINTN                              SourceY,
  IN UINTN                              DestinationX,
  IN UINTN                              DestinationY,
  IN UINTN                              Width,
  IN UINTN                              Height,
  IN UINTN                              Delta OPTIONAL
  )
{
  if ((This == NULL) || (mBltConfigure == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  return (EFI_STATUS)FrameBufferBlt (
                       mBltConfigure,
                       BltBuffer,
                       BltOperation,
                       SourceX,
                       SourceY,
                       DestinationX,
                       DestinationY,
                       Width,
                       Height,
                       Delta
                       );
}

STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL  mGop = {
  KenzoGopQueryMode,
  KenzoGopSetMode,
  KenzoGopBlt,
  &mGopMode
};

STATIC
VOID
ProbeAndConfigureMdp5Splash (
  OUT UINT64  *OutFbBase,
  OUT UINT32  *OutWidth,
  OUT UINT32  *OutHeight
  )
{
  UINTN   Index;
  UINT32  PipeBase;
  UINT32  FbAddr;
  UINT32  ImgSize;
  UINT32  Width;
  UINT32  Height;
  UINT64  FbSize;

  *OutFbBase = KENZO_FB_FALLBACK_ADDR;
  *OutWidth  = KENZO_FB_FALLBACK_WIDTH;
  *OutHeight = KENZO_FB_FALLBACK_HEIGHT;

  if ((MmioRead32 (MDP_GDSCR) & GDSC_POWER_ON_BIT) == 0) {
    DEBUG ((DEBUG_WARN, "KenzoSimpleFb: MDP_GDSCR off, using fallback %dx%d @ 0x%lx\n",
            *OutWidth, *OutHeight, *OutFbBase));
    return;
  }

  for (Index = 0; Index < sizeof (mMdpPipes) / sizeof (mMdpPipes[0]); Index++) {
    PipeBase = mMdpPipes[Index].Base;
    FbAddr   = MmioRead32 (PipeBase + PIPE_SSPP_SRC0_ADDR);
    if (FbAddr == 0) {
      continue;
    }

    ImgSize = MmioRead32 (PipeBase + PIPE_SSPP_SRC_IMG_SIZE);
    Width   = ImgSize & 0xFFFF;
    Height  = (ImgSize >> 16) & 0xFFFF;
    FbSize  = (UINT64)Width * (UINT64)Height * 4ULL;

    if ((Width >= 320) && (Width <= 2560) &&
        (Height >= 320) && (Height <= 2560) &&
        ((UINT64)FbAddr >= KENZO_SPLASH_MIN) &&
        (((UINT64)FbAddr + FbSize) <= KENZO_SPLASH_MAX))
    {
      // Reconfigure SSPP pipe from 24-bit RGB888 to 32-bit BGRX8888
      MmioWrite32 (PipeBase + PIPE_SSPP_SRC_FORMAT, MDP5_SRC_FORMAT_XRGB8888);
      MmioWrite32 (PipeBase + PIPE_SSPP_SRC_UNPACK_PATTERN, MDP5_SRC_UNPACK_BGRX);
      MmioWrite32 (PipeBase + PIPE_SSPP_SRC_YSTRIDE, Width * 4);
      MmioWrite32 (MDP_CTL_0_FLUSH, mMdpPipes[Index].FlushBit);

      *OutFbBase = (UINT64)FbAddr;
      *OutWidth  = Width;
      *OutHeight = Height;
      DEBUG ((DEBUG_INFO, "KenzoSimpleFb: MDP5 pipe 0x%x -> %dx%d BGRX8888 @ 0x%lx\n",
              PipeBase, Width, Height, *OutFbBase));
      return;
    }
  }

  DEBUG ((DEBUG_WARN, "KenzoSimpleFb: no active MDP5 splash pipe found, using fallback\n"));
}

EFI_STATUS
EFIAPI
KenzoSimpleFbDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS     Status;
  UINT64         FbBase;
  UINT32         Width;
  UINT32         Height;
  UINTN          FbSize;
  UINTN          ConfigureSize;
  RETURN_STATUS  RetStatus;
  EFI_HANDLE     Handle;

  // Bring-up visual breadcrumb Band 4 (BLUE, rows 240..320 in 24-bit RGB888 at 0x83200000)
  // Proves DecompressFvs + DxeCore + DXE dispatch reached KenzoSimpleFbDxe!
  {
    volatile UINT8  *Fb = (volatile UINT8 *)(UINTN)(0x83200000ULL + (1080 * 240 * 3));
    UINTN           Idx;
    for (Idx = 0; Idx < (1080 * 80 * 3); Idx += 3) {
      Fb[Idx + 0] = 0xFF;
      Fb[Idx + 1] = 0x00;
      Fb[Idx + 2] = 0x00;
    }
  }

  ProbeAndConfigureMdp5Splash (&FbBase, &Width, &Height);
  FbSize = (UINTN)Width * (UINTN)Height * 4;

  ZeroMem ((VOID *)(UINTN)FbBase, FbSize);

  // Thin CYAN bar (rows 0..40, 32-bit BGRX) so we know KenzoSimpleFbDxe cleared & switched to 32-bit!
  {
    volatile UINT32  *Fb32 = (volatile UINT32 *)(UINTN)FbBase;
    UINTN            Idx;
    for (Idx = 0; Idx < (Width * 40); Idx++) {
      Fb32[Idx] = 0x0000FFFF;
    }
  }

  mModeInfo.Version              = 0;
  mModeInfo.HorizontalResolution = Width;
  mModeInfo.VerticalResolution   = Height;
  mModeInfo.PixelFormat          = PixelBlueGreenRedReserved8BitPerColor;
  mModeInfo.PixelsPerScanLine    = Width;
  ZeroMem (&mModeInfo.PixelInformation, sizeof (mModeInfo.PixelInformation));

  mGopMode.FrameBufferBase = (EFI_PHYSICAL_ADDRESS)FbBase;
  mGopMode.FrameBufferSize = FbSize;

  ConfigureSize = 0;
  RetStatus = FrameBufferBltConfigure ((VOID *)(UINTN)FbBase, &mModeInfo, NULL, &ConfigureSize);
  if (RetStatus == RETURN_BUFFER_TOO_SMALL) {
    mBltConfigure = AllocatePool (ConfigureSize);
    if (mBltConfigure != NULL) {
      RetStatus = FrameBufferBltConfigure ((VOID *)(UINTN)FbBase, &mModeInfo, mBltConfigure, &ConfigureSize);
      if (RETURN_ERROR (RetStatus)) {
        FreePool (mBltConfigure);
        mBltConfigure = NULL;
      }
    }
  }

  Handle = NULL;
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Handle,
                  &gEfiGraphicsOutputProtocolGuid,
                  &mGop,
                  &gEfiDevicePathProtocolGuid,
                  &mFbDevicePath,
                  NULL
                  );
  return Status;
}
