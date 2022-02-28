/*
 * BootMaster/apple.c
 * Functions specific to Apple computers
 *
 * Copyright (c) 2015 Roderick W. Smith
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */
/*
 * Modified for RefindPlus
 * Copyright (c) 2020-2022 Dayo Akanji (sf.net/u/dakanji/profile)
 *
 * Modifications distributed under the preceding terms.
 */

#include "global.h"
#include "config.h"
#include "lib.h"
#include "apple.h"
#include "mystrings.h"
#include "screenmgt.h"
#include "../include/refit_call_wrapper.h"

CHAR16    *gCsrStatus     = NULL;
BOOLEAN    MuteLogger     = FALSE;
BOOLEAN    MsgNormalised  = FALSE;

// Get CSR (Apple's Configurable Security Restrictions; aka System Integrity
// Protection [SIP], or "rootless") status information. If the variable is not
// present and the firmware is Apple, fake it and claim it is enabled, since
// that is how MacOS 10.11 treats a system with the variable absent.
EFI_STATUS GetCsrStatus (
    IN OUT UINT32 *CsrStatus
) {
    EFI_STATUS  Status;
    UINTN       CsrLength;
    UINT32     *ReturnValue  = NULL;
    EFI_GUID    CsrGuid      = APPLE_GUID;

    Status = EfivarGetRaw (
        &CsrGuid,
        L"csr-active-config",
        (VOID **) &ReturnValue,
        &CsrLength
    );
    if (EFI_ERROR(Status)) {
        if (Status == EFI_NOT_FOUND) {
            *CsrStatus = SIP_ENABLED_EX;
            gCsrStatus = StrDuplicate (L"SIP/SSV Enabled (Cleared/Empty)");

            // Treat as Success
            Status = EFI_SUCCESS;
        }
        else {
            gCsrStatus = StrDuplicate (L"Error While Getting SIP/SSV Status");
        }

        // Early Return ... Return Status
        return Status;
    }

    if (CsrLength != 4) {
        gCsrStatus = StrDuplicate (L"Unknown SIP/SSV Status");

        // Early Return ... Return Error
        return EFI_BAD_BUFFER_SIZE;
    }

    *CsrStatus = *ReturnValue;

    return Status;
} // EFI_STATUS GetCsrStatus()

// Store string describing CSR status value in gCsrStatus variable, which appears
// on the Info page. If DisplayMessage is TRUE, displays the new value of
// gCsrStatus on the screen for four seconds.
VOID RecordgCsrStatus (
    UINT32  CsrStatus,
    BOOLEAN DisplayMessage
) {
    CHAR16   *MsgStr    = NULL;
    EG_PIXEL  BGColor   = COLOR_LIGHTBLUE;
    BOOLEAN   CheckMute = FALSE;

    switch (CsrStatus) {
        // SIP "Cleared" Setting
        case SIP_ENABLED_EX:
            gCsrStatus = PoolPrint (L"SIP Enabled (Cleared/Empty)");
            break;

        // SIP "Enabled" Setting
        case SIP_ENABLED:
            gCsrStatus = PoolPrint (
                L"SIP Enabled (0x%04x)",
                CsrStatus
            );

        break;
        // SIP "Disabled" Settings
        case SIP_DISABLED:
        case SIP_DISABLED_B:
        case SIP_DISABLED_EX:
        case SIP_DISABLED_DBG:
        case SIP_DISABLED_KEXT:
        case SIP_DISABLED_EXTRA:
            gCsrStatus = PoolPrint (
                L"SIP Disabled (0x%04x)",
                CsrStatus
            );

        break;
        // SSV "Disabled" Settings
        case SSV_DISABLED:
        case SSV_DISABLED_B:
        case SSV_DISABLED_EX:
            gCsrStatus = PoolPrint (
                L"SIP/SSV Disabled (0x%04x)",
                CsrStatus
            );

        break;
        // Recognised Custom SIP "Disabled" Settings
        case SSV_DISABLED_ANY:
        case SSV_DISABLED_KEXT:
        case SSV_DISABLED_ANY_EX:
            gCsrStatus = PoolPrint (
                L"SIP/SSV Disabled (0x%04x - Custom Setting)",
                CsrStatus
            );

        break;
        // Wide Open and Max Legal CSR "Disabled" Settings
        case SSV_DISABLED_WIDE_OPEN:
        case CSR_MAX_LEGAL_VALUE:
            gCsrStatus = PoolPrint (
                L"SIP/SSV Removed (0x%04x - Caution!)",
                CsrStatus
            );

        break;
        // Unknown Custom Setting
        default:
            gCsrStatus = PoolPrint (
                L"SIP/SSV Disabled: 0x%04x - Caution: Unknown Custom Setting",
                CsrStatus
            );
    } // switch

    if (DisplayMessage) {
        MsgStr = (MsgNormalised)
            ? PoolPrint (L"Normalised CSR:- '%s'", gCsrStatus)
            : PoolPrint (L"%s", gCsrStatus);

        #if REFIT_DEBUG > 0
        LOG_MSG(
            "%s    * %s%s",
            (MsgNormalised) ? OffsetNext : L"",
            MsgStr,
            (MsgNormalised) ? L"" : L"\n\n"
        );
        #endif

        MY_MUTELOGGER_SET;
        egDisplayMessage (MsgStr, &BGColor, CENTER);
        PauseSeconds (4);
        MY_MUTELOGGER_OFF;

        MY_FREE_POOL(MsgStr);
    }
} // VOID RecordgCsrStatus()

