# reBART

Resizable BAR for Intel 8 series (Haswell/Lynx Point) motherboards (Z87, H87, Q87, B85, Q85, H81)

On Intel 8 series the firmware cannot decode memory above 4GB.
8 series firmware doesn't expose any MMIO space above 4GB. The host bridge advertises
no 64bit decode, no GCD region is registered above the top of DRAM. 
That code is in the firmware of the generation before and after, but not in 8 series.

The adjacent Intel generations, 6, 7 series and 9 series+ can decode above 4 GB, 
so use [xCuri0's ReBarUEFI](https://github.com/xCuri0/ReBarUEFI).
reBART may still be useful on them if ReBarUEFI fails. For example, a black screen during post.
(yes you can try reBART on Intel 6,7,9+ series motherboards)

reBART resizes after the firmware is done with the card. 
It reprograms the BAR, opens the upstream bridge's prefetchable window above 4GB, 
and moves the GOP framebuffer to the new address so the OS boots with the BAR in place.

## Requirements

- 64-bit OS, booted UEFI/GPT with CSM off
- Discrete NVIDIA / AMD or Intel GPU as primary display, recent driver

## Modding the BIOS

Needs
- [UEFITool](https://github.com/LongSoft/UEFITool/releases/tag/0.28.0) UEFITool 0.28.0
- [AmiBoardInfoTool](https://github.com/xCuri0/AmiBoardInfoTool/releases) for DSDT
- **rebart_dsdt_patch.exe**
- **reBART.ffs** driver

### 1) Patch the DSDT

- Open your BIOS file in UEFITool
- Press Ctrl+F, click the **GUID** tab, paste
   `9F3A0016-AE55-4288-829D-D22FD344C347`, and press OK
- Double click the result to jump to it. Click the arrows to expand it until you see a line that says **PE32 image section**
- Right-click **PE32 image section** > **Extract body**, and save it as `AmiBoardInfo.efi`.
- Open a command prompt and run:

   ```
   AmiBoardInfoTool -a AmiBoardInfo.efi -d DSDT.aml
   ```

   You now have `DSDT.aml`

### Below DSDT patching is for 8 series only. Others may refer [DSDT Patching](https://github.com/xCuri0/ReBarUEFI/wiki/DSDT-Patching) 

**patch with `rebart_dsdt_patch.exe`**

In command prompt run:

```
rebart_dsdt_patch.exe DSDT.aml DSDTMod.aml
```

### 2) Put it back

1. Put the patched table back into the module:

   ```
   AmiBoardInfoTool -a AmiBoardInfo.efi -d DSDTMod.aml -o AmiBoardInfoMod.efi
   ```

2. Back in UEFITool, right-click the same **PE32 image section** > **Replace
   body**, and pick the new `AmiBoardInfoMod.efi`

### 3) Insert the reBART driver
   - **Find** File -> Search (Ctrl+F) -> GUID tab -> Header only search for 3C1DE39F-D207-408A-AACC-731CFB7F1DD7
   - **Add**: right click the file -> *Insert after…* -> pick the `reBART.ffs`
   - **Save**: File -> *Save image file* save the bios

### 4) Flash the new ROM with the board's bios flash tool or AFU tools, then clear CMOS.

### 5) Set the BAR size from OS 
Use ReBarState.exe [ReBarState](https://github.com/xCuri0/ReBarUEFI/releases) tool to set the BAR size. Reboot to apply. **If a size won't POST, clear CMOS**

## Notes
To enable resizable BAR on AMD Polaris & Vega, apply this registry key

```asl
Windows Registry Editor Version 5.00

[HKEY_LOCAL_MACHINE\SYSTEM\ControlSet001\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}\0000]
"KMD_EnableReBarForLegacyASIC"=dword:00000001
"KMD_RebarControlMode"=dword:00000001
"KMD_RebarControlSupport"=dword:00000001
```

- AMD Polaris - 4GB BAR on a 4GB card trips an AMD driver bug. 3D init fails with E_FAIL. Use 2GB BAR

## License

MIT - [LICENSE](LICENSE). Third-party attributions in [NOTICE](NOTICE)

## Credits

- [ReBarUEFI](https://github.com/xCuri0/ReBarUEFI) `ReBarState` variable interop and the Resizable BAR approach
- [NvStrapsReBar](https://github.com/terminatorul/NvStrapsReBar) Turing strap method
- Built with EDK2/TianoCore
