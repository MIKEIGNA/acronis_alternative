#include <windows.h>
#include <winioctl.h>       // For IOCTL_DISK_GET_DRIVE_LAYOUT_EX
#include <vss.h>
#include <vswriter.h>
#include <vsbackup.h>
#include <iostream>
#include <fstream>
#include <string>
#include <filesystem>
#include <comdef.h>
#include <memory>
#include <algorithm>

// Link with vssapi.lib (MSVC will also link needed Windows libraries)
#pragma comment(lib, "vssapi.lib")

// Helper macro for HRESULT checking and logging
#define CHECK_HR_AND_FAIL(hr, msg) \
    if (FAILED(hr)) { \
        std::cerr << msg << " (hr=0x" << std::hex << hr << ")\n"; \
        return false; \
    }

//
// VSSFileLevelBackup performs a file-level backup (copies files from the shadow copy)
// using VSS to obtain a consistent snapshot of a given volume.
//
class VSSFileLevelBackup {
private:
    IVssBackupComponents* backupComponents = nullptr;
    VSS_ID snapshotSetId = GUID_NULL;
    VSS_ID snapshotId = GUID_NULL;
    std::wstring sourceDrive;
    std::wstring destFolder;

public:
    VSSFileLevelBackup(const std::wstring& source, const std::wstring& destination)
        : sourceDrive(source), destFolder(destination) {
    }

    ~VSSFileLevelBackup() {
        if (backupComponents) {
            backupComponents->Release();
        }
        CoUninitialize();
    }

    bool Initialize() {
        HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
        CHECK_HR_AND_FAIL(hr, "Failed to initialize COM");

        hr = CreateVssBackupComponents(&backupComponents);
        CHECK_HR_AND_FAIL(hr, "Failed to create VSS backup components");

        hr = backupComponents->InitializeForBackup();
        CHECK_HR_AND_FAIL(hr, "Failed to initialize for backup");

        hr = backupComponents->SetBackupState(true, true, VSS_BT_FULL, false);
        CHECK_HR_AND_FAIL(hr, "Failed to set backup state");

        return true;
    }

    bool CreateSnapshot() {
        HRESULT hr = backupComponents->StartSnapshotSet(&snapshotSetId);
        CHECK_HR_AND_FAIL(hr, "Failed to start snapshot set");

        hr = backupComponents->AddToSnapshotSet(const_cast<LPWSTR>(sourceDrive.c_str()), GUID_NULL, &snapshotId);
        CHECK_HR_AND_FAIL(hr, "Failed to add volume to snapshot set");

        {
            IVssAsync* pAsync = nullptr;
            hr = backupComponents->PrepareForBackup(&pAsync);
            CHECK_HR_AND_FAIL(hr, "PrepareForBackup failed");
            if (pAsync) {
                hr = pAsync->Wait();
                pAsync->Release();
                CHECK_HR_AND_FAIL(hr, "PrepareForBackup Wait() failed");
            }
        }

        {
            IVssAsync* pAsyncSnapshot = nullptr;
            hr = backupComponents->DoSnapshotSet(&pAsyncSnapshot);
            CHECK_HR_AND_FAIL(hr, "DoSnapshotSet failed");
            if (pAsyncSnapshot) {
                hr = pAsyncSnapshot->Wait();
                pAsyncSnapshot->Release();
                CHECK_HR_AND_FAIL(hr, "DoSnapshotSet Wait() failed");
            }
        }
        return true;
    }

    bool FileLevelBackup() {
        VSS_SNAPSHOT_PROP snapProp;
        ZeroMemory(&snapProp, sizeof(snapProp));

        HRESULT hr = backupComponents->GetSnapshotProperties(snapshotId, &snapProp);
        if (FAILED(hr)) {
            std::cerr << "Failed to get snapshot properties (hr=0x" << std::hex << hr << ")\n";
            return false;
        }

        std::wstring shadowPath = snapProp.m_pwszSnapshotDeviceObject;
        if (shadowPath.empty()) {
            std::cerr << "Snapshot device path is empty.\n";
            VssFreeSnapshotProperties(&snapProp);
            return false;
        }
        std::wcout << L"Shadow copy device: " << shadowPath << std::endl;

        // Map the shadow copy device to a drive letter (Z:)
        std::wstring mountPoint = L"Z:";
        if (!DefineDosDeviceW(DDD_RAW_TARGET_PATH, mountPoint.c_str(), shadowPath.c_str())) {
            std::cerr << "Failed to map shadow copy to " << std::string("Z:")
                << " (error=0x" << std::hex << GetLastError() << ")\n";
            VssFreeSnapshotProperties(&snapProp);
            return false;
        }
        std::wstring srcPath = mountPoint + L"\\";
        std::wcout << L"Mounted shadow copy at: " << srcPath << std::endl;

        try {
            std::filesystem::create_directories(destFolder);
            std::filesystem::copy(srcPath, destFolder,
                std::filesystem::copy_options::recursive |
                std::filesystem::copy_options::overwrite_existing);
        }
        catch (const std::filesystem::filesystem_error& ex) {
            std::cerr << "Filesystem copy error: " << ex.what() << "\n";
            DefineDosDeviceW(DDD_RAW_TARGET_PATH | DDD_REMOVE_DEFINITION, mountPoint.c_str(), shadowPath.c_str());
            VssFreeSnapshotProperties(&snapProp);
            return false;
        }

        // Unmap the drive letter.
        if (!DefineDosDeviceW(DDD_RAW_TARGET_PATH | DDD_REMOVE_DEFINITION, mountPoint.c_str(), shadowPath.c_str())) {
            std::cerr << "Failed to remove drive mapping (error=0x" << std::hex << GetLastError() << ")\n";
        }

        VssFreeSnapshotProperties(&snapProp);
        return true;
    }