// Find the current CSR status and reset it to the next one in the
// GlobalConfig.CsrValues list, or to the first value if the current
// value is not on the list.
VOID RotateCsrValue (VOID) {
    EFI_STATUS    Status;
    UINT32        CurrentValue, TargetCsr;
    UINT32        StorageFlags = APPLE_FLAGS;
    EFI_GUID      CsrGuid      = APPLE_GUID;
    UINT32_LIST  *ListItem;

    #if REFIT_DEBUG > 0
    ALT_LOG(1, LOG_LINE_SEPARATOR, L"Rotating SIP Value");
    #endif

    Status = GetCsrStatus (&CurrentValue);
    if (EFI_ERROR(Status) || !GlobalConfig.CsrValues) {
        gCsrStatus = StrDuplicate (L"Could Not Retrieve SIP/SSV Status");

        #if REFIT_DEBUG > 0
        ALT_LOG(1, LOG_LINE_NORMAL, gCsrStatus);
        #endif

        EG_PIXEL BGColor = COLOR_LIGHTBLUE;
        egDisplayMessage (
            gCsrStatus,
            &BGColor,
            CENTER
        );
        PauseSeconds (4);

        // Early Return
        return;
    }

    ListItem = GlobalConfig.CsrValues;
    while ((ListItem != NULL) && (ListItem->Value != CurrentValue)) {
        ListItem = ListItem->Next;
    } // while

    TargetCsr = (ListItem == NULL || ListItem->Next == NULL)
        ? GlobalConfig.CsrValues->Value
        : ListItem->Next->Value;

    #if REFIT_DEBUG > 0
    if (TargetCsr == 0) {
        // Set target CSR value to NULL
        ALT_LOG(1, LOG_LINE_NORMAL,
            L"Clearing SIP to 'NULL' from '0x%04x'",
            CurrentValue
        );
    }
    else if (CurrentValue == 0) {
        ALT_LOG(1, LOG_LINE_NORMAL,
            L"Setting SIP to '0x%04x' from 'NULL'",
            TargetCsr
        );
    }
    else {
        ALT_LOG(1, LOG_LINE_NORMAL,
            L"Setting SIP to '0x%04x' from '0x%04x'",
            CurrentValue, TargetCsr
        );
    }
    #endif

    Status = (TargetCsr != 0)
        ? EfivarSetRaw (
            &CsrGuid, L"csr-active-config",
            &TargetCsr, 4, TRUE
        )
        : REFIT_CALL_5_WRAPPER(
            gRT->SetVariable, L"csr-active-config",
            &CsrGuid, StorageFlags, 0, NULL
        );
    if (EFI_ERROR(Status)) {
        gCsrStatus = StrDuplicate (L"Error While Setting SIP/SSV");

        #if REFIT_DEBUG > 0
        ALT_LOG(1, LOG_LINE_NORMAL, gCsrStatus);
        #endif

        EG_PIXEL BGColor = COLOR_LIGHTBLUE;
        egDisplayMessage (
            gCsrStatus,
            &BGColor,
            CENTER
        );
        PauseSeconds (4);

        // Early Return
        return;
    }

    RecordgCsrStatus (TargetCsr, TRUE);

    #if REFIT_DEBUG > 0
    ALT_LOG(1, LOG_LINE_NORMAL,
        L"Successfully Set SIP/SSV:- '0x%04x'",
        TargetCsr
    );
    #endif
} // VOID RotateCsrValue()


