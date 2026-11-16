# Boots a Four Eyes image under QEMU for Windows with the Windows Hypervisor Platform, headless.
#
#   powershell -File qnx/image/boot_whpx.ps1 -Image DIR [-Qemu PATH] [-Snapshot]
#   (-Qemu defaults to $env:QEMU, else qemu-system-x86_64.exe on the PATH)
#
# DIR holds ifs.bin, disk-qemu.vmdk and disk-qemu (from qnx/image/build.sh, copied out of WSL).
# The guest's ssh is forwarded to 127.0.0.1:2242. Its serial console is a TCP server on 127.0.0.1:5520 and is also
# written to DIR\console-<time>.log. The QEMU process id goes to DIR\qemu.pid, so tools/guest.py stops only this guest.
#
# QNX 8.0's startup needs the always-running APIC timer bit in CPUID leaf 6, which Hyper-V hides; "+arat" with
# "x-force-features=on" restores it. AVX-512 stays off: llama.cpp is built for AVX2.
param(
    [Parameter(Mandatory = $true)][string]$Image,
    [string]$Qemu = $(if ($env:QEMU) { $env:QEMU } else { "qemu-system-x86_64.exe" }),
    [switch]$Snapshot
)
$ErrorActionPreference = "Stop"
$Image = (Resolve-Path $Image).Path
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$log = "console-$stamp.log"
$noAvx512 = "-avx512f,-avx512dq,-avx512ifma,-avx512cd,-avx512bw,-avx512vl,-avx512vbmi,-avx512vbmi2,-avx512vnni,-avx512bitalg,-avx512-vpopcntdq,-avx512-bf16"
$drive = "file=disk-qemu.vmdk,if=ide,id=drv0"
if ($Snapshot) { $drive += ",snapshot=on" }
$a = @("-accel", "whpx", "-machine", "pc", "-cpu", "host,+arat,x-force-features=on,$noAvx512", "-smp", "2", "-m", "4G",
    "-drive", $drive,
    "-netdev", "user,id=net0,hostfwd=tcp:127.0.0.1:2242-:22",
    "-device", "virtio-net-pci,netdev=net0", "-object", "rng-builtin,id=rng0", "-device", "virtio-rng-pci,rng=rng0",
    "-kernel", "ifs.bin", "-display", "none", "-vga", "none",
    "-chardev", "socket,id=s0,host=127.0.0.1,port=5520,server=on,wait=off,logfile=$log", "-serial", "chardev:s0",
    "-pidfile", "qemu.pid")
$p = Start-Process -FilePath $Qemu -WorkingDirectory $Image -ArgumentList $a -PassThru -WindowStyle Hidden `
    -RedirectStandardError (Join-Path $Image "qemu-$stamp.err") -RedirectStandardOutput (Join-Path $Image "qemu-$stamp.out")
"started pid $($p.Id); ssh 127.0.0.1:2242; serial 127.0.0.1:5520; console $Image\$log"