    bool Cleanup() {
        if (backupComponents) {
            IVssAsync* pAsync = nullptr;
            HRESULT hr = backupComponents->BackupComplete(&pAsync);
            if (FAILED(hr)) {
                std::cerr << "BackupComplete failed (hr=0x" << std::hex << hr << ")\n";
                return false;
            }
            if (pAsync) {
                hr = pAsync->Wait();
                pAsync->Release();
                if (FAILED(hr)) {
                    std::cerr << "BackupComplete Wait() failed (hr=0x" << std::hex << hr << ")\n";
                    return false;
                }
            }
        }
        return true;
    }
};

//
// CapturePhysicalDriveMetadata reads low-level disk metadata from a specified physical drive.
// It captures a boot record (first 4KB) and the drive's partition layout using IOCTL_DISK_GET_DRIVE_LAYOUT_EX.
// Both results are written as binary files in the destination folder.
//
bool CapturePhysicalDriveMetadata(int driveNumber, const std::wstring& destFolder) {
    // Build the physical drive path: "\\.\PhysicalDriveX"
    std::wstring drivePath = L"\\\\.\\PhysicalDrive" + std::to_wstring(driveNumber);
    HANDLE hDrive = CreateFileW(drivePath.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
        OPEN_EXISTING, 0, NULL);
    if (hDrive == INVALID_HANDLE_VALUE) {
        std::wcerr << L"Failed to open " << drivePath << L" (error=0x" << std::hex << GetLastError() << L")\n";
        return false;
    }

    // Read the first 4 KB for boot records (this should cover MBR or GPT header, etc.)
    const DWORD BOOT_RECORD_SIZE = 4096;
    std::unique_ptr<BYTE[]> bootRecord(new BYTE[BOOT_RECORD_SIZE]);
    DWORD bytesRead = 0;
    if (!ReadFile(hDrive, bootRecord.get(), BOOT_RECORD_SIZE, &bytesRead, NULL)) {
        std::wcerr << L"ReadFile for boot record failed (error=0x" << std::hex << GetLastError() << L")\n";
        CloseHandle(hDrive);
        return false;
    }

    // Write the boot record to a file in the destination folder.
    std::filesystem::path bootPath = std::filesystem::path(destFolder) / L"boot_record.bin";
    try {
        std::ofstream bootFile(bootPath, std::ios::binary);
        if (!bootFile) {
            std::wcerr << L"Failed to open " << bootPath.wstring() << L" for writing.\n";
            CloseHandle(hDrive);
            return false;
        }
        bootFile.write(reinterpret_cast<char*>(bootRecord.get()), bytesRead);
        bootFile.close();
        std::wcout << L"Boot record (" << bytesRead << L" bytes) written to " << bootPath.wstring() << std::endl;
    }
    catch (const std::exception& ex) {
        std::wcerr << L"Exception writing boot record: " << ex.what() << std::endl;
        CloseHandle(hDrive);
        return false;
    }

    // Get the drive layout information.
    DWORD outSize = sizeof(DRIVE_LAYOUT_INFORMATION_EX) + 128 * sizeof(PARTITION_INFORMATION_EX);
    std::unique_ptr<BYTE[]> layoutBuffer(new BYTE[outSize]);
    DWORD bytesReturned = 0;
    if (!DeviceIoControl(hDrive, IOCTL_DISK_GET_DRIVE_LAYOUT_EX,
        NULL, 0, layoutBuffer.get(), outSize, &bytesReturned, NULL)) {
        std::wcerr << L"IOCTL_DISK_GET_DRIVE_LAYOUT_EX failed (error=0x"
            << std::hex << GetLastError() << L")\n";
        CloseHandle(hDrive);
        return false;
    }

    // Write the raw drive layout info to a binary file.
    std::filesystem::path layoutPath = std::filesystem::path(destFolder) / L"drive_layout.bin";
    try {
        std::ofstream layoutFile(layoutPath, std::ios::binary);
        if (!layoutFile) {
            std::wcerr << L"Failed to open " << layoutPath.wstring() << L" for writing.\n";
            CloseHandle(hDrive);
            return false;
        }
        layoutFile.write(reinterpret_cast<char*>(layoutBuffer.get()), bytesReturned);
        layoutFile.close();
        std::wcout << L"Drive layout (" << bytesReturned << L" bytes) written to " << layoutPath.wstring() << std::endl;
    }
    catch (const std::exception& ex) {
        std::wcerr << L"Exception writing drive layout: " << ex.what() << std::endl;
        CloseHandle(hDrive);
        return false;
    }

    CloseHandle(hDrive);
    return true;
}