EFI_STATUS NormaliseCSR (VOID) {
    EFI_STATUS  Status;
    UINT32      OurCSR;
    BOOLEAN     CheckMute = FALSE;

    MY_MUTELOGGER_SET;
    Status = GetCsrStatus (&OurCSR);  // Get csr-active-config value
    MY_MUTELOGGER_OFF;
    // Restore logging

    if (EFI_ERROR(Status)) {
        if (Status == EFI_NOT_FOUND) {
            // csr-active-config not found ... Proceed as 'OK'
            Status = EFI_ALREADY_STARTED;
        }

        // Early Return ... Return Status
        return Status;
    }

    if ((OurCSR & CSR_ALLOW_APPLE_INTERNAL) == 0) {
        // 'CSR_ALLOW_APPLE_INTERNAL' bit not present ... Proceed as 'OK'
        return EFI_ALREADY_STARTED;
    }

    // 'CSR_ALLOW_APPLE_INTERNAL' bit present ... Clear and Reset
    MsgNormalised = TRUE;
    OurCSR &= ~CSR_ALLOW_APPLE_INTERNAL;
    RecordgCsrStatus (OurCSR, TRUE);
    MsgNormalised = FALSE;

    return EFI_SUCCESS;
} // EFI_STATUS NormaliseCSR()


/*
 * The definitions below and the SetAppleOSInfo() function are based on a GRUB patch by Andreas Heider:
 * https://lists.gnu.org/archive/html/grub-devel/2013-12/msg00442.html
 */

#define EFI_APPLE_SET_OS_PROTOCOL_GUID  { 0xc5c5da95, 0x7d5c, 0x45e6, \
    { 0xb2, 0xf1, 0x3f, 0xd5, 0x2b, 0xb1, 0x00, 0x77 } }

typedef struct EfiAppleSetOsInterface {
    UINT64 Version;
    EFI_STATUS EFIAPI (*SetOsVersion) (IN CHAR8 *Version);
    EFI_STATUS EFIAPI (*SetOsVendor) (IN CHAR8 *Vendor);
} EfiAppleSetOsInterface;

