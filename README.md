# uefi-kenzo

Custom EDK II / Mu-Silicium UEFI platform port for **Xiaomi Redmi Note 3 (`kenzo`)**, Qualcomm MSM8956 (Snapdragon 650).

## Platform Details
- **Architecture**: ARM64 (`AARCH64`)
- **Firmware Base (`FD_BASE`)**: `0x9FC00000`
- **Firmware Size (`FD_SIZE`)**: `0x00300000` (3 MiB)
- **GIC Version**: GICv2 (`Distributor 0x0B000000`, `CPU Interface 0x0B002000`)
- **Timer Frequency**: 19.2 MHz (`19200000`)
- **Framebuffer**: `1080x1920` (BOE NT35532 1080p Single-DSI) at physical address `0x83000000` (`cont_splash_mem`)

## UniXNU Integration
Integrates `UniXNUPkg` (menu, boot option registrar, FDT initrd RAM disk) and `DarwinBoot` (XNU loader).