//
// Simple helper to check if drive letter Z is available.
// Returns true if not present in GetLogicalDrives bitmask.
bool isDriveLetterAvailable(wchar_t letter) {
    DWORD drives = GetLogicalDrives();
    return (drives & (1 << (letter - L'A'))) == 0;
}

//
// Check if running as administrator.
static bool IsRunningAsAdmin() {
    BOOL isAdmin = FALSE;
    PSID adminGroup = NULL;
    SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;

    if (!AllocateAndInitializeSid(&ntAuthority, 2,
        SECURITY_BUILTIN_DOMAIN_RID,
        DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &adminGroup))
    {
        std::cerr << "Failed to initialize SID\n";
        return false;
    }

    if (!CheckTokenMembership(NULL, adminGroup, &isAdmin)) {
        FreeSid(adminGroup);
        std::cerr << "Failed to check token membership\n";
        return false;
    }

    FreeSid(adminGroup);
    return (isAdmin == TRUE);
}

//
// Main: Performs a VSS file-level backup and captures disk metadata.
//
int wmain() {
    if (!IsRunningAsAdmin()) {
        std::wcerr << L"This program requires administrator privileges.\n";
        return 1;
    }

    std::wstring volume;
    std::wstring destFolder;
    std::wstring driveNumStr;

    std::wcout << L"Enter volume to snapshot (e.g., C:\\): ";
    std::getline(std::wcin, volume);
    if (volume.empty()) {
        volume = L"C:\\";
    }

    std::wcout << L"Enter destination folder for backup (e.g., D:\\Backup\\SystemImage): ";
    std::getline(std::wcin, destFolder);
    if (destFolder.empty()) {
        std::wcerr << L"No destination folder provided.\n";
        return 1;
    }

    std::wcout << L"Enter physical drive number for metadata capture (e.g., 0 for \\\\.\\PhysicalDrive0): ";
    std::getline(std::wcin, driveNumStr);
    int driveNumber = 0;
    if (!driveNumStr.empty()) {
        try {
            driveNumber = std::stoi(driveNumStr);
        }
        catch (...) {
            std::wcerr << L"Invalid drive number. Defaulting to 0.\n";
            driveNumber = 0;
        }
    }

    // Check that drive letter Z is available for our VSS mount.
    if (!isDriveLetterAvailable(L'Z')) {
        std::wcerr << L"Drive letter Z is in use. Please free it or choose a different letter.\n";
        return 1;
    }

    // Perform VSS file-level backup.
    VSSFileLevelBackup backup(volume, destFolder);
    if (!backup.Initialize()) {
        std::cerr << "VSS Initialization failed.\n";
        return 1;
    }

    std::cout << "Creating VSS snapshot...\n";
    if (!backup.CreateSnapshot()) {
        std::cerr << "CreateSnapshot failed.\n";
        return 1;
    }

    std::cout << "Performing file-level backup...\n";
    if (!backup.FileLevelBackup()) {
        std::cerr << "FileLevelBackup failed.\n";
    }

    std::cout << "Cleaning up VSS snapshot...\n";
    if (!backup.Cleanup()) {
        std::cerr << "BackupComplete failed.\n";
    }

    // Capture additional disk metadata (boot record and partition layout)
    std::cout << "Capturing physical drive metadata...\n";
    if (!CapturePhysicalDriveMetadata(driveNumber, destFolder)) {
        std::cerr << "Physical drive metadata capture failed.\n";
    }

    std::cout << "Backup finished.\n";
    return 0;
}