// Function to tell the firmware that MacOS is being launched. This is
// required to work around problems on some Macs that do not fully
// initialize some hardware (especially video displays) when third-party
// OSes are launched in EFI mode.
EFI_STATUS SetAppleOSInfo (VOID) {
    EFI_STATUS               Status;
    EFI_GUID                 apple_set_os_guid  = EFI_APPLE_SET_OS_PROTOCOL_GUID;
    CHAR16                  *AppleVersionOS     = NULL;
    CHAR8                   *MacVersionStr      = NULL;
    EfiAppleSetOsInterface  *SetOs              = NULL;

    if (GlobalConfig.SpoofOSXVersion == NULL) {
        // Early Return ... Treat as success
        return EFI_SUCCESS;
    }

    Status = REFIT_CALL_3_WRAPPER(
        gBS->LocateProtocol,
        &apple_set_os_guid,
        NULL,
        (VOID **) &SetOs
    );
    if (EFI_ERROR(Status) || SetOs == NULL) {
        #if REFIT_DEBUG > 0
        ALT_LOG(1, LOG_LINE_NORMAL,
            L"Not a Mac ... Not Setting MacOS Information"
        );
        #endif

        // Early Return ... Treat as success
        return EFI_SUCCESS;
    }

    if (SetOs->Version == 0) {
        // Early Return ... Treat as success
        return EFI_SUCCESS;
    }

    AppleVersionOS = StrDuplicate (L"Mac OS X");
    if (AppleVersionOS == NULL) {
        // Early Return ... Out of Memory
        return EFI_OUT_OF_RESOURCES;
    }

    MergeStrings (&AppleVersionOS, GlobalConfig.SpoofOSXVersion, ' ');

    MacVersionStr = AllocateZeroPool (
        (StrLen (AppleVersionOS) + 1) * sizeof (CHAR8)
    );
    if (MacVersionStr == NULL) {
        MY_FREE_POOL(AppleVersionOS);

        // Early Return ... Out of Memory
        return EFI_OUT_OF_RESOURCES;
    }

    #if REFIT_DEBUG > 0
    ALT_LOG(1, LOG_LINE_NORMAL,
        L"Set MacOS Information:- '%s'",
        AppleVersionOS
    );
    #endif

    UnicodeStrToAsciiStr (AppleVersionOS, MacVersionStr);
    Status = REFIT_CALL_1_WRAPPER(
        SetOs->SetOsVersion, MacVersionStr
    );

    MY_FREE_POOL(MacVersionStr);
    MY_FREE_POOL(AppleVersionOS);

    if (EFI_ERROR(Status)) {
        // Early Return ... Return Error
        return Status;
    }

    if (SetOs->Version >= 2) {
        REFIT_CALL_1_WRAPPER(
            SetOs->SetOsVendor, (CHAR8 *) "Apple Inc."
        );
    }

    return EFI_SUCCESS;
} // EFI_STATUS SetAppleOSInfo()



//
// APFS Volume Role Identification
//

/**
 * Copyright (C) 2019, vit9696
 *
 * All rights reserved.
 *
 * This program and the accompanying materials
 * are licensed and made available under the terms and conditions of the BSD License
 * which accompanies this distribution.  The full text of the license may be found at
 * http://opensource.org/licenses/bsd-license.php
 *
 * THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
 * WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.
**/
/**
 * Modified for RefindPlus
 * Copyright (c) 2021 Dayo Akanji (sf.net/u/dakanji/profile)
 *
 * Modifications distributed under the preceding terms.
**/

#ifdef __MAKEWITH_TIANO
// DA-TAG: Limit to TianoCore - START

extern
BOOLEAN OcOverflowAddUN (
    UINTN   A,
    UINTN   B,
    UINTN  *Result
);
extern
VOID * OcReadFile (
    IN  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *FileSystem,
    IN  CONST CHAR16                     *FilePath,
    OUT UINT32                           *FileSize    OPTIONAL,
    IN  UINT32                            MaxFileSize OPTIONAL
);
extern
UINTN OcFileDevicePathNameSize (
    IN CONST FILEPATH_DEVICE_PATH  *FilePath
);

