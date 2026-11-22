param(
    [switch]$Unregister
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Este script requiere ejecutarse como Administrador para registrar el servidor COM en HKLM."
}

$programDataDir = "C:\ProgramData\ZeroCam"
$installedDll = Join-Path $programDataDir "ZeroCamSource.dll"
$ipcFile = Join-Path $programDataDir "ipc_ring.bin"
$ipcMinBytes = 6220928 # sizeof(zerocam::ZeroCamSharedMemory)
$clsidRootKey = "HKLM:\SOFTWARE\Classes\CLSID\{8F4A9C12-6E3B-4D71-9A25-3C8E71B40D62}"
$clsidKey = "$clsidRootKey\InprocServer32"

function Assert-NotReparsePoint([string]$PathToCheck) {
    if (Test-Path -LiteralPath $PathToCheck) {
        $item = Get-Item -LiteralPath $PathToCheck -Force
        if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Se detecto un enlace simbolico o reparse point no permitido: $PathToCheck"
        }
    }
}

# Helper nativo Win32 para validar hardlinks/reparse points y aplicar DACL + SACL Mandatory Label (NW,NR) por handle.
if (-not ("ZeroCamAclNative" -as [type])) {
    Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;

public static class ZeroCamAclNative {
    private const uint FILE_TYPE_DISK = 1;
    private const uint FILE_ATTRIBUTE_DIRECTORY = 0x10;
    private const uint FILE_ATTRIBUTE_REPARSE_POINT = 0x400;
    private const uint FILE_FLAG_OPEN_REPARSE_POINT = 0x00200000;
    private const uint FILE_FLAG_BACKUP_SEMANTICS = 0x02000000;
    private const uint FILE_SHARE_READ = 1;
    private const uint FILE_SHARE_WRITE = 2;
    private const uint FILE_SHARE_DELETE = 4;
    private const uint OPEN_EXISTING = 3;
    private const uint OPEN_ALWAYS = 4;
    private const uint GENERIC_READ = 0x80000000;
    private const uint GENERIC_WRITE = 0x40000000;
    private const uint READ_CONTROL = 0x00020000;
    private const uint WRITE_DAC = 0x00040000;
    private const uint WRITE_OWNER = 0x00080000;
    private const uint DACL_SECURITY_INFORMATION = 0x00000004;
    private const uint LABEL_SECURITY_INFORMATION = 0x00000010;
    private const uint PROTECTED_DACL_SECURITY_INFORMATION = 0x80000000;
    private const int SE_FILE_OBJECT = 1;

    [StructLayout(LayoutKind.Sequential)]
    private struct FILETIME { public uint Low; public uint High; }

    [StructLayout(LayoutKind.Sequential)]
    private struct BY_HANDLE_FILE_INFORMATION {
        public uint FileAttributes;
        public FILETIME CreationTime;
        public FILETIME LastAccessTime;
        public FILETIME LastWriteTime;
        public uint VolumeSerialNumber;
        public uint FileSizeHigh;
        public uint FileSizeLow;
        public uint NumberOfLinks;
        public uint FileIndexHigh;
        public uint FileIndexLow;
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr CreateFileW(
        string fileName, uint access, uint share, IntPtr sa, uint disp, uint flags, IntPtr tmpl);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool CloseHandle(IntPtr h);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern uint GetFileType(IntPtr h);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool GetFileInformationByHandle(IntPtr h, out BY_HANDLE_FILE_INFORMATION info);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool GetFileSizeEx(IntPtr h, out long size);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool SetFilePointerEx(IntPtr h, long dist, out long newPos, uint moveMethod);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool SetEndOfFile(IntPtr h);

    [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool ConvertStringSecurityDescriptorToSecurityDescriptorW(
        string sddl, uint rev, out IntPtr sd, IntPtr size);

    [DllImport("advapi32.dll", SetLastError = true)]
    private static extern bool GetSecurityDescriptorDacl(
        IntPtr sd, out bool present, out IntPtr dacl, out bool defaulted);

    [DllImport("advapi32.dll", SetLastError = true)]
    private static extern bool GetSecurityDescriptorSacl(
        IntPtr sd, out bool present, out IntPtr sacl, out bool defaulted);

    [DllImport("advapi32.dll", SetLastError = true)]
    private static extern uint SetSecurityInfo(
        IntPtr handle, int objectType, uint secInfo,
        IntPtr owner, IntPtr group, IntPtr dacl, IntPtr sacl);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr LocalFree(IntPtr hMem);

    private static readonly IntPtr INVALID_HANDLE = new IntPtr(-1);

    private static void ApplySddlToHandle(IntPtr h, string sddl, bool includeLabel) {
        IntPtr sd;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, 1, out sd, IntPtr.Zero)) {
            throw new InvalidOperationException("SDDL invalido (" + Marshal.GetLastWin32Error() + ")");
        }
        try {
            bool daclPresent, daclDef, saclPresent = false, saclDef = false;
            IntPtr dacl, sacl = IntPtr.Zero;
            if (!GetSecurityDescriptorDacl(sd, out daclPresent, out dacl, out daclDef) || !daclPresent) {
                throw new InvalidOperationException("No se pudo extraer DACL del SDDL.");
            }
            uint secInfo = DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION;
            if (includeLabel) {
                if (!GetSecurityDescriptorSacl(sd, out saclPresent, out sacl, out saclDef) || !saclPresent) {
                    throw new InvalidOperationException("No se pudo extraer Mandatory Label SACL del SDDL.");
                }
                secInfo |= LABEL_SECURITY_INFORMATION;
            }
            uint rc = SetSecurityInfo(h, SE_FILE_OBJECT, secInfo, IntPtr.Zero, IntPtr.Zero, dacl, includeLabel ? sacl : IntPtr.Zero);
            if (rc != 0) {
                throw new InvalidOperationException("SetSecurityInfo fallo con codigo Win32 " + rc);
            }
        } finally {
            LocalFree(sd);
        }
    }

    private static void ValidateRegularSingleLinkHandle(IntPtr h, string label) {
        if (GetFileType(h) != FILE_TYPE_DISK) {
            throw new InvalidOperationException("Archivo no es disco regular: " + label);
        }
        BY_HANDLE_FILE_INFORMATION info;
        if (!GetFileInformationByHandle(h, out info)) {
            throw new InvalidOperationException("GetFileInformationByHandle fallo: " + label);
        }
        if ((info.FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0) {
            throw new InvalidOperationException("Reparse point o directorio no permitido: " + label);
        }
        if (info.NumberOfLinks != 1) {
            throw new InvalidOperationException("Hardlink no permitido (nNumberOfLinks=" + info.NumberOfLinks + "): " + label);
        }
    }

    public static void ProtectDirectoryByHandle(string dirPath, string sddl) {
        IntPtr h = CreateFileW(
            dirPath, READ_CONTROL | WRITE_DAC,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            IntPtr.Zero, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, IntPtr.Zero);
        if (h == INVALID_HANDLE) {
            throw new InvalidOperationException("No se pudo abrir directorio " + dirPath + " (" + Marshal.GetLastWin32Error() + ")");
        }
        try {
            BY_HANDLE_FILE_INFORMATION info;
            if (!GetFileInformationByHandle(h, out info) ||
                (info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
                (info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                throw new InvalidOperationException("Directorio invalido o reparse point: " + dirPath);
            }
            ApplySddlToHandle(h, sddl, false);
        } finally {
            CloseHandle(h);
        }
    }

    public static void ValidateExistingFileIfPresent(string filePath) {
        IntPtr h = CreateFileW(
            filePath, READ_CONTROL,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            IntPtr.Zero, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT, IntPtr.Zero);
        if (h == INVALID_HANDLE) {
            int err = Marshal.GetLastWin32Error();
            if (err == 2 || err == 3) return; // ERROR_FILE_NOT_FOUND / ERROR_PATH_NOT_FOUND
            throw new InvalidOperationException("No se pudo inspeccionar archivo existente " + filePath + " (Win32 " + err + ")");
        }
        try {
            ValidateRegularSingleLinkHandle(h, filePath);
        } finally {
            CloseHandle(h);
        }
    }

    public static void ProtectRegularFileByHandle(string filePath, string sddl, bool includeLabel) {
        uint access = READ_CONTROL | WRITE_DAC | (includeLabel ? WRITE_OWNER : 0u);
        IntPtr h = CreateFileW(
            filePath, access,
            FILE_SHARE_READ,
            IntPtr.Zero, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT, IntPtr.Zero);
        if (h == INVALID_HANDLE) {
            throw new InvalidOperationException("No se pudo abrir " + filePath + " para ACL (Win32 " + Marshal.GetLastWin32Error() + ")");
        }
        try {
            ValidateRegularSingleLinkHandle(h, filePath);
            ApplySddlToHandle(h, sddl, includeLabel);
        } finally {
            CloseHandle(h);
        }
    }

    public static void CreateAndProtectIpcFile(string ipcPath, long exactBytes, string sddl) {
        IntPtr h = CreateFileW(
            ipcPath,
            GENERIC_READ | GENERIC_WRITE | WRITE_DAC | WRITE_OWNER,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            IntPtr.Zero, OPEN_ALWAYS,
            FILE_FLAG_OPEN_REPARSE_POINT, IntPtr.Zero);
        if (h == INVALID_HANDLE) {
            throw new InvalidOperationException("No se pudo abrir/crear IPC " + ipcPath + " (Win32 " + Marshal.GetLastWin32Error() + ")");
        }
        try {
            ValidateRegularSingleLinkHandle(h, ipcPath);
            long curSize;
            if (!GetFileSizeEx(h, out curSize)) {
                throw new InvalidOperationException("GetFileSizeEx fallo sobre " + ipcPath);
            }
            if (curSize != exactBytes) {
                long newPos;
                if (!SetFilePointerEx(h, exactBytes, out newPos, 0) || !SetEndOfFile(h)) {
                    throw new InvalidOperationException("No se pudo fijar tamano exacto de " + ipcPath);
                }
            }
            ApplySddlToHandle(h, sddl, true);
        } finally {
            CloseHandle(h);
        }
    }
}
"@
}

# Detiene FrameServer temporalmente para liberar la DLL previa y lo reinicia siempre al salir.
$wasRunning = ((Get-Service FrameServer -ErrorAction SilentlyContinue).Status -eq "Running")
if ($wasRunning) {
    Stop-Service FrameServer -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 400
}

try {
    if ($Unregister) {
        if (Test-Path $clsidRootKey) {
            Remove-Item -Path $clsidRootKey -Recurse -Force
        }
        Write-Host "[ZeroCam] Servidor COM desregistrado."
        exit 0
    }

    $sourceDll = Get-ChildItem (Join-Path $root "build") -Filter "ZeroCamSource.dll" -Recurse -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $sourceDll) {
        throw "No se encontro ZeroCamSource.dll compilada. Ejecuta primero scripts\build_windows.ps1."
    }

    Assert-NotReparsePoint $programDataDir
    if (-not (Test-Path -LiteralPath $programDataDir)) {
        New-Item -ItemType Directory -Path $programDataDir -Force | Out-Null
    }
    Assert-NotReparsePoint $programDataDir

    # Corta herencia Users:(WD,AD) de C:\ProgramData por handle ANTES de copiar la DLL o crear ipc_ring.bin (sin AC ni BU).
    [ZeroCamAclNative]::ProtectDirectoryByHandle(
        $programDataDir,
        "D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FRFX;;;LS)(A;OICI;FRFX;;;IU)"
    )

    # Valida ausencia de reparse points y hardlinks (nNumberOfLinks == 1) ANTES de sobrescribir/abrir archivos.
    [ZeroCamAclNative]::ValidateExistingFileIfPresent($installedDll)
    [ZeroCamAclNative]::ValidateExistingFileIfPresent($ipcFile)

    Copy-Item -LiteralPath $sourceDll.FullName -Destination $installedDll -Force
    [ZeroCamAclNative]::ProtectRegularFileByHandle(
        $installedDll,
        "D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FRFX;;;LS)(A;;FRFX;;;IU)",
        $false
    )

    # Pre-crea ipc_ring.bin por handle con tamano exacto de ZeroCamSharedMemory, valida nNumberOfLinks == 1 y aplica DACL + Mandatory Label Medium:(NW,NR).
    [ZeroCamAclNative]::CreateAndProtectIpcFile(
        $ipcFile,
        [long]$ipcMinBytes,
        "D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FRFW;;;LS)(A;;FRFW;;;IU)S:(ML;;NWNR;;;ME)"
    )

    & regsvr32.exe /s $installedDll
    if ($LASTEXITCODE -ne 0) {
        throw "regsvr32 fallo con codigo $LASTEXITCODE."
    }

    if (-not (Test-Path $clsidKey)) {
        throw "No se encontro la clave registrada $clsidKey"
    }

    Write-Host "[ZeroCam] Camara virtual registrada exitosamente en $installedDll"
    Get-ItemProperty $clsidKey | Select-Object '(default)', ThreadingModel | Format-List
} finally {
    Start-Service FrameServer -ErrorAction SilentlyContinue
}
