[Defines]
  PLATFORM_NAME                  = kenzo
  PLATFORM_GUID                  = 8A1901B3-855E-4CF6-BA55-6E5C20A2E2C9
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x00010005
  OUTPUT_DIRECTORY               = Build/kenzoPkg
  SUPPORTED_ARCHITECTURES        = AARCH64
  BUILD_TARGETS                  = RELEASE|DEBUG
  SKUID_IDENTIFIER               = DEFAULT
  FLASH_DEFINITION               = Platforms/Xiaomi/kenzoPkg/kenzo.fdf

[PcdsFixedAtBuild]
  # Memory Base
  gArmTokenSpaceGuid.PcdSystemMemoryBase|0x80000000
  gArmTokenSpaceGuid.PcdSystemMemorySize|0x80000000

  # Firmware FD
  gArmTokenSpaceGuid.PcdFdBaseAddress|$(FD_BASE)
  gArmTokenSpaceGuid.PcdFdSize|$(FD_SIZE)

  # Stack
  gArmPlatformTokenSpaceGuid.PcdCPUCoresStackBase|0x9FF90000
  gArmPlatformTokenSpaceGuid.PcdCPUCorePrimaryStackSize|0x40000

  # GICv2 on MSM8956
  gArmTokenSpaceGuid.PcdGicDistributorBase|0x0B000000
  gArmTokenSpaceGuid.PcdGicInterruptInterfaceBase|0x0B002000

  # Arch Timer
  gArmTokenSpaceGuid.PcdArmArchTimerFreqInHz|19200000

  # Framebuffer (NT35532 1080p Single-DSI)
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferWidth|1080
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferHeight|1920
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferColorDepth|32

  # Device Identification
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemManufacturer|"Xiaomi"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemModel|"Redmi Note 3"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailModel|"kenzo"

[LibraryClasses]
  MemoryMapLib|Platforms/Xiaomi/kenzoPkg/Library/MemoryMapLib/MemoryMapLib.inf
  HobLib|MdePkg/Library/DxeHobLib/DxeHobLib.inf
  FdtLib|MdePkg/Library/BaseFdtLib/BaseFdtLib.inf

[Components]
  # Core Platform
  Platforms/Xiaomi/kenzoPkg/Library/MemoryMapLib/MemoryMapLib.inf

  # UniXNU integration
  UniXNUPkg/Application/UniXNU/UniXNU.inf
  UniXNUPkg/Driver/UniXNUDxe/UniXNUDxe.inf
  UniXNUPkg/Driver/UniXNURamDiskDxe/UniXNURamDiskDxe.inf
  DarwinBootPkg/Driver/DarwinBoot.inf

  # Filesystem & Storage
  MdeModulePkg/Universal/Disk/DiskIoDxe/DiskIoDxe.inf
  FatPkg/EnhancedFatDxe/Fat.inf