static
CHAR16 * RP_GetBootPathName (
    IN  EFI_DEVICE_PATH_PROTOCOL  *DevicePath
) {
    UINTN                            Len;
    UINTN                            Size;
    UINTN                            PathNameSize;
    CHAR16                          *PathName;
    CHAR16                          *FilePathName;
    FILEPATH_DEVICE_PATH            *FilePath;

    if (DevicePathType    (DevicePath) != MEDIA_DEVICE_PATH ||
        DevicePathSubType (DevicePath) != MEDIA_FILEPATH_DP
    ) {
        PathName = AllocateZeroPool (sizeof (L"\\"));
        if (PathName == NULL) {
            // Early Return ... Return NULL
            return NULL;
        }

        StrCpyS (PathName, sizeof (L"\\"), L"\\");

        // Early Return ... Return Output
        return PathName;
    }

    FilePath     = (FILEPATH_DEVICE_PATH *) DevicePath;
    Size         = OcFileDevicePathNameSize (FilePath);
    PathNameSize = Size + sizeof (CHAR16);

    PathName = AllocateZeroPool (PathNameSize);
    if (PathName == NULL) {
        // Early Return ... Return NULL
        return NULL;
    }

    CopyMem (PathName, FilePath->PathName, Size);
    if (!MyStrStr (PathName, L"\\")) {
        StrCpyS (PathName, PathNameSize, L"\\");

        // Early Return ... Return Output
        return PathName;
    }

    Len = StrLen (PathName);
    FilePathName = &PathName[Len - 1];
    while (*FilePathName != L'\\') {
        *FilePathName = L'\0';
        --FilePathName;
    } // while

    return PathName;
} // static EFI_STATUS RP_GetBootPathName

static
CHAR16 * RP_GetAppleDiskLabelEx (
    IN  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *FileSystem,
    IN  CHAR16                           *BootDirectoryName,
    IN  CONST CHAR16                     *LabelFilename
) {
    UINTN     DiskLabelPathSize;
    UINTN     MaxVolumelabelSize = 64;
    CHAR8    *AsciiDiskLabel;
    CHAR16   *DiskLabelPath;
    CHAR16   *UnicodeDiskLabel = NULL;
    UINT32    DiskLabelLength;

    DiskLabelPathSize = StrSize (BootDirectoryName) + StrSize (LabelFilename) - sizeof (CHAR16);

    DiskLabelPath = AllocatePool (DiskLabelPathSize);
    if (DiskLabelPath == NULL) {
        // Early Return ... Return NULL
        return NULL;
    }

    UnicodeSPrint (DiskLabelPath, DiskLabelPathSize, L"%s%s", BootDirectoryName, LabelFilename);

    AsciiDiskLabel = (CHAR8 *) OcReadFile (
        FileSystem,
        DiskLabelPath,
        &DiskLabelLength,
        MaxVolumelabelSize
    );

    MY_FREE_POOL(DiskLabelPath);

    if (AsciiDiskLabel == NULL) {
        // Early Return ... Return NULL
        return NULL;
    }

    UnicodeDiskLabel = MyAsciiStrCopyToUnicode (AsciiDiskLabel, DiskLabelLength);

    MY_FREE_POOL(AsciiDiskLabel);

    if (UnicodeDiskLabel == NULL) {
        // Early Return ... Return NULL
        return NULL;
    }

    MyUnicodeFilterString (UnicodeDiskLabel, TRUE);

    return UnicodeDiskLabel;
} // static CHAR16 * RP_GetAppleDiskLabelEx()

CHAR16 * RP_GetAppleDiskLabel (
    IN  REFIT_VOLUME *Volume
) {
    EFI_STATUS                        Status;
    CHAR16                           *BootDirectoryName;
    CHAR16                           *AppleDiskLabel = NULL;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *FileSystem;

    BootDirectoryName = RP_GetBootPathName (Volume->DevicePath);
    if (BootDirectoryName == NULL) {
        // Early Return ... Return NULL
        return NULL;
    }

    Status = gBS->HandleProtocol (
        Volume->DeviceHandle,
        &gEfiSimpleFileSystemProtocolGuid,
        (VOID **) &FileSystem
    );
    if (EFI_ERROR (Status)) {
        MY_FREE_POOL(BootDirectoryName);

        // Early Return ... Return NULL
        return NULL;
    }

    AppleDiskLabel = RP_GetAppleDiskLabelEx (
        FileSystem,
        BootDirectoryName,
        L".contentDetails"
    );

    if (AppleDiskLabel == NULL) {
        AppleDiskLabel = RP_GetAppleDiskLabelEx (
            FileSystem,
            BootDirectoryName,
            L".disk_label.contentDetails"
        );
    }
    MY_FREE_POOL(BootDirectoryName);

    return AppleDiskLabel;
} // CHAR16 * RP_GetAppleDiskLabel()

static
VOID * RP_GetFileInfo (
    IN  EFI_FILE_PROTOCOL  *File,
    IN  EFI_GUID           *InformationType,
    IN  UINTN               MinFileInfoSize,
    OUT UINTN              *RealFileInfoSize  OPTIONAL
) {
    EFI_STATUS  Status;
    UINTN       FileInfoSize;
    VOID       *FileInfoBuffer;

    FileInfoSize   = 0;
    FileInfoBuffer = NULL;

    Status = File->GetInfo (
        File,
        InformationType,
        &FileInfoSize,
        NULL
    );
    if (Status != EFI_BUFFER_TOO_SMALL ||
        FileInfoSize < MinFileInfoSize
    ) {
        // Early Return ... Return NULL
        return NULL;
    }

    // Some drivers (i.e. built-in 32-bit Apple HFS driver) may possibly
    // omit null terminators from file info data.
    if (CompareGuid (InformationType, &gEfiFileInfoGuid) &&
        OcOverflowAddUN (FileInfoSize, sizeof (CHAR16), &FileInfoSize)
    ) {
        // Early Return ... Return NULL
        return NULL;
    }

    FileInfoBuffer = AllocateZeroPool (FileInfoSize);
    if (FileInfoBuffer == NULL) {
        // Early Return ... Return NULL
        return NULL;
    }

    Status = File->GetInfo (
        File,
        InformationType,
        &FileInfoSize,
        FileInfoBuffer
    );
    if (EFI_ERROR(Status)) {
        MY_FREE_POOL(FileInfoBuffer);

        // Early Return ... Return NULL
        return NULL;
    }

    if (RealFileInfoSize != NULL) {
        *RealFileInfoSize = FileInfoSize;
    }

    return FileInfoBuffer;
} // static VOID * RP_GetFileInfo()

static
EFI_STATUS RP_GetApfsSpecialFileInfo (
    IN     EFI_FILE_PROTOCOL           *Root,
    IN OUT APPLE_APFS_VOLUME_INFO     **VolumeInfo    OPTIONAL,
    IN OUT APPLE_APFS_CONTAINER_INFO  **ContainerInfo OPTIONAL
) {
    EFI_GUID AppleApfsVolumeInfoGuid    = APPLE_APFS_VOLUME_INFO_GUID;
    EFI_GUID AppleApfsContainerInfoGuid = APPLE_APFS_CONTAINER_INFO_GUID;

    if (ContainerInfo == NULL && VolumeInfo == NULL) {
        return EFI_INVALID_PARAMETER;
    }

    if (VolumeInfo != NULL) {
        *VolumeInfo = RP_GetFileInfo (
            Root,
            &AppleApfsVolumeInfoGuid,
            sizeof (**VolumeInfo),
            NULL
        );

        if (*VolumeInfo == NULL) {
            // Early Return ... Return Error
            return EFI_NOT_FOUND;
        }
    }

    if (ContainerInfo != NULL) {
        *ContainerInfo = RP_GetFileInfo (
            Root,
            &AppleApfsContainerInfoGuid,
            sizeof (**ContainerInfo),
            NULL
        );

        if (*ContainerInfo == NULL) {
            MY_FREE_POOL(*VolumeInfo);

            // Early Return ... Return Error
            return EFI_NOT_FOUND;
        }
    }

    return EFI_SUCCESS;
} // static EFI_STATUS RP_GetApfsSpecialFileInfo()

EFI_STATUS RP_GetApfsVolumeInfo (
    IN  EFI_HANDLE               Device,
    OUT EFI_GUID                *ContainerGuid OPTIONAL,
    OUT EFI_GUID                *VolumeGuid    OPTIONAL,
    OUT APPLE_APFS_VOLUME_ROLE  *VolumeRole    OPTIONAL
) {
    EFI_STATUS                       Status;
    EFI_FILE_PROTOCOL               *Root;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *FileSystem;
    APPLE_APFS_CONTAINER_INFO       *ApfsContainerInfo;
    APPLE_APFS_VOLUME_INFO          *ApfsVolumeInfo;

    if (ContainerGuid == NULL
        && VolumeGuid == NULL
        && VolumeRole == NULL
    ) {
        // Early Return ... Return Error
        return EFI_INVALID_PARAMETER;
    }

    Root = NULL;

    Status = gBS->HandleProtocol (
        Device,
        &gEfiSimpleFileSystemProtocolGuid,
        (VOID **) &FileSystem
    );
    if (EFI_ERROR(Status)) {
        // Early Return ... Return Error
        return Status;
    }

    Status = FileSystem->OpenVolume (FileSystem, &Root);
    if (EFI_ERROR(Status)) {
        // Early Return ... Return Error
        return Status;
    }

    Status = RP_GetApfsSpecialFileInfo (Root, &ApfsVolumeInfo, &ApfsContainerInfo);
    Root->Close (Root);
    if (EFI_ERROR(Status)) {
        // Early Return ... Return Error
        return EFI_NOT_FOUND;
    }

    if (VolumeGuid) {
        CopyGuid (
            VolumeGuid,
            &ApfsVolumeInfo->Uuid
        );
    }

    if (VolumeRole) {
        *VolumeRole = ApfsVolumeInfo->Role;
    }

    if (ContainerGuid) {
        CopyGuid (
            ContainerGuid,
            &ApfsContainerInfo->Uuid
        );
    }

    MY_FREE_POOL(ApfsVolumeInfo);
    MY_FREE_POOL(ApfsContainerInfo);

    return EFI_SUCCESS;
} // EFI_STATUS RP_GetApfsVolumeInfo()

// DA-TAG: Limit to TianoCore - END
#endif

// Delete recovery-boot-mode/internet-recovery-mode flags
VOID ClearRecoveryBootFlag (VOID) {
    EFI_STATUS  Status;
    UINTN       BufferSize;
    VOID       *TmpBuffer    = NULL;
    CHAR16     *VariableName = NULL;
    UINT32      StorageFlags = APPLE_FLAGS;
    EFI_GUID    AppleGuid    = APPLE_GUID;

    BufferSize   = 0;
    VariableName = L"recovery-boot-mode";
    Status = REFIT_CALL_5_WRAPPER(
        gRT->GetVariable,
        VariableName,
        &AppleGuid,
        NULL,
        &BufferSize,
        TmpBuffer
    );
    if (Status == EFI_BUFFER_TOO_SMALL || BufferSize != 0) {
        REFIT_CALL_5_WRAPPER(
            gRT->SetVariable,
            VariableName,
            &AppleGuid,
            StorageFlags,
            0, NULL
        );
        MY_FREE_POOL(TmpBuffer);
    }

    BufferSize   = 0;
    VariableName = L"internet-recovery-mode";
    Status = REFIT_CALL_5_WRAPPER(
        gRT->GetVariable,
        VariableName,
        &AppleGuid,
        NULL,
        &BufferSize,
        TmpBuffer
    );
    if (Status == EFI_BUFFER_TOO_SMALL || BufferSize != 0) {
        REFIT_CALL_5_WRAPPER(
            gRT->SetVariable,
            VariableName,
            &AppleGuid,
            StorageFlags,
            0, NULL
        );
        MY_FREE_POOL(TmpBuffer);
    }

    BufferSize   = 0;
    VariableName = L"RecoveryBootInitiator";
    Status = REFIT_CALL_5_WRAPPER(
        gRT->GetVariable,
        VariableName,
        &AppleGuid,
        NULL,
        &BufferSize,
        TmpBuffer
    );
    if (Status == EFI_BUFFER_TOO_SMALL || BufferSize != 0) {
        REFIT_CALL_5_WRAPPER(
            gRT->SetVariable,
            VariableName,
            &AppleGuid,
            StorageFlags,
            0, NULL
        );
        MY_FREE_POOL(TmpBuffer);
    }
} // VOID ClearRecoveryBootFlag()
