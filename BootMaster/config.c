/*
 * BootMaster/config.c
 * Configuration file functions
 *
 * Copyright (c) 2006 Christoph Pfisterer
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the
 *    distribution.
 *
 *  * Neither the name of Christoph Pfisterer nor the names of the
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Modifications copyright (c) 2012-2023 Roderick W. Smith
 *
 * Modifications distributed under the terms of the GNU General Public
 * License (GPL) version 3 (GPLv3) or (at your option) any later version.
 *
 */
/*
 * Modified for RefindPlus
 * Copyright (c) 2020-2023 Dayo Akanji (sf.net/u/dakanji/profile)
 * Portions Copyright (c) 2021 Joe van Tunen (joevt@shaw.ca)
 *
 * Modifications distributed under the preceding terms.
 */

#include "global.h"
#include "lib.h"
#include "icns.h"
#include "menu.h"
#include "config.h"
#include "screenmgt.h"
#include "apple.h"
#include "mystrings.h"
#include "scan.h"
#include "../include/refit_call_wrapper.h"
#include "../mok/mok.h"

// Constants

#define LINUX_OPTIONS_FILENAMES  L"refindplus_linux.conf,refindplus-linux.conf,refind_linux.conf,refind-linux.conf"
#define MAXCONFIGFILESIZE        (128*1024)

#define ENCODING_ISO8859_1  (0)
#define ENCODING_UTF8       (1)
#define ENCODING_UTF16_LE   (2)

#define LAST_MINUTE         (1439) /* Last minute of a day */

UINTN   ReadLoops       = 0;
UINTN   TotalEntryCount = 0;
UINTN   ValidEntryCount = 0;

BOOLEAN OuterLoop      =  TRUE;
BOOLEAN SilenceAPFS    =  TRUE;
BOOLEAN FirstInclude   =  TRUE;
BOOLEAN ManualInclude  = FALSE;
BOOLEAN FoundFontImage =  TRUE;

// Control Forensic Logging
#if REFIT_DEBUG > 1
    BOOLEAN ForensicLogging = TRUE;
#else
    BOOLEAN ForensicLogging = FALSE;
#endif

extern BOOLEAN         ForceTextOnly;

// Sets GlobalConfig.LinuxMatchPatterns based on the input comma-delimited set
// of prefixes. An asterisk ("*") is added to each of the input prefixes and
// GlobalConfig.LinuxMatchPatterns is set to the resulting comma-delimited
// string.
static
VOID SetLinuxMatchPatterns (
    CHAR16 *Prefixes
) {
    UINTN i;
    CHAR16 *Pattern;
    CHAR16 *PatternSet;

    i = 0;
    PatternSet = NULL;
    while ((Pattern = FindCommaDelimited(Prefixes, i++)) != NULL) {
        MergeStrings (&Pattern, L"*", 0);
        MergeStrings (&PatternSet, Pattern, L',');
        MY_FREE_POOL(Pattern);
    }

    MY_FREE_POOL(GlobalConfig.LinuxMatchPatterns);
    GlobalConfig.LinuxMatchPatterns = PatternSet;
} // VOID SetLinuxMatchPatterns()

EFI_STATUS RefitReadFile (
    IN     EFI_FILE_HANDLE  BaseDir,
    IN     CHAR16          *FileName,
    IN OUT REFIT_FILE      *File,
    OUT    UINTN           *size
) {
    EFI_STATUS       Status;
    EFI_FILE_HANDLE  FileHandle;
    EFI_FILE_INFO   *FileInfo;
    CHAR16          *Message;
    UINT64           ReadSize;

    File->Buffer     = NULL;
    File->BufferSize = 0;

    // read the file, allocating a buffer on the way
    Status = REFIT_CALL_5_WRAPPER(
        BaseDir->Open, BaseDir,
        &FileHandle, FileName,
        EFI_FILE_MODE_READ, 0
    );

    Message = PoolPrint (L"While Loading File:- '%s'", FileName);
    if (CheckError (Status, Message)) {
        MY_FREE_POOL(Message);

        // Early Return
        return Status;
    }

    FileInfo = LibFileInfo (FileHandle);
    if (FileInfo == NULL) {
        // TODO: print and register the error
        REFIT_CALL_1_WRAPPER(FileHandle->Close, FileHandle);

        // Early Return
        return EFI_LOAD_ERROR;
    }
    ReadSize = FileInfo->FileSize;
    MY_FREE_POOL(FileInfo);

    File->BufferSize = (UINTN) ReadSize;
    File->Buffer = AllocatePool (File->BufferSize);
    if (File->Buffer == NULL) {
       size = 0;

       // Early Return
       return EFI_OUT_OF_RESOURCES;
    }

    *size = File->BufferSize;

    Status = REFIT_CALL_3_WRAPPER(
        FileHandle->Read, FileHandle,
        &File->BufferSize, File->Buffer
    );
    if (CheckError (Status, Message)) {
        size = 0;
        MY_FREE_POOL(Message);
        MY_FREE_POOL(File->Buffer);
        REFIT_CALL_1_WRAPPER(FileHandle->Close, FileHandle);

        // Early Return
        return Status;
    }
    MY_FREE_POOL(Message);

    REFIT_CALL_1_WRAPPER(FileHandle->Close, FileHandle);

    // Setup for reading
    File->Current8Ptr  = (CHAR8 *)File->Buffer;
    File->End8Ptr      = File->Current8Ptr + File->BufferSize;
    File->Current16Ptr = (CHAR16 *)File->Buffer;
    File->End16Ptr     = File->Current16Ptr + (File->BufferSize >> 1);

    // DA_TAG: Investigate This
    //        Detect other encodings
    //        Some are also implemented
    //
    // Detect Encoding
    File->Encoding = ENCODING_ISO8859_1;   // default: 1:1 translation of CHAR8 to CHAR16
    if (File->BufferSize >= 4) {
        if (File->Buffer[0] == 0xFF && File->Buffer[1] == 0xFE) {
            // BOM in UTF-16 little endian (or UTF-32 little endian)
            File->Encoding = ENCODING_UTF16_LE;   // use CHAR16 as is
            File->Current16Ptr++;
        }
        else if (File->Buffer[0] == 0xEF && File->Buffer[1] == 0xBB && File->Buffer[2] == 0xBF) {
            // BOM in UTF-8
            File->Encoding = ENCODING_UTF8;       // translate from UTF-8 to UTF-16
            File->Current8Ptr += 3;
        }
        else if (File->Buffer[1] == 0 && File->Buffer[3] == 0) {
            File->Encoding = ENCODING_UTF16_LE;   // use CHAR16 as is
        }
    }

    return EFI_SUCCESS;
}

//
// Get a single line of text from a file
//

static
CHAR16 * ReadLine (
    REFIT_FILE *File
) {
    CHAR16  *Line;
    CHAR16  *qChar16;
    CHAR16  *pChar16;
    CHAR16  *LineEndChar16;
    CHAR16  *LineStartChar16;

    CHAR8   *pChar08;
    CHAR8   *LineEndChar08;
    CHAR8   *LineStartChar08;
    UINTN    LineLength;


    if (File->Buffer == NULL) {
        // Early Return
        return NULL;
    }

    if (File->Encoding != ENCODING_UTF8      &&
        File->Encoding != ENCODING_UTF16_LE  &&
        File->Encoding != ENCODING_ISO8859_1
    ) {
        // Early Return ... Unsupported encoding
        return NULL;
    }

    if (File->Encoding == ENCODING_UTF8 ||
        File->Encoding == ENCODING_ISO8859_1
    ) {
        pChar08 = File->Current8Ptr;
        if (pChar08 >= File->End8Ptr) {
            // Early Return
            return NULL;
        }

        LineStartChar08 = pChar08;
        for (; pChar08 < File->End8Ptr; pChar08++) {
            if (*pChar08 == 13 || *pChar08 == 10) {
                break;
            }
        }
        LineEndChar08 = pChar08;
        for (; pChar08 < File->End8Ptr; pChar08++) {
            if (*pChar08 != 13 && *pChar08 != 10) {
                break;
            }
        }
        File->Current8Ptr = pChar08;

        LineLength = (UINTN) (LineEndChar08 - LineStartChar08) + 1;
        Line = AllocatePool (sizeof (CHAR16) * LineLength);
        if (Line == NULL) {
            // Early Return
            return NULL;
        }

        qChar16 = Line;
        if (File->Encoding == ENCODING_ISO8859_1) {
            for (pChar08 = LineStartChar08; pChar08 < LineEndChar08; ) {
                *qChar16++ = *pChar08++;
            }
        }
        else if (File->Encoding == ENCODING_UTF8) {
            // DA-TAG: Investigate This
            //         Actually handle UTF-8
            //         Currently just duplicates previous block
            for (pChar08 = LineStartChar08; pChar08 < LineEndChar08; ) {
                *qChar16++ = *pChar08++;
            }
        }
        *qChar16 = 0;

        return Line;
    }

    // Encoding is ENCODING_UTF16_LE
    pChar16 = File->Current16Ptr;
    if (pChar16 >= File->End16Ptr) {
        // Early Return
        return NULL;
    }

    LineStartChar16 = pChar16;
    for (; pChar16 < File->End16Ptr; pChar16++) {
        if (*pChar16 == 13 || *pChar16 == 10) {
            break;
        }
    }
    LineEndChar16 = pChar16;
    for (; pChar16 < File->End16Ptr; pChar16++) {
        if (*pChar16 != 13 && *pChar16 != 10) {
            break;
        }
    }
    File->Current16Ptr = pChar16;

    LineLength = (UINTN) (LineEndChar16 - LineStartChar16) + 1;
    Line = AllocatePool (sizeof (CHAR16) * LineLength);
    if (Line == NULL) {
        // Early Return
        return NULL;
    }

    for (pChar16 = LineStartChar16, qChar16 = Line; pChar16 < LineEndChar16; ) {
        *qChar16++ = *pChar16++;
    }
    *qChar16 = 0;

    return Line;
}

// Returns FALSE if *p points to the end of a token, TRUE otherwise.
// Also modifies *p **IF** the first and second characters are both
// quotes ('"'); it deletes one of them.
static
BOOLEAN KeepReading (
    IN OUT CHAR16  *p,
    IN OUT BOOLEAN *IsQuoted
) {
    CHAR16  *Temp;
    BOOLEAN  MoreToRead;

    if ((p == NULL) || (IsQuoted == NULL)) {
        return FALSE;
    }

    if (*p == L'\0') {
        return FALSE;
    }

    MoreToRead = FALSE;
    if ((
        *p != ' '  &&
        *p != '\t' &&
        *p != '='  &&
        *p != '#'  &&
        *p != ','
    ) || *IsQuoted) {
        MoreToRead = TRUE;
    }

    if (*p == L'"') {
        if (p[1] != L'"') {
            *IsQuoted  = !(*IsQuoted);
            MoreToRead = FALSE;
        }
        else {
            Temp = StrDuplicate (&p[1]);
            if (Temp != NULL) {
                StrCpy (p, Temp);
                MY_FREE_POOL(Temp);
            }
            MoreToRead = TRUE;
        }
    } // if first character is a quote

    return MoreToRead;
} // BOOLEAN KeepReading()

//
// Get a line of tokens from a file
//
UINTN ReadTokenLine (
    IN  REFIT_FILE   *File,
    OUT CHAR16     ***TokenList
) {
    BOOLEAN  LineFinished;
    BOOLEAN  IsQuoted;
    CHAR16  *Line, *Token, *p;
    UINTN    TokenCount;

    *TokenList = NULL;

    IsQuoted = FALSE;
    TokenCount = 0;
    while (TokenCount == 0) {
        Line = ReadLine (File);
        if (Line == NULL) {
            return 0;
        }

        p = Line;
        LineFinished = FALSE;
        while (!LineFinished) {
            // Skip whitespace and find start of token
            while (!IsQuoted &&
                (
                    *p == ' '  ||
                    *p == '\t' ||
                    *p == '='  ||
                    *p == ','
                )
            ) {
                p++;
            } // while

            if (*p == 0 || *p == '#') {
                break;
            }

            if (*p == '"') {
               IsQuoted = !IsQuoted;
               p++;
            }

            Token = p;

            // Find end of token
            while (KeepReading (p, &IsQuoted)) {
               if ((*p == L'/') && !IsQuoted) {
                   // Switch Unix style to DOS style directory separators
                   *p = L'\\';
               }
               p++;
            } // while

            if (*p == L'\0' || *p == L'#') {
                LineFinished = TRUE;
            }
            *p++ = 0;

            AddListElement (
                (VOID ***) TokenList,
                &TokenCount,
                (VOID *) StrDuplicate (Token)
            );
        } // while !LineFinished

        MY_FREE_POOL(Line);
    } // while TokenCount == 0

    return TokenCount;
} // UINTN ReadTokenLine()

VOID FreeTokenLine (
    IN OUT CHAR16 ***TokenList,
    IN OUT UINTN    *TokenCount
) {
    // DA-TAG: Investigate this
    //         Also free the items
    FreeList ((VOID ***) TokenList, TokenCount);
} // VOID FreeTokenLine()

// Handle a parameter with a single integer argument (signed)
static
VOID HandleSignedInt (
    IN  CHAR16 **TokenList,
    IN  UINTN    TokenCount,
    OUT INTN    *Value
) {
    if (TokenCount == 2) {
        *Value = (TokenList[1][0] == '-')
            ? Atoi(TokenList[1]+1) * -1
            : Atoi(TokenList[1]);
    }
} // static VOID HandleSignedInt()

// Handle a parameter with a single integer argument (unsigned)
static
VOID HandleUnsignedInt (
    IN  CHAR16 **TokenList,
    IN  UINTN    TokenCount,
    OUT UINTN   *Value
) {
    if (TokenCount == 2) {
        *Value = Atoi(TokenList[1]);
    }
} // static VOID HandleUnsignedInt()

// Handle a parameter with a single string argument
static
VOID HandleString (
    IN  CHAR16  **TokenList,
    IN  UINTN     TokenCount,
    OUT CHAR16  **Target
) {
    if ((TokenCount == 2) && Target) {
        MY_FREE_POOL(*Target);
        *Target = StrDuplicate (TokenList[1]);
    }
} // static VOID HandleString()

// Handle a parameter with a series of string arguments, to replace or be added to a
// comma-delimited list. Passes each token through the CleanUpPathNameSlashes() function
// to ensure consistency in subsequent comparisons of filenames. If the first
// non-keyword token is "+", the list is added to the existing target string; otherwise,
// the tokens replace the current string.
static
VOID HandleStrings (
    IN  CHAR16 **TokenList,
    IN  UINTN    TokenCount,
    OUT CHAR16 **Target
) {
    UINTN   i;
    BOOLEAN AddMode;

    if (!Target) {
        return;
    }

    AddMode = FALSE;
    if ((TokenCount > 2) && (StrCmp (TokenList[1], L"+") == 0)) {
        AddMode = TRUE;
    }

    if ((*Target != NULL) && !AddMode) {
        MY_FREE_POOL(*Target);
    }

    for (i = 1; i < TokenCount; i++) {
        if ((i != 1) || !AddMode) {
            CleanUpPathNameSlashes (TokenList[i]);
            MergeStrings (Target, TokenList[i], L',');
        }
    }
} // static VOID HandleStrings()

// Handle a parameter with a series of hexadecimal arguments, to replace or be added to a
// linked list of UINT32 values. Any item with a non-hexadecimal value is discarded, as is
// any value that exceeds MaxValue. If the first non-keyword token is "+", the new list is
// added to the existing Target; otherwise, the interpreted tokens replace the current
// Target.
static
VOID HandleHexes (
    IN  CHAR16       **TokenList,
    IN  UINTN          TokenCount,
    IN  UINTN          MaxValue,
    OUT UINT32_LIST  **Target
) {
    UINTN        i;
    UINTN        InputIndex;
    UINT32       Value;
    UINT32_LIST *EndOfList;
    UINT32_LIST *NewEntry;

    EndOfList = NULL;
    if ((TokenCount > 2) && (StrCmp (TokenList[1], L"+") == 0)) {
        InputIndex = 2;
        EndOfList = *Target;
        while (EndOfList && (EndOfList->Next != NULL)) {
            EndOfList = EndOfList->Next;
        }
    }
    else {
        InputIndex = 1;
        EraseUint32List (Target);
    }

    for (i = InputIndex; i < TokenCount; i++) {
        if (!IsValidHex (TokenList[i])) {
            continue;
        }

        Value = (UINT32) StrToHex (TokenList[i], 0, 8);
        if (Value > MaxValue) {
            continue;
        }

        NewEntry = AllocatePool (sizeof (UINT32_LIST));
        if (NewEntry == NULL) {
            return;
        }

        NewEntry->Value = Value;
        NewEntry->Next = NULL;
        if (EndOfList == NULL) {
            EndOfList = NewEntry;
            *Target = NewEntry;
        }
        else {
            EndOfList->Next = NewEntry;
            EndOfList = NewEntry;
        }
    } // for
} // static VOID HandleHexes()

// Convert TimeString (in "HH:MM" format) to a pure-minute format. Values should be
// in the range from 0 (for 00:00, or midnight) to 1439 (for 23:59; aka LAST_MINUTE).
// Any value outside that range denotes an error in the specification. Note that if
// the input is a number that includes no colon, this function will return the original
// number in UINTN form.
static
UINTN HandleTime (
    IN CHAR16 *TimeString
) {
    UINTN i, Hour, Minute, TimeLength;

    TimeLength = StrLen (TimeString);
    i = Hour = Minute = 0;
    while (i < TimeLength) {
        if (TimeString[i] == L':') {
            Hour = Minute;
            Minute = 0;
        }

        if ((TimeString[i] >= L'0') && (TimeString[i] <= '9')) {
            Minute *= 10;
            Minute += (TimeString[i] - L'0');
        }

        i++;
    } // while

    return (Hour * 60 + Minute);
} // BOOLEAN HandleTime()

static
BOOLEAN HandleBoolean (
    IN CHAR16 **TokenList,
    IN UINTN    TokenCount
) {
    BOOLEAN TruthValue;

    TruthValue = TRUE;
    if ((TokenCount >= 2) &&
        (
            StrCmp (TokenList[1], L"0") == 0
            || MyStriCmp (TokenList[1], L"false")
            || MyStriCmp (TokenList[1], L"off")
        )
    ) {
        TruthValue = FALSE;
    }

    return TruthValue;
} // BOOLEAN HandleBoolean()

// Sets the default boot loader IF the current time is within the bounds
// defined by the third and fourth tokens in the TokenList.
static
VOID SetDefaultByTime (
    IN  CHAR16 **TokenList,
    OUT CHAR16 **Default
) {
    EFI_STATUS            Status;
    UINTN                 Now;
    UINTN                 EndTime;
    UINTN                 StartTime;
    CHAR16               *MsgStr;
    EFI_TIME              CurrentTime;
    BOOLEAN               SetIt;

    StartTime = HandleTime (TokenList[2]);
    EndTime   = HandleTime (TokenList[3]);

    if ((StartTime <= LAST_MINUTE) && (EndTime <= LAST_MINUTE)) {
        Status = REFIT_CALL_2_WRAPPER(gRT->GetTime, &CurrentTime, NULL);
        if (EFI_ERROR(Status)) {
            return;
        }

        Now = CurrentTime.Hour * 60 + CurrentTime.Minute;
        if (Now > LAST_MINUTE) {
            // Should not happen ... Just being paranoid
            MsgStr = PoolPrint (
                L"ERROR: Impossible System Time:- %d:%d",
                CurrentTime.Hour, CurrentTime.Minute
            );

            #if REFIT_DEBUG > 0
            LOG_MSG("  - %s", MsgStr);
            LOG_MSG("\n");
            #endif

            Print (L"%s\n", MsgStr);
            MY_FREE_POOL(MsgStr);

            // Early Return
            return;
        }

        SetIt = FALSE;
        if (StartTime < EndTime) {
            // Time range does NOT cross midnight
            if ((Now >= StartTime) && (Now <= EndTime)) {
                SetIt = TRUE;
            }
        }
        else {
            // Time range DOES cross midnight
            if ((Now >= StartTime) || (Now <= EndTime)) {
                SetIt = TRUE;
            }
        }

        if (SetIt) {
            MY_FREE_POOL(*Default);
            *Default = StrDuplicate (TokenList[1]);
        }
    } // if ((StartTime <= LAST_MINUTE) && (EndTime <= LAST_MINUTE))
} // VOID SetDefaultByTime()

static
LOADER_ENTRY * AddPreparedLoaderEntry (
    LOADER_ENTRY *Entry
) {
    AddMenuEntry (MainMenu, (REFIT_MENU_ENTRY *) Entry);

    return Entry;
} // LOADER_ENTRY * AddPreparedLoaderEntry()

// read config file
VOID ReadConfig (
    CHAR16 *FileName
) {
    EFI_STATUS        Status;
    REFIT_FILE        File;
    BOOLEAN           DoneTool;
    BOOLEAN           AllowIncludes;
    BOOLEAN           HiddenTagsFlag;
    BOOLEAN           DeclineSetting;
    CHAR16          **TokenList;
    CHAR16           *Flag;
    CHAR16           *TempStr;
    CHAR16           *MsgStr;
    UINTN             i, j;
    UINTN             TokenCount;
    UINTN             InvalidEntries;
    INTN              MaxLogLevel;

    #if REFIT_DEBUG > 0
    INTN             RealLogLevel;
    INTN             HighLogLevel;
    #endif

    // Control 'Include' Depth
    ReadLoops = ReadLoops + 1;
    if (ReadLoops > 2) {
        return;
    }

    AllowIncludes = OuterLoop;

    #if REFIT_DEBUG > 0
    if (AllowIncludes) LOG_MSG("R E A D   C O N F I G U R A T I O N   T O K E N S");
    MuteLogger = TRUE;
    #endif

    // Set a few defaults only if we are loading the default file.
    if (MyStriCmp (FileName, GlobalConfig.ConfigFilename)) {
        MY_FREE_POOL(GlobalConfig.DontScanTools);

        MY_FREE_POOL(GlobalConfig.AlsoScan);
        GlobalConfig.AlsoScan = StrDuplicate (ALSO_SCAN_DIRS);

        MY_FREE_POOL(GlobalConfig.DontScanDirs);
        if (SelfVolume) {
            TempStr = GuidAsString (&(SelfVolume->PartGuid));
        }
        MergeStrings (&TempStr, SelfDirPath, L':');
        MergeStrings (&TempStr, MEMTEST_LOCATIONS, L',');
        GlobalConfig.DontScanDirs = TempStr;

        MY_FREE_POOL(GlobalConfig.DontScanFiles);
        GlobalConfig.DontScanFiles = StrDuplicate (DONT_SCAN_FILES);

        MY_FREE_POOL(GlobalConfig.DontScanFirmware);
        MergeStrings (&(GlobalConfig.DontScanFiles), MOK_NAMES, L',');
        MergeStrings (&(GlobalConfig.DontScanFiles), FWUPDATE_NAMES, L',');

        MY_FREE_POOL(GlobalConfig.DontScanVolumes);
        GlobalConfig.DontScanVolumes = StrDuplicate (DONT_SCAN_VOLUMES);

        MY_FREE_POOL(GlobalConfig.WindowsRecoveryFiles);
        GlobalConfig.WindowsRecoveryFiles = StrDuplicate (WINDOWS_RECOVERY_FILES);

        MY_FREE_POOL(GlobalConfig.MacOSRecoveryFiles);
        GlobalConfig.MacOSRecoveryFiles = StrDuplicate (MACOS_RECOVERY_FILES);

        MY_FREE_POOL(GlobalConfig.DefaultSelection);
        GlobalConfig.DefaultSelection = StrDuplicate (L"+");

        MY_FREE_POOL(GlobalConfig.LinuxPrefixes);
        GlobalConfig.LinuxPrefixes = StrDuplicate (L"+");
    } // if

    if (!FileExists (SelfDir, FileName)) {
        SwitchToText (FALSE);

        MsgStr = StrDuplicate (
            L"  - WARN: Cannot Find Configuration File ... Loading Defaults!!"
        );
        #if REFIT_DEBUG > 0
        MuteLogger = FALSE;
        LOG_MSG("%s%s", OffsetNext, MsgStr);
        MuteLogger = TRUE;
        #endif
        PrintUglyText (MsgStr, NEXTLINE);
        MY_FREE_POOL(MsgStr);

        if (!FileExists (SelfDir, L"icons")) {
            MsgStr = StrDuplicate (
                L"  - WARN: Cannot Find Icons Directory ... Switching to Text Mode!!"
            );
            #if REFIT_DEBUG > 0
            MuteLogger = FALSE;
            LOG_MSG("%s%s", OffsetNext, MsgStr);
            MuteLogger = TRUE;
            #endif
            PrintUglyText (MsgStr, NEXTLINE);
            MY_FREE_POOL(MsgStr);

            GlobalConfig.TextOnly = TRUE;
        }

        #if REFIT_DEBUG > 0
        MuteLogger = FALSE;
        LOG_MSG("\n");
        MuteLogger = TRUE; /* Explicit For FB Infer */
        #endif

        PauseForKey();
        SwitchToGraphics();

        return;
    }

    Status = RefitReadFile (SelfDir, FileName, &File, &i);
    if (EFI_ERROR(Status)) {
        #if REFIT_DEBUG > 0
        MuteLogger = FALSE;
        LOG_MSG("\n");
        MuteLogger = TRUE; /* Explicit For FB Infer */
        #endif

        return;
    }

    MaxLogLevel = (ForensicLogging) ? MAXLOGLEVEL + 1 : MAXLOGLEVEL;
    for (;;) {
        TokenCount = ReadTokenLine (&File, &TokenList);
        if (TokenCount == 0) {
            break;
        }

        if (MyStriCmp (TokenList[0], L"timeout")) {
            // DA-TAG: Signed integer as can have negative value
            HandleSignedInt (TokenList, TokenCount, &(GlobalConfig.Timeout));
            GlobalConfig.DirectBoot = (GlobalConfig.Timeout < 0) ? TRUE : FALSE;

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'timeout'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (MyStriCmp (TokenList[0], L"shutdown_after_timeout")) {
            GlobalConfig.ShutdownAfterTimeout = HandleBoolean (TokenList, TokenCount);

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'shutdown_after_timeout'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (MyStriCmp (TokenList[0], L"hideui")) {
            for (i = 1; i < TokenCount; i++) {
                Flag = TokenList[i];
                if (0);
                else if (MyStriCmp (Flag, L"all")       ) GlobalConfig.HideUIFlags  = HIDEUI_FLAG_ALL;
                else if (MyStriCmp (Flag, L"label")     ) GlobalConfig.HideUIFlags |= HIDEUI_FLAG_LABEL;
                else if (MyStriCmp (Flag, L"hints")     ) GlobalConfig.HideUIFlags |= HIDEUI_FLAG_HINTS;
                else if (MyStriCmp (Flag, L"banner")    ) GlobalConfig.HideUIFlags |= HIDEUI_FLAG_BANNER;
                else if (MyStriCmp (Flag, L"hwtest")    ) GlobalConfig.HideUIFlags |= HIDEUI_FLAG_HWTEST;
                else if (MyStriCmp (Flag, L"arrows")    ) GlobalConfig.HideUIFlags |= HIDEUI_FLAG_ARROWS;
                else if (MyStriCmp (Flag, L"editor")    ) GlobalConfig.HideUIFlags |= HIDEUI_FLAG_EDITOR;
                else if (MyStriCmp (Flag, L"badges")    ) GlobalConfig.HideUIFlags |= HIDEUI_FLAG_BADGES;
                else if (MyStriCmp (Flag, L"safemode")  ) GlobalConfig.HideUIFlags |= HIDEUI_FLAG_SAFEMODE;
                else if (MyStriCmp (Flag, L"singleuser")) GlobalConfig.HideUIFlags |= HIDEUI_FLAG_SINGLEUSER;
                else {
                    SwitchToText (FALSE);

                    MsgStr = PoolPrint (
                        L"  - WARN: Invalid 'hideui' Flag:- '%s'",
                        Flag
                    );
                    PrintUglyText (MsgStr, NEXTLINE);

                    #if REFIT_DEBUG > 0
                    MuteLogger = FALSE;
                    LOG_MSG("%s%s", OffsetNext, MsgStr);
                    MuteLogger = TRUE;
                    #endif

                    PauseForKey();
                    MY_FREE_POOL(MsgStr);
                }
            } // for

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'hideui'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (MyStriCmp (TokenList[0], L"icons_dir")) {
            HandleString (TokenList, TokenCount, &(GlobalConfig.IconsDir));

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'icons_dir'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (MyStriCmp (TokenList[0], L"set_boot_args")) {
            HandleString (TokenList, TokenCount, &(GlobalConfig.SetBootArgs));

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'set_boot_args'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (MyStriCmp (TokenList[0], L"scanfor")) {
            for (i = 0; i < NUM_SCAN_OPTIONS; i++) {
                GlobalConfig.ScanFor[i] = (i < TokenCount) ? TokenList[i][0] : ' ';
            } // for

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'scanfor'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (MyStriCmp (TokenList[0], L"use_nvram")) {
            GlobalConfig.UseNvram = HandleBoolean (TokenList, TokenCount);

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'use_nvram'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (MyStriCmp (TokenList[0], L"uefi_deep_legacy_scan")) {
            GlobalConfig.DeepLegacyScan = HandleBoolean (TokenList, TokenCount);

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'uefi_deep_legacy_scan'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (MyStriCmp (TokenList[0], L"disable_rescan_dxe")) {
            DeclineSetting = HandleBoolean (TokenList, TokenCount);
            GlobalConfig.RescanDXE = (DeclineSetting) ? FALSE : TRUE;

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'disable_rescan_dxe'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (MyStriCmp (TokenList[0], L"ransom_drives")) {
            GlobalConfig.RansomDrives = (AppleFirmware) ? FALSE : HandleBoolean (TokenList, TokenCount);

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'ransom_drives'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (MyStriCmp (TokenList[0], L"scan_delay") && (TokenCount == 2)) {
            HandleUnsignedInt (TokenList, TokenCount, &(GlobalConfig.ScanDelay));

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'scan_delay'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (MyStriCmp (TokenList[0], L"log_level") && (TokenCount == 2)) {
            // DA-TAG: Signed integer as *MAY* have negative value input
            HandleSignedInt (TokenList, TokenCount, &(GlobalConfig.LogLevel));
            // Sanitise levels
            if (0);
            else if (GlobalConfig.LogLevel < LOGLEVELOFF) GlobalConfig.LogLevel = LOGLEVELOFF;
            else if (GlobalConfig.LogLevel > MaxLogLevel) GlobalConfig.LogLevel = MaxLogLevel;

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'log_level'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (MyStriCmp (TokenList[0], L"icon_row_move") && (TokenCount == 2)) {
            // DA-TAG: Signed integer as *MAY* have negative value input
            HandleSignedInt (TokenList, TokenCount, &(GlobalConfig.IconRowMove));

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'icon_row_move'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (MyStriCmp (TokenList[0], L"icon_row_tune") && (TokenCount == 2)) {
            // DA-TAG: Signed integer as *MAY* have negative value input
            HandleSignedInt (TokenList, TokenCount, &(GlobalConfig.IconRowTune));
            // Store as opposite number
            GlobalConfig.IconRowTune *= -1;

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'icon_row_tune'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (MyStriCmp (TokenList[0], L"also_scan_dirs")) {
            HandleStrings (TokenList, TokenCount, &(GlobalConfig.AlsoScan));

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'also_scan_dirs'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (
            MyStriCmp (TokenList[0], L"dont_scan_dirs") ||
            MyStriCmp (TokenList[0], L"don't_scan_dirs")
        ) {
            HandleStrings (TokenList, TokenCount, &(GlobalConfig.DontScanDirs));

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'dont_scan_dirs'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (
            MyStriCmp (TokenList[0], L"dont_scan_files") ||
            MyStriCmp (TokenList[0], L"don't_scan_files")
        ) {
            HandleStrings (TokenList, TokenCount, &(GlobalConfig.DontScanFiles));

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'dont_scan_files'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (
            MyStriCmp (TokenList[0], L"dont_scan_tools") ||
            MyStriCmp (TokenList[0], L"don't_scan_tools")
        ) {
            HandleStrings (TokenList, TokenCount, &(GlobalConfig.DontScanTools));

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'dont_scan_tools'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (
            MyStriCmp (TokenList[0], L"dont_scan_firmware") ||
            MyStriCmp (TokenList[0], L"don't_scan_firmware")
        ) {
            HandleStrings (TokenList, TokenCount, &(GlobalConfig.DontScanFirmware));

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'dont_scan_firmware'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (
            MyStriCmp (TokenList[0], L"dont_scan_volumes") ||
            MyStriCmp (TokenList[0], L"don't_scan_volumes")
        ) {
            // Note: Do not use HandleStrings() because it modifies slashes.
            //       However, This might be present in the volume name.
            MY_FREE_POOL(GlobalConfig.DontScanVolumes);
            for (i = 1; i < TokenCount; i++) {
                MergeStrings (&GlobalConfig.DontScanVolumes, TokenList[i], L',');
            }

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'dont_scan_volumes'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (MyStriCmp (TokenList[0], L"windows_recovery_files")) {
            HandleStrings (TokenList, TokenCount, &(GlobalConfig.WindowsRecoveryFiles));

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'windows_recovery_files'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (MyStriCmp (TokenList[0], L"scan_driver_dirs")) {
            HandleStrings (TokenList, TokenCount, &(GlobalConfig.DriverDirs));

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'scan_driver_dirs'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (MyStriCmp (TokenList[0], L"showtools")) {
            // DA-TAG: HiddenTags reset looks strange but is actually valid
            //         Artificial default of 'TRUE' needed as misconfig exit option
            //         This sets real default of 'FALSE' when 'showtools' is present
            GlobalConfig.HiddenTags = FALSE;

            SetMem (GlobalConfig.ShowTools, NUM_TOOLS * sizeof (UINTN), 0);

            DoneTool = FALSE;
            InvalidEntries = 0;
            i = 0;
            for (;;) {
                // DA-TAG: Start Index is 1 Here ('i' for NUM_TOOLS/TokenList)
                i = i + 1;
                if (i >= TokenCount ||
                    i >= (NUM_TOOLS + InvalidEntries)
                ) {
                    // Break Loop
                    break;
                }

                // Set Showtools Index
                j = (DoneTool) ? j + 1 : 0;

                Flag = TokenList[i];
                if (0);
                else if (MyStriCmp (Flag, L"exit")            ) GlobalConfig.ShowTools[j] = TAG_EXIT;
                else if (MyStriCmp (Flag, L"shell")           ) GlobalConfig.ShowTools[j] = TAG_SHELL;
                else if (MyStriCmp (Flag, L"gdisk")           ) GlobalConfig.ShowTools[j] = TAG_GDISK;
                else if (MyStriCmp (Flag, L"about")           ) GlobalConfig.ShowTools[j] = TAG_ABOUT;
                else if (MyStriCmp (Flag, L"reboot")          ) GlobalConfig.ShowTools[j] = TAG_REBOOT;
                else if (MyStriCmp (Flag, L"gptsync")         ) GlobalConfig.ShowTools[j] = TAG_GPTSYNC;
                else if (MyStriCmp (Flag, L"install")         ) GlobalConfig.ShowTools[j] = TAG_INSTALL;
                else if (MyStriCmp (Flag, L"netboot")         ) GlobalConfig.ShowTools[j] = TAG_NETBOOT;
                else if (MyStriCmp (Flag, L"memtest")         ) GlobalConfig.ShowTools[j] = TAG_MEMTEST;
                else if (MyStriCmp (Flag, L"memtest86")       ) GlobalConfig.ShowTools[j] = TAG_MEMTEST;
                else if (MyStriCmp (Flag, L"shutdown")        ) GlobalConfig.ShowTools[j] = TAG_SHUTDOWN;
                else if (MyStriCmp (Flag, L"mok_tool")        ) GlobalConfig.ShowTools[j] = TAG_MOK_TOOL;
                else if (MyStriCmp (Flag, L"firmware")        ) GlobalConfig.ShowTools[j] = TAG_FIRMWARE;
                else if (MyStriCmp (Flag, L"bootorder")       ) GlobalConfig.ShowTools[j] = TAG_BOOTORDER;
                else if (MyStriCmp (Flag, L"csr_rotate")      ) GlobalConfig.ShowTools[j] = TAG_CSR_ROTATE;
                else if (MyStriCmp (Flag, L"fwupdate")        ) GlobalConfig.ShowTools[j] = TAG_FWUPDATE_TOOL;
                else if (MyStriCmp (Flag, L"clean_nvram")     ) GlobalConfig.ShowTools[j] = TAG_INFO_NVRAMCLEAN;
                else if (MyStriCmp (Flag, L"windows_recovery")) GlobalConfig.ShowTools[j] = TAG_RECOVERY_WINDOWS;
                else if (MyStriCmp (Flag, L"apple_recovery")  ) GlobalConfig.ShowTools[j] = TAG_RECOVERY_APPLE;
                else if (MyStriCmp (Flag, L"hidden_tags")) {
                    GlobalConfig.ShowTools[j] = TAG_HIDDEN;
                    GlobalConfig.HiddenTags = TRUE;
                }
                else {
                    #if REFIT_DEBUG > 0
                    MuteLogger = FALSE;
                    ALT_LOG(1, LOG_THREE_STAR_MID, L"Invalid Config Entry in 'showtools' List:- '%s'!!", Flag);
                    MuteLogger = TRUE;
                    #endif

                    // Handle Showtools Index
                    j = (DoneTool) ? j - 1 : 0;

                    // Increment Invalid Entry Count
                    InvalidEntries = InvalidEntries + 1;

                    // Skip 'DoneTool' Reset
                    continue;
                }
                DoneTool = TRUE;
            } // for ;;

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'showtools'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (MyStriCmp (TokenList[0], L"support_gzipped_loaders")) {
            GlobalConfig.GzippedLoaders = HandleBoolean (TokenList, TokenCount);


            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'support_gzipped_loaders'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (MyStriCmp (TokenList[0], L"banner")) {
            HandleString (TokenList, TokenCount, &(GlobalConfig.BannerFileName));

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'banner'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (MyStriCmp (TokenList[0], L"banner_scale") && (TokenCount == 2)) {
            if (MyStriCmp (TokenList[1], L"noscale")) {
                GlobalConfig.BannerScale = BANNER_NOSCALE;
            }
            else if (
                MyStriCmp (TokenList[1], L"fillscreen") ||
                MyStriCmp (TokenList[1], L"fullscreen")
            ) {
                GlobalConfig.BannerScale = BANNER_FILLSCREEN;
            }
            else {
                MsgStr = PoolPrint (
                    L"  - WARN: Invalid 'banner_type' Flag:- '%s'",
                    TokenList[1]
                );
                PrintUglyText (MsgStr, NEXTLINE);

                #if REFIT_DEBUG > 0
                MuteLogger = FALSE;
                LOG_MSG("%s%s", OffsetNext, MsgStr);
                MuteLogger = TRUE;
                #endif

                PauseForKey();
                MY_FREE_POOL(MsgStr);
            } // if/else MyStriCmp TokenList[0]

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'banner_scale'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (MyStriCmp (TokenList[0], L"nvram_variable_limit") && (TokenCount == 2)) {
            HandleUnsignedInt (TokenList, TokenCount, &(GlobalConfig.NvramVariableLimit));

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'nvram_variable_limit'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (MyStriCmp (TokenList[0], L"small_icon_size") && (TokenCount == 2)) {
            HandleUnsignedInt (TokenList, TokenCount, &i);
            if (i >= 32) {
                GlobalConfig.IconSizes[ICON_SIZE_SMALL] = i;
            }

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'small_icon_size'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (MyStriCmp (TokenList[0], L"big_icon_size") && (TokenCount == 2)) {
            HandleUnsignedInt (TokenList, TokenCount, &i);
            if (i >= 32) {
                GlobalConfig.IconSizes[ICON_SIZE_BIG] = i;
                GlobalConfig.IconSizes[ICON_SIZE_BADGE] = i / 4;
            }

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'big_icon_size'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (MyStriCmp (TokenList[0], L"mouse_size") && (TokenCount == 2)) {
            HandleUnsignedInt (TokenList, TokenCount, &i);
            if (i >= DEFAULT_MOUSE_SIZE) {
                GlobalConfig.IconSizes[ICON_SIZE_MOUSE] = i;
            }

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'mouse_size'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (MyStriCmp (TokenList[0], L"selection_small")) {
            HandleString (TokenList, TokenCount, &(GlobalConfig.SelectionSmallFileName));

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'selection_small'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (MyStriCmp (TokenList[0], L"selection_big")) {
            HandleString (TokenList, TokenCount, &(GlobalConfig.SelectionBigFileName));

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'selection_big'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (MyStriCmp (TokenList[0], L"default_selection")) {
            if (TokenCount == 4) {
                SetDefaultByTime (TokenList, &(GlobalConfig.DefaultSelection));
            }
            else {
                HandleString (TokenList, TokenCount, &(GlobalConfig.DefaultSelection));
            }

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'default_selection'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (MyStriCmp (TokenList[0], L"textonly")) {
            GlobalConfig.TextOnly = HandleBoolean (TokenList, TokenCount);

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'textonly'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (MyStriCmp (TokenList[0], L"textmode")) {
            HandleUnsignedInt (TokenList, TokenCount, &(GlobalConfig.RequestedTextMode));

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'textmode'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (MyStriCmp (TokenList[0], L"resolution") && ((TokenCount == 2) || (TokenCount == 3))) {
            if (MyStriCmp(TokenList[1], L"max")) {
                // DA-TAG: Has been set to 0 so as to ignore the 'max' setting
                //GlobalConfig.RequestedScreenWidth  = MAX_RES_CODE;
                //GlobalConfig.RequestedScreenHeight = MAX_RES_CODE;
                GlobalConfig.RequestedScreenWidth  = 0;
                GlobalConfig.RequestedScreenHeight = 0;
            }
            else {
                GlobalConfig.RequestedScreenWidth = Atoi(TokenList[1]);
                if (TokenCount == 3) {
                    GlobalConfig.RequestedScreenHeight = Atoi(TokenList[2]);
                }
                else {
                    GlobalConfig.RequestedScreenHeight = 0;
                }
            }

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'resolution'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (MyStriCmp (TokenList[0], L"screensaver")) {
            // DA-TAG: Signed integer as can have negative value
            HandleSignedInt (TokenList, TokenCount, &(GlobalConfig.ScreensaverTime));

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'screensaver'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (MyStriCmp (TokenList[0], L"use_graphics_for")) {
            if ((TokenCount == 2) || ((TokenCount > 2) && (!MyStriCmp (TokenList[1], L"+")))) {
                GlobalConfig.GraphicsFor = 0;
            }

            for (i = 1; i < TokenCount; i++) {
                if (0);
                else if (MyStriCmp (TokenList[i], L"osx")     ) GlobalConfig.GraphicsFor |= GRAPHICS_FOR_OSX;
                else if (MyStriCmp (TokenList[i], L"grub")    ) GlobalConfig.GraphicsFor |= GRAPHICS_FOR_GRUB;
                else if (MyStriCmp (TokenList[i], L"linux")   ) GlobalConfig.GraphicsFor |= GRAPHICS_FOR_LINUX;
                else if (MyStriCmp (TokenList[i], L"elilo")   ) GlobalConfig.GraphicsFor |= GRAPHICS_FOR_ELILO;
                else if (MyStriCmp (TokenList[i], L"clover")  ) GlobalConfig.GraphicsFor |= GRAPHICS_FOR_CLOVER;
                else if (MyStriCmp (TokenList[i], L"windows") ) GlobalConfig.GraphicsFor |= GRAPHICS_FOR_WINDOWS;
                else if (MyStriCmp (TokenList[i], L"opencore")) GlobalConfig.GraphicsFor |= GRAPHICS_FOR_OPENCORE;
            } // for

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'use_graphics_for'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (MyStriCmp (TokenList[0], L"font") && (TokenCount == 2)) {
            egLoadFont (TokenList[1]);

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'font'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (MyStriCmp (TokenList[0], L"scan_all_linux_kernels")) {
            GlobalConfig.ScanAllLinux = HandleBoolean (TokenList, TokenCount);

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'scan_all_linux_kernels'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (MyStriCmp (TokenList[0], L"fold_linux_kernels")) {
            GlobalConfig.FoldLinuxKernels = HandleBoolean (TokenList, TokenCount);

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'fold_linux_kernels'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (MyStriCmp (TokenList[0], L"linux_prefixes")) {
            HandleStrings (TokenList, TokenCount, &(GlobalConfig.LinuxPrefixes));

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'linux_prefixes'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (MyStriCmp (TokenList[0], L"extra_kernel_version_strings")) {
            HandleStrings (TokenList, TokenCount, &(GlobalConfig.ExtraKernelVersionStrings));

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'extra_kernel_version_strings'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (MyStriCmp (TokenList[0], L"max_tags")) {
            HandleUnsignedInt (TokenList, TokenCount, &(GlobalConfig.MaxTags));

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'max_tags'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (MyStriCmp (TokenList[0], L"enable_and_lock_vmx")) {
            GlobalConfig.EnableAndLockVMX = HandleBoolean (TokenList, TokenCount);

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'enable_and_lock_vmx'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (MyStriCmp (TokenList[0], L"spoof_osx_version")) {
            HandleString (TokenList, TokenCount, &(GlobalConfig.SpoofOSXVersion));

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'spoof_osx_version'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (MyStriCmp (TokenList[0], L"csr_values")) {
            HandleHexes (TokenList, TokenCount, CSR_MAX_LEGAL_VALUE, &(GlobalConfig.CsrValues));

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'csr_values'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (MyStriCmp (TokenList[0], L"screen_rgb") && TokenCount == 4) {
            // DA-TAG: Consider handling hex input?
            //         KISS ... Stick with integers
            GlobalConfig.ScreenR = Atoi(TokenList[1]);
            GlobalConfig.ScreenG = Atoi(TokenList[2]);
            GlobalConfig.ScreenB = Atoi(TokenList[3]);

            // Record whether a valid custom screen BG is specified
            GlobalConfig.CustomScreenBG = (
                GlobalConfig.ScreenR >= 0 && GlobalConfig.ScreenR <= 255 &&
                GlobalConfig.ScreenG >= 0 && GlobalConfig.ScreenG <= 255 &&
                GlobalConfig.ScreenB >= 0 && GlobalConfig.ScreenB <= 255
            );

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'screen_rgb'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (AllowIncludes
            && (TokenCount == 2)
            && MyStriCmp (TokenList[0], L"include")
            && MyStriCmp (FileName, GlobalConfig.ConfigFilename)
        ) {
            if (!MyStriCmp (TokenList[1], FileName)) {
                #if REFIT_DEBUG > 0
                // DA-TAG: Always log this in case LogLevel is overriden
                RealLogLevel = 0;
                HighLogLevel = MaxLogLevel * 10;
                if (GlobalConfig.LogLevel < MINLOGLEVEL) {
                    RealLogLevel = GlobalConfig.LogLevel;
                    GlobalConfig.LogLevel = HighLogLevel;
                }

                MuteLogger = FALSE;
                if (FirstInclude) {
                    LOG_MSG("\n");
                    LOG_MSG("Detected Overrides File - L O A D   S E T T I N G   O V E R R I D E S");
                    FirstInclude = FALSE;
                }
                LOG_MSG("%s* Supplementary Configuration ... %s", OffsetNext, TokenList[1]);
                MuteLogger = TRUE; /* Explicit For FB Infer */
                #endif

                // Set 'AllowIncludes' to 'false' to break any 'include' chains
                OuterLoop = FALSE;
                ReadConfig (TokenList[1]);
                OuterLoop = TRUE;
                // Reset 'AllowIncludes' to accomodate multiple instances in main file

                #if REFIT_DEBUG > 0
                // DA-TAG: Restore the RealLogLevel
                if (GlobalConfig.LogLevel == HighLogLevel) {
                    GlobalConfig.LogLevel = RealLogLevel;
                }

                // Failsafe
                MuteLogger = TRUE; /* Explicit For FB Infer */
                #endif
            }
        }
        else if (MyStriCmp (TokenList[0], L"write_systemd_vars")) {
            GlobalConfig.WriteSystemdVars = HandleBoolean (TokenList, TokenCount);

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'write_systemd_vars'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (MyStriCmp (TokenList[0], L"unicode_collation")) {
            GlobalConfig.UnicodeCollation = HandleBoolean (TokenList, TokenCount);

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'unicode_collation'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (MyStriCmp (TokenList[0], L"enable_mouse")) {
            GlobalConfig.EnableMouse = HandleBoolean (TokenList, TokenCount);

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'enable_mouse'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif

            // DA-TAG: Force 'RescanDXE'
            //         Update other instances if changing
            if (GlobalConfig.EnableMouse) {
                GlobalConfig.RescanDXE = TRUE;
            }
        }
        else if (MyStriCmp (TokenList[0], L"enable_touch")) {
            GlobalConfig.EnableTouch = HandleBoolean (TokenList, TokenCount);

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'enable_touch'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif

            // DA-TAG: Force 'RescanDXE'
            //         Update other instances if changing
            if (GlobalConfig.EnableTouch) {
                GlobalConfig.RescanDXE = TRUE;
            }
        }
        else if (MyStriCmp (TokenList[0], L"provide_console_gop")) {
            GlobalConfig.ProvideConsoleGOP = HandleBoolean (TokenList, TokenCount);

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'provide_console_gop'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (
            MyStriCmp (TokenList[0], L"transient_boot") ||
            MyStriCmp (TokenList[0], L"ignore_previous_boot")
        ) {
            // DA_TAG: Accomodate Deprecation
            GlobalConfig.TransientBoot = HandleBoolean (TokenList, TokenCount);

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'transient_boot'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (
            MyStriCmp (TokenList[0], L"hidden_icons_ignore") ||
            MyStriCmp (TokenList[0], L"ignore_hidden_icons")
        ) {
            // DA_TAG: Accomodate Deprecation
            GlobalConfig.HiddenIconsIgnore = HandleBoolean (TokenList, TokenCount);

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'hidden_icons_ignore'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (
            MyStriCmp (TokenList[0], L"hidden_icons_external") ||
            MyStriCmp (TokenList[0], L"external_hidden_icons")
        ) {
            // DA_TAG: Accomodate Deprecation
            GlobalConfig.HiddenIconsExternal = HandleBoolean (TokenList, TokenCount);

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'hidden_icons_external'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (
            MyStriCmp (TokenList[0], L"hidden_icons_prefer") ||
            MyStriCmp (TokenList[0], L"prefer_hidden_icons")
        ) {
            // DA_TAG: Accomodate Deprecation
            GlobalConfig.HiddenIconsPrefer = HandleBoolean (TokenList, TokenCount);

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'hidden_icons_prefer'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (
            MyStriCmp (TokenList[0], L"renderer_text") ||
            MyStriCmp (TokenList[0], L"text_renderer")
        ) {
            // DA_TAG: Accomodate Deprecation
            GlobalConfig.UseTextRenderer = HandleBoolean (TokenList, TokenCount);

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'renderer_text'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (
            MyStriCmp (TokenList[0], L"pass_uga_through") ||
            MyStriCmp (TokenList[0], L"uga_pass_through")
        ) {
            // DA_TAG: Accomodate Deprecation
            GlobalConfig.PassUgaThrough = HandleBoolean (TokenList, TokenCount);

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'pass_uga_through'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (
            MyStriCmp (TokenList[0], L"renderer_direct_gop") ||
            MyStriCmp (TokenList[0], L"direct_gop_renderer")
        ) {
            // DA_TAG: Accomodate Deprecation
            GlobalConfig.UseDirectGop = HandleBoolean (TokenList, TokenCount);

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'renderer_direct_gop'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (
            MyStriCmp (TokenList[0], L"force_trim") ||
            MyStriCmp (TokenList[0], L"trim_force")
        ) {
            // DA_TAG: Accomodate Deprecation
            GlobalConfig.ForceTRIM = HandleBoolean (TokenList, TokenCount);

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'force_trim'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (
            MyStriCmp (TokenList[0], L"decline_reload_gop") ||
            MyStriCmp (TokenList[0], L"decline_reloadgop")
        ) {
            // DA_TAG: Accomodate Deprecation
            DeclineSetting = HandleBoolean (TokenList, TokenCount);
            GlobalConfig.ReloadGOP = (DeclineSetting) ? FALSE : TRUE;

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'decline_reload_gop'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (
            MyStriCmp (TokenList[0], L"decline_apfs_load") ||
            MyStriCmp (TokenList[0], L"decline_apfsload")
        ) {
            // DA_TAG: Accomodate Deprecation
            DeclineSetting = HandleBoolean (TokenList, TokenCount);
            GlobalConfig.SupplyAPFS = (DeclineSetting) ? FALSE : TRUE;

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'decline_apfs_load'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (
            MyStriCmp (TokenList[0], L"decline_apfs_mute") ||
            MyStriCmp (TokenList[0], L"decline_apfsmute")
        ) {
            // DA_TAG: Accomodate Deprecation
            DeclineSetting = HandleBoolean (TokenList, TokenCount);
            GlobalConfig.SilenceAPFS = (DeclineSetting) ? FALSE : TRUE;

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'decline_apfs_mute'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (
            MyStriCmp (TokenList[0], L"decline_apfs_sync") ||
            MyStriCmp (TokenList[0], L"decline_apfssync")
        ) {
            // DA_TAG: Accomodate Deprecation
            DeclineSetting = HandleBoolean (TokenList, TokenCount);
            GlobalConfig.SyncAPFS = (DeclineSetting) ? FALSE : TRUE;

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'decline_apfs_sync'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (
            MyStriCmp (TokenList[0], L"decline_apple_fb") ||
            MyStriCmp (TokenList[0], L"decline_applefb")
        ) {
            // DA_TAG: Accomodate Deprecation
            DeclineSetting = HandleBoolean (TokenList, TokenCount);
            GlobalConfig.SupplyAppleFB = (!AppleFirmware)
                ? FALSE
                : (DeclineSetting) ? FALSE : TRUE;

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'decline_apple_fb'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (
            MyStriCmp (TokenList[0], L"decline_nvram_protect") ||
            MyStriCmp (TokenList[0], L"decline_nvramprotect")
        ) {
            // DA_TAG: Accomodate Deprecation
            DeclineSetting = HandleBoolean (TokenList, TokenCount);
            GlobalConfig.NvramProtect = (!AppleFirmware)
                ? FALSE
                : (DeclineSetting) ? FALSE : TRUE;

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'decline_nvram_protect'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (MyStriCmp (TokenList[0], L"decline_help_icon")) {
            DeclineSetting = HandleBoolean (TokenList, TokenCount);
            GlobalConfig.HelpIcon = (DeclineSetting) ? FALSE : TRUE;

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'decline_help_icon'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (
            MyStriCmp (TokenList[0], L"decline_help_tags")  ||
            MyStriCmp (TokenList[0], L"decline_tags_help") ||
            MyStriCmp (TokenList[0], L"decline_tagshelp")
        ) {
            // DA_TAG: Accomodate Deprecation
            DeclineSetting = HandleBoolean (TokenList, TokenCount);
            GlobalConfig.HelpTags = (DeclineSetting) ? FALSE : TRUE;

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'decline_help_tags'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (
            MyStriCmp (TokenList[0], L"decline_help_text")  ||
            MyStriCmp (TokenList[0], L"decline_text_help") ||
            MyStriCmp (TokenList[0], L"decline_texthelp")
        ) {
            // DA_TAG: Accomodate Deprecation
            DeclineSetting = HandleBoolean (TokenList, TokenCount);
            GlobalConfig.HelpText = (DeclineSetting) ? FALSE : TRUE;

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'decline_help_text'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (
            MyStriCmp (TokenList[0], L"csr_normalise") ||
            MyStriCmp (TokenList[0], L"normalise_csr")
        ) {
            // DA_TAG: Accomodate Deprecation
            GlobalConfig.NormaliseCSR = HandleBoolean (TokenList, TokenCount);

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'csr_normalise'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (
            MyStriCmp (TokenList[0], L"csr_dynamic") ||
            MyStriCmp (TokenList[0], L"active_csr")
        ) {
            // DA_TAG: Accomodate Deprecation
            // DA-TAG: Signed integer as can have negative value
            HandleSignedInt (TokenList, TokenCount, &(GlobalConfig.DynamicCSR));

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'csr_dynamic'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (MyStriCmp (TokenList[0], L"mouse_speed") && (TokenCount == 2)) {
            HandleUnsignedInt (TokenList, TokenCount, &i);
            if (i < 1)  i = 1;
            if (i > 32) i = 32;
            GlobalConfig.MouseSpeed = i;

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'mouse_speed'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (MyStriCmp (TokenList[0], L"continue_on_warning")) {
            GlobalConfig.ContinueOnWarning = HandleBoolean (TokenList, TokenCount);

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'continue_on_warning'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (MyStriCmp (TokenList[0], L"decouple_key_f10")) {
            GlobalConfig.DecoupleKeyF10 = HandleBoolean (TokenList, TokenCount);

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'decouple_key_f10'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (MyStriCmp (TokenList[0], L"disable_nvram_paniclog")) {
            GlobalConfig.DisableNvramPanicLog = HandleBoolean (TokenList, TokenCount);

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'disable_nvram_paniclog'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (MyStriCmp (TokenList[0], L"disable_compat_check")) {
            GlobalConfig.DisableCompatCheck = HandleBoolean (TokenList, TokenCount);

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'disable_compat_check'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (MyStriCmp (TokenList[0], L"disable_amfi")) {
            GlobalConfig.DisableAMFI = HandleBoolean (TokenList, TokenCount);

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'disable_amfi'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (MyStriCmp (TokenList[0], L"follow_symlinks")) {
            GlobalConfig.FollowSymlinks = HandleBoolean (TokenList, TokenCount);

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'follow_symlinks'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (MyStriCmp (TokenList[0], L"prefer_uga")) {
            GlobalConfig.PreferUGA = HandleBoolean (TokenList, TokenCount);

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'prefer_uga'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (MyStriCmp (TokenList[0], L"supply_nvme")) {
            GlobalConfig.SupplyNVME = HandleBoolean (TokenList, TokenCount);

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'supply_nvme'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (MyStriCmp (TokenList[0], L"supply_uefi")) {
            GlobalConfig.SupplyUEFI = HandleBoolean (TokenList, TokenCount);

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'supply_uefi'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (MyStriCmp (TokenList[0], L"nvram_protect_ex")) {
            GlobalConfig.NvramProtectEx = (!AppleFirmware)
                ? FALSE
                : HandleBoolean (TokenList, TokenCount);

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'nvram_protect_ex'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (
            StriSubCmp (L"esp_filter", TokenList[0]) ||
            StriSubCmp (L"espfilter", TokenList[0])
        ) {
            DeclineSetting = HandleBoolean (TokenList, TokenCount);
            if (MyStriCmp (TokenList[0], L"enable_esp_filter")) {
                GlobalConfig.ScanAllESP = (DeclineSetting) ? FALSE : TRUE;
            }
            else if (
                MyStriCmp (TokenList[0], L"disable_esp_filter") ||
                MyStriCmp (TokenList[0], L"disable_espfilter")
            ) {
                // DA_TAG: Duplication Purely to Accomodate Deprecation
                //         Change top level 'substring' check when dropped
                GlobalConfig.ScanAllESP = DeclineSetting;
            }

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'enable_esp_filter'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }
        else if (MyStriCmp (TokenList[0], L"scale_ui")) {
            // DA-TAG: Signed integer as can have negative value
            HandleSignedInt (TokenList, TokenCount, &(GlobalConfig.ScaleUI));

            #if REFIT_DEBUG > 0
            if (!AllowIncludes) {
                MuteLogger = FALSE;
                LOG_MSG("%s  - Updated:- 'scale_ui'", OffsetNext);
                MuteLogger = TRUE;
            }
            #endif
        }

        FreeTokenLine (&TokenList, &TokenCount);
    } // for ;;
    FreeTokenLine (&TokenList, &TokenCount);

    // Forced Default Settings
    if (AppleFirmware)  GlobalConfig.RansomDrives   = FALSE;
    if (!AppleFirmware) GlobalConfig.NvramProtect   = FALSE;
    if (!AppleFirmware) GlobalConfig.NvramProtectEx = FALSE;
    if (!AppleFirmware) GlobalConfig.SupplyAppleFB  = FALSE;

    // Prioritise EnableTouch
    if (GlobalConfig.EnableTouch) {
        GlobalConfig.EnableMouse = FALSE;
    }

    if (GlobalConfig.HelpTags) {
        // "TagHelp" feature is active ... Set "found" flag to false
        HiddenTagsFlag = FALSE;
        // Loop through GlobalConfig.ShowTools list to check for "hidden_tags" tool
        for (i = 0; i < NUM_TOOLS; i++) {
            switch (GlobalConfig.ShowTools[i]) {
                case TAG_EXIT:
                case TAG_ABOUT:
                case TAG_SHELL:
                case TAG_GDISK:
                case TAG_REBOOT:
                case TAG_MEMTEST:
                case TAG_GPTSYNC:
                case TAG_NETBOOT:
                case TAG_INSTALL:
                case TAG_MOK_TOOL:
                case TAG_FIRMWARE:
                case TAG_SHUTDOWN:
                case TAG_BOOTORDER:
                case TAG_CSR_ROTATE:
                case TAG_FWUPDATE_TOOL:
                case TAG_INFO_NVRAMCLEAN:
                case TAG_RECOVERY_WINDOWS:
                case TAG_RECOVERY_APPLE:
                    // Continue checking

                break;
                case TAG_HIDDEN:
                    // Tag to end search ... "hidden_tags" tool is already set
                    HiddenTagsFlag = TRUE;

                break;
                default:
                    // Setup help needed ... "hidden_tags" tool is not set
                    GlobalConfig.ShowTools[i] = TAG_HIDDEN;
                    GlobalConfig.HiddenTags   = TRUE;

                    // Tag to end search ... "hidden_tags" tool is now set
                    HiddenTagsFlag = TRUE;
            } // switch

            if (HiddenTagsFlag) {
                // Halt search loop
                break;
            }
        } // for
    } // if GlobalConfig.HelpTags

    if ((GlobalConfig.DontScanFiles) && (GlobalConfig.WindowsRecoveryFiles)) {
        MergeStrings (&(GlobalConfig.DontScanFiles), GlobalConfig.WindowsRecoveryFiles, L',');
    }
    MY_FREE_POOL(File.Buffer);

    SetLinuxMatchPatterns (GlobalConfig.LinuxPrefixes);

    if (!FileExists (SelfDir, L"icons") && !FileExists (SelfDir, GlobalConfig.IconsDir)) {
        #if REFIT_DEBUG > 0
        MuteLogger = FALSE;
        LOG_MSG(
            "%s  - WARN: Cannot Find Icons Directory ... Activating Text-Only Mode",
            OffsetNext
        );
        MuteLogger = TRUE;
        #endif

        GlobalConfig.TextOnly = ForceTextOnly = TRUE;
    }

    SilenceAPFS = GlobalConfig.SilenceAPFS;

    #if REFIT_DEBUG > 0
    MuteLogger = FALSE;

    // Skip this on inner loops
    if (OuterLoop) {
        // Disable further config loading on exiting the outer loop
        ReadLoops = 1000;

        if (!FoundFontImage) {
            LOG_MSG(
                "%s  - WARN: Font image file is invalid ... Using default font",
                OffsetNext
            );
            FoundFontImage = TRUE;
        }

        // Log formating on exiting outer loop
        LOG_MSG("\n");
        LOG_MSG("Process Configuration Options ... Success");
        LOG_MSG("\n\n");
    }
    #endif
} // VOID ReadConfig()

static
VOID AddSubmenu (
    LOADER_ENTRY *Entry,
    REFIT_FILE   *File,
    REFIT_VOLUME *Volume,
    CHAR16       *Title
) {
    REFIT_MENU_SCREEN   *SubScreen;
    LOADER_ENTRY        *SubEntry;
    UINTN                TokenCount;
    CHAR16              *TmpName;
    CHAR16             **TokenList;
    BOOLEAN              TitleVolume;

    #if REFIT_DEBUG > 1
    CHAR16 *FuncTag = L"AddSubmenu";
    #endif

    LOG_SEP(L"X");
    LOG_INCREMENT();
    BREAD_CRUMB(L"%s:  1 - START", FuncTag);

    SubScreen = InitializeSubScreen (Entry);
    BREAD_CRUMB(L"%s:  2", FuncTag);
    if (SubScreen == NULL) {
        BREAD_CRUMB(L"%s:  1a 1 - END:- VOID", FuncTag);
        LOG_DECREMENT();
        LOG_SEP(L"X");

        return;
    }

    // Set defaults for the new entry
    // Will be modified based on lines read from the config file
    BREAD_CRUMB(L"%s:  3", FuncTag);
    SubEntry = InitializeLoaderEntry (Entry);
    BREAD_CRUMB(L"%s:  4", FuncTag);
    if (SubEntry == NULL) {
        BREAD_CRUMB(L"%s:  4a 1", FuncTag);
        FreeMenuScreen (&SubScreen);

        BREAD_CRUMB(L"%s:  4a 2 - END:- VOID", FuncTag);
        LOG_DECREMENT();
        LOG_SEP(L"X");

        return;
    }

    BREAD_CRUMB(L"%s:  5", FuncTag);
    SubEntry->Enabled = TRUE;
    TitleVolume = FALSE;

    while ((SubEntry->Enabled)
        && ((TokenCount = ReadTokenLine (File, &TokenList)) > 0)
        && (StrCmp (TokenList[0], L"}") != 0)
    ) {
        LOG_SEP(L"X");
        BREAD_CRUMB(L"%s:  5a 1 - WHILE LOOP:- START", FuncTag);
        if (MyStriCmp (TokenList[0], L"disabled")) {
            BREAD_CRUMB(L"%s:  5a 1a", FuncTag);
            SubEntry->Enabled = FALSE;
        }
        else if (MyStriCmp (TokenList[0], L"loader") && (TokenCount > 1)) {
            BREAD_CRUMB(L"%s:  5a 1b", FuncTag);

            // Set the boot loader filename
            FreeVolume (&SubEntry->Volume);
            MY_FREE_POOL(SubEntry->LoaderPath);
            SubEntry->LoaderPath = StrDuplicate (TokenList[1]);
            SubEntry->Volume     = CopyVolume (Volume);
        }
        else if (MyStriCmp (TokenList[0], L"volume") && (TokenCount > 1)) {
            BREAD_CRUMB(L"%s:  5a 1c", FuncTag);

            if (FindVolume (&Volume, TokenList[1])) {
                if ((Volume != NULL) && (Volume->IsReadable) && (Volume->RootDir)) {
                    TitleVolume = TRUE;
                    FreeVolume (&SubEntry->Volume);
                    MY_FREE_IMAGE(SubEntry->me.BadgeImage);
                    SubEntry->Volume        = CopyVolume (Volume);
                    SubEntry->me.BadgeImage = egCopyImage (Volume->VolBadgeImage);
                }
            }
        }
        else if (MyStriCmp (TokenList[0], L"initrd")) {
            BREAD_CRUMB(L"%s:  5a 1d", FuncTag);

            if (TokenCount > 1) {
                MY_FREE_POOL(SubEntry->InitrdPath);
                SubEntry->InitrdPath = StrDuplicate (TokenList[1]);
            }
        }
        else if (MyStriCmp (TokenList[0], L"options")) {
            BREAD_CRUMB(L"%s:  5a 1e", FuncTag);

            if (TokenCount > 1) {
                MY_FREE_POOL(SubEntry->LoadOptions);
                SubEntry->LoadOptions = StrDuplicate (TokenList[1]);
            }
        }
        else if (MyStriCmp (TokenList[0], L"add_options") && (TokenCount > 1)) {
            BREAD_CRUMB(L"%s:  5a 1f", FuncTag);

            MergeStrings (&SubEntry->LoadOptions, TokenList[1], L' ');
        }
        else if (MyStriCmp (TokenList[0], L"graphics") && (TokenCount > 1)) {
            BREAD_CRUMB(L"%s:  5a 1g", FuncTag);

            SubEntry->UseGraphicsMode = MyStriCmp (TokenList[1], L"on");
        }
        else {
            BREAD_CRUMB(L"%s:  5a 1h - WARN ... ''%s' Token is Invalid!!", FuncTag, TokenList[0]);
        }

        BREAD_CRUMB(L"%s:  5a 2", FuncTag);
        FreeTokenLine (&TokenList, &TokenCount);

        BREAD_CRUMB(L"%s:  5a 3 - WHILE LOOP:- END", FuncTag);
        LOG_SEP(L"X");
    } // while

    BREAD_CRUMB(L"%s:  6", FuncTag);
    if (!SubEntry->Enabled) {
        BREAD_CRUMB(L"%s:  6a 1", FuncTag);
        FreeMenuEntry ((REFIT_MENU_ENTRY **) SubEntry);

        BREAD_CRUMB(L"%s:  6a 2 - END:- VOID", FuncTag);
        LOG_DECREMENT();
        LOG_SEP(L"X");

        #if REFIT_DEBUG > 0
        ALT_LOG(1, LOG_LINE_NORMAL, L"SubEntry is Disabled!!");
        #endif

        return;
    }

    BREAD_CRUMB(L"%s:  7", FuncTag);
    MY_FREE_POOL(SubEntry->me.Title);
    if (!TitleVolume) {
        BREAD_CRUMB(L"%s:  7a 1", FuncTag);

        SubEntry->me.Title = StrDuplicate (
            (Title != NULL) ? Title : L"Instance: Unknown"
        );
    }
    else {
        BREAD_CRUMB(L"%s:  7b 1", FuncTag);

        TmpName = (Title != NULL)
            ? Title
            : L"Instance: Unknown";
        SubEntry->me.Title = PoolPrint (
            L"Load %s%s%s%s%s",
            TmpName,
            SetVolJoin (TmpName),
            SetVolKind (TmpName, Volume->VolName),
            SetVolFlag (TmpName, Volume->VolName),
            SetVolType (TmpName, Volume->VolName)
        );
    }

    BREAD_CRUMB(L"%s:  8", FuncTag);
    if (SubEntry->InitrdPath != NULL) {
        BREAD_CRUMB(L"%s:  8a 1", FuncTag);
        MergeStrings (&SubEntry->LoadOptions, L"initrd=", L' ');
        MergeStrings (&SubEntry->LoadOptions, SubEntry->InitrdPath, 0);
        MY_FREE_POOL(SubEntry->InitrdPath);
        BREAD_CRUMB(L"%s:  8a 2", FuncTag);
    }

    BREAD_CRUMB(L"%s:  9", FuncTag);
    AddSubMenuEntry (SubScreen, (REFIT_MENU_ENTRY *) SubEntry);

    // DA-TAG: Investigate This
    //         Freeing the SubScreen below causes a hang
    //BREAD_CRUMB(L"%s:  10", FuncTag);
    //FreeMenuScreen (&Entry->me.SubScreen);

    BREAD_CRUMB(L"%s:  10", FuncTag);
    Entry->me.SubScreen = SubScreen;

    BREAD_CRUMB(L"%s:  11 - END:- VOID", FuncTag);
    LOG_DECREMENT();
    LOG_SEP(L"X");
} // VOID AddSubmenu()

// Adds the options from a single config.conf stanza to a new loader entry and returns
// that entry. The calling function is then responsible for adding the entry to the
// list of entries.
static
LOADER_ENTRY * AddStanzaEntries (
    REFIT_FILE   *File,
    REFIT_VOLUME *Volume,
    CHAR16       *Title
) {
    UINTN           TokenCount;
    CHAR16        **TokenList;
    CHAR16         *OurEfiBootNumber;
    CHAR16         *LoadOptions;
    BOOLEAN         RetVal;
    BOOLEAN         HasPath;
    BOOLEAN         DefaultsSet;
    BOOLEAN         AddedSubmenu;
    BOOLEAN         FirmwareBootNum;
    REFIT_VOLUME   *CurrentVolume;
    REFIT_VOLUME   *PreviousVolume;
    LOADER_ENTRY   *Entry;

    #if REFIT_DEBUG > 0
    CHAR16         *MsgStr;
    static BOOLEAN  OtherCall = FALSE;
    #endif

    // Prepare the menu entry
    Entry = InitializeLoaderEntry (NULL);
    if (Entry == NULL) {
        #if REFIT_DEBUG > 0
        ALT_LOG(1, LOG_STAR_SEPARATOR, L"Could Not Initialise Loader Entry for User Configured Stanza");
        #endif

        return NULL;
    }

    Entry->Title = (Title != NULL)
        ? PoolPrint (L"Manual Stanza: %s", Title)
        : StrDuplicate (L"Manual Stanza: Title Not Found");
    Entry->me.Row          = 0;
    Entry->Enabled         = TRUE;
    Entry->Volume          = CopyVolume (Volume);
    Entry->me.BadgeImage   = egCopyImage (Volume->VolBadgeImage);
    Entry->DiscoveryType   = DISCOVERY_TYPE_MANUAL;

    // Parse the config file to add options for a single stanza, terminating when the token
    // is "}" or when the end of file is reached.
    #if REFIT_DEBUG > 0
    /* Exception for LOG_THREE_STAR_SEP */
    ALT_LOG(1, LOG_THREE_STAR_SEP,
        L"%s",
        (!OtherCall) ? L"FIRST STANZA" : L"NEXT STANZA"
    );
    OtherCall = TRUE;

    ALT_LOG(1, LOG_LINE_NORMAL, L"Adding User Configured Stanza:- '%s'", Entry->Title);
    #endif

    CurrentVolume = Volume;
    LoadOptions = OurEfiBootNumber = NULL;
    FirmwareBootNum = AddedSubmenu = DefaultsSet = HasPath = FALSE;

    while (Entry->Enabled
        && ((TokenCount = ReadTokenLine (File, &TokenList)) > 0)
        && (StrCmp (TokenList[0], L"}") != 0)
    ) {
        if (MyStriCmp (TokenList[0], L"disabled")) {
            Entry->Enabled = FALSE;
        }
        else if (MyStriCmp (TokenList[0], L"loader") && (TokenCount > 1)) {
            // Set the boot loader filename
            // DA-TAG: Avoid Memory Leak
            MY_FREE_POOL(Entry->LoaderPath);
            Entry->LoaderPath = StrDuplicate (TokenList[1]);

            HasPath = (Entry->LoaderPath && StrLen (Entry->LoaderPath) > 0);
            if (HasPath) {
                #if REFIT_DEBUG > 0
                ALT_LOG(1, LOG_LINE_NORMAL, L"Adding Loader Path:- '%s'", Entry->LoaderPath);
                #endif

                SetLoaderDefaults (Entry, TokenList[1], CurrentVolume);

                // Discard default options, if any
                MY_FREE_POOL(Entry->LoadOptions);
                DefaultsSet = TRUE;
            }
        }
        else if (MyStriCmp (TokenList[0], L"volume") && (TokenCount > 1)) {
            PreviousVolume = CurrentVolume;
            if (!FindVolume (&CurrentVolume, TokenList[1])) {
                #if REFIT_DEBUG > 0
                ALT_LOG(1, LOG_THREE_STAR_MID,
                    L"Could Not Find Volume:- '%s'",
                    TokenList[1]
                );
                #endif
            }
            else {
                if ((CurrentVolume != NULL) &&
                    (CurrentVolume->RootDir) &&
                    (CurrentVolume->IsReadable)
                ) {
                    #if REFIT_DEBUG > 0
                    ALT_LOG(1, LOG_LINE_NORMAL, L"Adding Volume for '%s'", Entry->Title);
                    #endif

                    // DA-TAG: Avoid Memory Leak
                    FreeVolume (&Entry->Volume);
                    Entry->Volume = CopyVolume (CurrentVolume);

                    // DA-TAG: Avoid Memory Leak
                    MY_FREE_IMAGE(Entry->me.BadgeImage);
                    Entry->me.BadgeImage = egCopyImage (CurrentVolume->VolBadgeImage);
                }
                else {
                    #if REFIT_DEBUG > 0
                    ALT_LOG(1, LOG_THREE_STAR_MID,
                        L"Could Not Add Volume ... Reverting to Previous:- '%s'",
                        PreviousVolume->VolName
                    );
                    #endif

                    // Invalid ... Reset to previous working volume
                    CurrentVolume = PreviousVolume;
                }
            } // if/else !FindVolume
        }
        else if (MyStriCmp (TokenList[0], L"icon") && (TokenCount > 1)) {
            if (!AllowGraphicsMode) {
                #if REFIT_DEBUG > 0
                ALT_LOG(1, LOG_THREE_STAR_MID,
                    L"In AddStanzaEntries ... Skipped Loading Icon in %s Mode",
                    (GlobalConfig.DirectBoot) ? L"DirectBoot" : L"Text Screen"
                );
                #endif
            }
            else {
                #if REFIT_DEBUG > 0
                MsgStr = PoolPrint (L"Adding Icon for '%s'", Entry->Title);
                ALT_LOG(1, LOG_LINE_NORMAL, L"%s", MsgStr);
                MY_FREE_POOL(MsgStr);
                #endif

                // DA-TAG: Avoid Memory Leak
                MY_FREE_IMAGE(Entry->me.Image);
                Entry->me.Image = egLoadIcon (
                    CurrentVolume->RootDir,
                    TokenList[1],
                    GlobalConfig.IconSizes[ICON_SIZE_BIG]
                );

                if (Entry->me.Image == NULL) {
                    // Set dummy image if icon was not found
                    Entry->me.Image = DummyImage (GlobalConfig.IconSizes[ICON_SIZE_BIG]);
                }
            }
        }
        else if (MyStriCmp (TokenList[0], L"initrd") && (TokenCount > 1)) {
            #if REFIT_DEBUG > 0
            ALT_LOG(1, LOG_LINE_NORMAL, L"Adding Initrd for '%s'", Entry->Title);
            #endif

            // DA-TAG: Avoid Memory Leak
            MY_FREE_POOL(Entry->InitrdPath);
            Entry->InitrdPath = StrDuplicate (TokenList[1]);
        }
        else if (MyStriCmp (TokenList[0], L"options") && (TokenCount > 1)) {
            #if REFIT_DEBUG > 0
            ALT_LOG(1, LOG_LINE_NORMAL, L"Adding Options for '%s'", Entry->Title);
            #endif

            // DA-TAG: Avoid Memory Leak
            MY_FREE_POOL(LoadOptions);
            LoadOptions = StrDuplicate (TokenList[1]);
        }
        else if (MyStriCmp (TokenList[0], L"ostype") && (TokenCount > 1)) {
            if (TokenCount > 1) {
                #if REFIT_DEBUG > 0
                ALT_LOG(1, LOG_LINE_NORMAL, L"Adding OS Type for '%s'", Entry->Title);
                #endif

                Entry->OSType = TokenList[1][0];
            }
        }
        else if (MyStriCmp (TokenList[0], L"graphics") && (TokenCount > 1)) {
            #if REFIT_DEBUG > 0
            ALT_LOG(1, LOG_LINE_NORMAL,
                L"Adding Graphics Mode for '%s'",
                (HasPath) ? Entry->LoaderPath : Entry->Title
            );
            #endif

            Entry->UseGraphicsMode = MyStriCmp (TokenList[1], L"on");
        }
        else if (MyStriCmp(TokenList[0], L"firmware_bootnum") && (TokenCount > 1)) {
            #if REFIT_DEBUG > 0
            ALT_LOG(1, LOG_LINE_NORMAL, L"Adding Firmware Bootnum Entry for '%s'", Entry->Title);
            #endif

            Entry->me.Tag        = TAG_FIRMWARE_LOADER;
            Entry->me.BadgeImage = BuiltinIcon (BUILTIN_ICON_VOL_EFI);

            if (Entry->me.BadgeImage == NULL) {
                // Set dummy image if badge was not found
                Entry->me.BadgeImage = DummyImage (GlobalConfig.IconSizes[ICON_SIZE_BADGE]);
            }

            DefaultsSet      = TRUE;
            FirmwareBootNum  = TRUE;
            MY_FREE_POOL(OurEfiBootNumber);
            OurEfiBootNumber = StrDuplicate (TokenList[1]);
        }
        else if (MyStriCmp (TokenList[0], L"submenuentry") && (TokenCount > 1)) {
            #if REFIT_DEBUG > 0
            ALT_LOG(1, LOG_BLANK_LINE_SEP, L"X");
            ALT_LOG(1, LOG_LINE_NORMAL,
                L"Add SubMenu Items to %s",
                (HasPath) ? Entry->LoaderPath : Entry->Title
            );
            #endif

            AddSubmenu (Entry, File, CurrentVolume, TokenList[1]);
            AddedSubmenu = TRUE;
        } // Set options to pass to the loader program

        FreeTokenLine (&TokenList, &TokenCount);
    } // while Entry->Enabled

    if (!Entry->Enabled) {
        FreeMenuEntry ((REFIT_MENU_ENTRY **) Entry);
        MY_FREE_POOL(OurEfiBootNumber);
        MY_FREE_POOL(LoadOptions);

        #if REFIT_DEBUG > 0
        ALT_LOG(1, LOG_LINE_NORMAL, L"Entry is Disabled!!");
        #endif

        return NULL;
    }

    // Set Screen Title
    if (!FirmwareBootNum && Entry->Volume->VolName) {
        Entry->me.Title = PoolPrint (
            L"Load %s%s%s%s%s",
            Entry->Title,
            SetVolJoin (Entry->Title),
            SetVolKind (Entry->Title, Volume->VolName),
            SetVolFlag (Entry->Title, Volume->VolName),
            SetVolType (Entry->Title, Volume->VolName)
        );
    }
    else {
        if (FirmwareBootNum) {
            // Clear potentially wrongly set items
            MY_FREE_POOL(Entry->LoaderPath);
            MY_FREE_POOL(Entry->EfiLoaderPath);
            MY_FREE_POOL(Entry->LoadOptions);
            MY_FREE_POOL(Entry->InitrdPath);

            Entry->me.Title = PoolPrint (
                L"Load %s ... [Firmware Boot Number]",
                Entry->Title
            );

            Entry->EfiBootNum = StrToHex (OurEfiBootNumber, 0, 16);
        }
        else {
            Entry->me.Title = PoolPrint (
                L"Load %s",
                Entry->Title
            );
        }
    }

    // Set load options, if any
    if (LoadOptions && StrLen (LoadOptions) > 0) {
        MY_FREE_POOL(Entry->LoadOptions);
        Entry->LoadOptions = StrDuplicate (LoadOptions);
    }

    if (AddedSubmenu) {
        RetVal = GetReturnMenuEntry (&Entry->me.SubScreen);
        if (!RetVal) {
            FreeMenuScreen (&Entry->me.SubScreen);
        }
    }

    if (Entry->InitrdPath && StrLen (Entry->InitrdPath) > 0) {
        if (Entry->LoadOptions && StrLen (Entry->LoadOptions) > 0) {
            MergeStrings (&Entry->LoadOptions, L"initrd=", L' ');
            MergeStrings (&Entry->LoadOptions, Entry->InitrdPath, 0);
        }
        else {
            Entry->LoadOptions = PoolPrint (
                L"initrd=%s",
                Entry->InitrdPath
            );
        }
        MY_FREE_POOL(Entry->InitrdPath);
    }

    if (!DefaultsSet) {
        // No "loader" line ... use bogus one
        SetLoaderDefaults (Entry, L"\\EFI\\BOOT\\nemo.efi", CurrentVolume);
    }

    if (AllowGraphicsMode && Entry->me.Image == NULL) {
        // Still no icon ... set dummy image
        Entry->me.Image = DummyImage (GlobalConfig.IconSizes[ICON_SIZE_BIG]);
    }

    MY_FREE_POOL(OurEfiBootNumber);
    MY_FREE_POOL(LoadOptions);

    return Entry;
} // static VOID AddStanzaEntries()

// Read the user-configured menu entries from config.conf and add or delete
// entries based on the contents of that file.
VOID ScanUserConfigured (
    CHAR16 *FileName
) {
    EFI_STATUS         Status;
    REFIT_FILE         File;
    CHAR16           **TokenList;
    UINTN              size;
    UINTN              TokenCount;
    LOADER_ENTRY      *Entry;

    #if REFIT_DEBUG > 0
    CHAR16             *TmpName;
    CHAR16             *CountStr;
    UINTN               LogLineType;

    #if REFIT_DEBUG > 1
    CHAR16 *FuncTag = L"ScanUserConfigured";
    #endif
    #endif

    if (!ManualInclude) {
        LOG_SEP(L"X");
        LOG_INCREMENT();
        BREAD_CRUMB(L"%s:  A - START", FuncTag);

        TotalEntryCount = ValidEntryCount = 0;
    }

    if (FileExists (SelfDir, FileName)) {
        Status = RefitReadFile (SelfDir, FileName, &File, &size);
        if (!EFI_ERROR(Status)) {
            while ((TokenCount = ReadTokenLine (&File, &TokenList)) > 0) {
                if (MyStriCmp (TokenList[0], L"menuentry") && (TokenCount > 1)) {
                    TotalEntryCount = TotalEntryCount + 1;
                    Entry = AddStanzaEntries (&File, SelfVolume, TokenList[1]);
                    if (Entry == NULL) {
                        FreeTokenLine (&TokenList, &TokenCount);
                        continue;
                    }

                    ValidEntryCount = ValidEntryCount + 1;
                    #if REFIT_DEBUG > 0
                    TmpName = (SelfVolume->VolName)
                        ? SelfVolume->VolName
                        : Entry->LoaderPath;
                    LOG_MSG(
                        "%s  - Found %s%s%s%s%s",
                        OffsetNext,
                        Entry->Title,
                        SetVolJoin (Entry->Title),
                        SetVolKind (Entry->Title, TmpName),
                        SetVolFlag (Entry->Title, TmpName),
                        SetVolType (Entry->Title, TmpName)
                    );
                    #endif

                    if (Entry->me.SubScreen == NULL) {
                        GenerateSubScreen (Entry, SelfVolume, TRUE);
                    }
                    AddPreparedLoaderEntry (Entry);
                }
                else if (
                    TokenCount == 2 &&
                    ManualInclude == FALSE &&
                    MyStriCmp (TokenList[0], L"include") &&
                    MyStriCmp (FileName, GlobalConfig.ConfigFilename)
                ) {
                    if (!MyStriCmp (TokenList[1], FileName)) {
                        // Scan manual stanza include file
                        #if REFIT_DEBUG > 0
                        #if REFIT_DEBUG < 2
                        ALT_LOG(1, LOG_BLANK_LINE_SEP, L"X");
                        ALT_LOG(1, LOG_THREE_STAR_MID, L"Process Include File for Manual Stanzas");
                        #else
                        LOG_SEP(L"X");
                        BREAD_CRUMB(L"%s:  A1 - INCLUDE FILE (%s): START", FuncTag, TokenList[1]);
                        #endif
                        #endif

                        ManualInclude = TRUE;
                        ScanUserConfigured (TokenList[1]);
                        ManualInclude = FALSE;

                        #if REFIT_DEBUG > 0
                        #if REFIT_DEBUG < 2
                        ALT_LOG(1, LOG_THREE_STAR_MID, L"Scanned Include File for Manual Stanzas");
                        #else
                        BREAD_CRUMB(L"%s:  A2 - INCLUDE FILE (%s): END", FuncTag, TokenList[1]);
                        LOG_SEP(L"X");
                        #endif
                        #endif
                    }
                }

                FreeTokenLine (&TokenList, &TokenCount);
            } // while

            FreeTokenLine (&TokenList, &TokenCount);
        }
    } // if FileExists

    #if REFIT_DEBUG > 0
    CountStr = (ValidEntryCount > 0) ? PoolPrint (L"%d", ValidEntryCount) : NULL;
    LogLineType = (ManualInclude) ? LOG_THREE_STAR_MID : LOG_THREE_STAR_SEP;
    ALT_LOG(1, LogLineType,
        L"Processed %d Manual Stanzas in '%s'%s%s%s%s",
        TotalEntryCount, FileName,
        (TotalEntryCount == 0) ? L"" : L" ... Found ",
        (ValidEntryCount < 1)  ? L"" : CountStr,
        (TotalEntryCount == 0) ? L"" : L" Valid/Active Stanza",
        (ValidEntryCount < 1)  ? L"" : L"s"
    );
    MY_FREE_POOL(CountStr);
    #endif

    if (ManualInclude) {
        ManualInclude = FALSE;
    }
    else {
        BREAD_CRUMB(L"%s:  Z - END:- VOID", FuncTag);
        LOG_DECREMENT();
        LOG_SEP(L"X");
    }
} // VOID ScanUserConfigured()

// Create an options file based on /etc/fstab. The resulting file has two options
// lines, one of which boots the system with "ro root={rootfs}" and the other of
// which boots the system with "ro root={rootfs} single", where "{rootfs}" is the
// filesystem identifier associated with the "/" line in /etc/fstab.
static
REFIT_FILE * GenerateOptionsFromEtcFstab (
    REFIT_VOLUME *Volume
) {
    EFI_STATUS    Status;
    UINTN         TokenCount, i;
    CHAR16      **TokenList;
    CHAR16       *Line;
    CHAR16       *Root;
    REFIT_FILE   *Fstab;
    REFIT_FILE   *Options;

    #if REFIT_DEBUG > 1
    CHAR16 *FuncTag = L"GenerateOptionsFromEtcFstab";
    #endif

    LOG_SEP(L"X");
    LOG_INCREMENT();
    BREAD_CRUMB(L"%s:  1 - START", FuncTag);

    if (!FileExists (Volume->RootDir, L"\\etc\\fstab")) {
        BREAD_CRUMB(L"%s:  1a 1 - END:- return NULL - '\\etc\\fstab' Does Not Exist", FuncTag);
        LOG_DECREMENT();
        LOG_SEP(L"X");

        // Early Return
        return NULL;
    }

    BREAD_CRUMB(L"%s:  2", FuncTag);
    Options = AllocateZeroPool (sizeof (REFIT_FILE));
    if (Options == NULL) {
        BREAD_CRUMB(L"%s:  2a 1 - END:- return NULL - OUT OF MEMORY!!", FuncTag);
        LOG_DECREMENT();
        LOG_SEP(L"X");

        // Early Return
        return NULL;
    }

    BREAD_CRUMB(L"%s:  3", FuncTag);
    Fstab = AllocateZeroPool (sizeof (REFIT_FILE));
    if (Fstab == NULL) {
        BREAD_CRUMB(L"%s:  3a 1 - END:- return NULL - OUT OF MEMORY!!", FuncTag);
        LOG_DECREMENT();
        LOG_SEP(L"X");

        MY_FREE_POOL(Options);

        // Early Return
        return NULL;
    }

    BREAD_CRUMB(L"%s:  4", FuncTag);
    Status = RefitReadFile (Volume->RootDir, L"\\etc\\fstab", Fstab, &i);

    BREAD_CRUMB(L"%s:  5", FuncTag);
    if (CheckError (Status, L"while reading /etc/fstab")) {
        BREAD_CRUMB(L"%s:  5a 1 - END:- return NULL - '\\etc\\fstab' is Unreadable", FuncTag);
        LOG_DECREMENT();
        LOG_SEP(L"X");

        MY_FREE_POOL(Options);
        MY_FREE_POOL(Fstab);

        // Early Return
        return NULL;
    }

    BREAD_CRUMB(L"%s:  6", FuncTag);
    // File read; locate root fs and create entries
    Options->Encoding = ENCODING_UTF16_LE;

    BREAD_CRUMB(L"%s:  7", FuncTag);
    while ((TokenCount = ReadTokenLine (Fstab, &TokenList)) > 0) {
        #if REFIT_DEBUG > 0
        ALT_LOG(1, LOG_THREE_STAR_MID,
            L"Read Line Holding %d Token%s From '/etc/fstab'",
            TokenCount,
            (TokenCount == 1) ? L"" : L"s"
        );
        #endif

        LOG_SEP(L"X");
        BREAD_CRUMB(L"%s:  7a 1 - WHILE LOOP:- START", FuncTag);
        if (TokenCount > 2) {
            BREAD_CRUMB(L"%s:  7a 1a 1", FuncTag);
            if (StrCmp (TokenList[1], L"\\") == 0) {
                BREAD_CRUMB(L"%s:  7a 1a 1a 1", FuncTag);
                Root = PoolPrint (L"%s", TokenList[0]);
            }
            else if (StrCmp (TokenList[2], L"\\") == 0) {
                BREAD_CRUMB(L"%s:  7a 1a 1b 1", FuncTag);
                Root = PoolPrint (L"%s=%s", TokenList[0], TokenList[1]);
            }
            else {
                BREAD_CRUMB(L"%s:  7a 1a 1c 1", FuncTag);
                Root = NULL;
            }

            BREAD_CRUMB(L"%s:  7a 1a 2", FuncTag);
            if (Root && (Root[0] != L'\0')) {
                BREAD_CRUMB(L"%s:  7a 1a 2a 1", FuncTag);
                for (i = 0; i < StrLen (Root); i++) {
                    LOG_SEP(L"X");
                    BREAD_CRUMB(L"%s:  7a 1a 2a 1a 1 - FOR LOOP:- START", FuncTag);
                    if (Root[i] == '\\') {
                        BREAD_CRUMB(L"%s:  7a 1a 2a 1a 1a 1 - Flip Slash", FuncTag);
                        Root[i] = '/';
                    }
                    BREAD_CRUMB(L"%s:  7a 1a 2a 1a 2 - FOR LOOP:- END", FuncTag);
                    LOG_SEP(L"X");
                }

                BREAD_CRUMB(L"%s:  7a 1a 2a 2", FuncTag);
                Line = PoolPrint (L"\"Boot with Normal Options\"    \"ro root=%s\"\n", Root);

                BREAD_CRUMB(L"%s:  7a 1a 2a 3", FuncTag);
                MergeStrings ((CHAR16 **) &(Options->Buffer), Line, 0);

                BREAD_CRUMB(L"%s:  7a 1a 2a 4", FuncTag);
                MY_FREE_POOL(Line);

                BREAD_CRUMB(L"%s:  7a 1a 2a 5", FuncTag);
                Line = PoolPrint (L"\"Boot into Single User Mode\"  \"ro root=%s single\"\n", Root);

                BREAD_CRUMB(L"%s:  7a 1a 2a 6", FuncTag);
                MergeStrings ((CHAR16**) &(Options->Buffer), Line, 0);

                BREAD_CRUMB(L"%s:  7a 1a 2a 7", FuncTag);
                MY_FREE_POOL(Line);

                BREAD_CRUMB(L"%s:  7a 1a 2a 8", FuncTag);
                Options->BufferSize = StrLen ((CHAR16*) Options->Buffer) * sizeof(CHAR16);
            } // if

            BREAD_CRUMB(L"%s:  7a 1a 3", FuncTag);
            MY_FREE_POOL(Root);
        } // if

        BREAD_CRUMB(L"%s:  7a 2", FuncTag);
        FreeTokenLine (&TokenList, &TokenCount);

        BREAD_CRUMB(L"%s:  7a 3 - WHILE LOOP:- END", FuncTag);
        LOG_SEP(L"X");
    } // while

    BREAD_CRUMB(L"%s:  8", FuncTag);
    if (Options->Buffer) {
        BREAD_CRUMB(L"%s:  8a 1", FuncTag);
        Options->Current8Ptr  = (CHAR8 *)Options->Buffer;
        Options->End8Ptr      = Options->Current8Ptr + Options->BufferSize;
        Options->Current16Ptr = (CHAR16 *)Options->Buffer;
        Options->End16Ptr     = Options->Current16Ptr + (Options->BufferSize >> 1);

        BREAD_CRUMB(L"%s:  8a 2", FuncTag);
    }
    else {
        BREAD_CRUMB(L"%s:  8b 1", FuncTag);
        MY_FREE_POOL(Options);
    }

    BREAD_CRUMB(L"%s:  9", FuncTag);
    MY_FREE_POOL(Fstab->Buffer);
    MY_FREE_POOL(Fstab);

    BREAD_CRUMB(L"%s:  10 - END:- return REFIT_FILE *Options", FuncTag);
    LOG_DECREMENT();
    LOG_SEP(L"X");

    return Options;
} // GenerateOptionsFromEtcFstab()


// Create options from partition type codes. Specifically, if the earlier
// partition scan found a partition with a type code corresponding to a root
// filesystem according to the Freedesktop.org Discoverable Partitions Spec
// (http://www.freedesktop.org/wiki/Specifications/DiscoverablePartitionsSpec/),
// this function returns an appropriate file with two lines, one with
// "ro root=/dev/disk/by-partuuid/{GUID}" and the other with that plus "single".
// Note that this function returns the LAST partition found with the
// appropriate type code, so this will work poorly on dual-boot systems or
// if the type code is set incorrectly.
static
REFIT_FILE * GenerateOptionsFromPartTypes (VOID) {
    REFIT_FILE   *Options;
    CHAR16       *Line, *GuidString, *WriteStatus;

    #if REFIT_DEBUG > 1
    CHAR16 *FuncTag = L"GenerateOptionsFromPartTypes";
    #endif

    LOG_SEP(L"X");
    LOG_INCREMENT();
    BREAD_CRUMB(L"%s:  1 - START", FuncTag);
    Options = NULL;
    if (GlobalConfig.DiscoveredRoot) {
        BREAD_CRUMB(L"%s:  1a 1", FuncTag);
        Options = AllocateZeroPool (sizeof (REFIT_FILE));

        BREAD_CRUMB(L"%s:  1a 2", FuncTag);
        if (Options) {
            BREAD_CRUMB(L"%s:  1a 2a 1", FuncTag);
            Options->Encoding = ENCODING_UTF16_LE;

            BREAD_CRUMB(L"%s:  1a 2a 2", FuncTag);
            GuidString = GuidAsString (&(GlobalConfig.DiscoveredRoot->PartGuid));

            BREAD_CRUMB(L"%s:  1a 2a 3", FuncTag);
            WriteStatus = GlobalConfig.DiscoveredRoot->IsMarkedReadOnly ? L"ro" : L"rw";

            BREAD_CRUMB(L"%s:  1a 2a 4", FuncTag);
            ToLower (GuidString);

            BREAD_CRUMB(L"%s:  1a 2a 5", FuncTag);
            if (GuidString) {
                BREAD_CRUMB(L"%s:  1a 2a 5a 1", FuncTag);
                Line = PoolPrint (
                    L"\"Boot with Normal Options\"    \"%s root=/dev/disk/by-partuuid/%s\"\n",
                    WriteStatus, GuidString
                );

                BREAD_CRUMB(L"%s:  1a 2a 5a 2", FuncTag);
                MergeStrings ((CHAR16 **) &(Options->Buffer), Line, 0);

                BREAD_CRUMB(L"%s:  1a 2a 5a 3", FuncTag);
                MY_FREE_POOL(Line);

                BREAD_CRUMB(L"%s:  1a 2a 5a 4", FuncTag);
                Line = PoolPrint (
                    L"\"Boot into Single User Mode\"  \"%s root=/dev/disk/by-partuuid/%s single\"\n",
                    WriteStatus, GuidString
                );

                BREAD_CRUMB(L"%s:  1a 2a 5a 5", FuncTag);
                MergeStrings ((CHAR16**) &(Options->Buffer), Line, 0);

                BREAD_CRUMB(L"%s:  1a 2a 5a 6", FuncTag);
                MY_FREE_POOL(Line);
                MY_FREE_POOL(GuidString);
            } // if (GuidString)

            BREAD_CRUMB(L"%s:  1a 2a 6", FuncTag);
            Options->BufferSize   = StrLen ((CHAR16*) Options->Buffer) * sizeof(CHAR16);
            Options->Current8Ptr  = (CHAR8 *) Options->Buffer;
            Options->End8Ptr      = Options->Current8Ptr + Options->BufferSize;
            Options->Current16Ptr = (CHAR16 *) Options->Buffer;
            Options->End16Ptr     = Options->Current16Ptr + (Options->BufferSize >> 1);
        } // if (Options allocated OK)
        BREAD_CRUMB(L"%s:  1a 3", FuncTag);
    } // if (partition has root GUID)

    BREAD_CRUMB(L"%s:  2 - END:- return REFIT_FILE *Options", FuncTag);
    LOG_DECREMENT();
    LOG_SEP(L"X");

    return Options;
} // REFIT_FILE * GenerateOptionsFromPartTypes()


// Read a Linux kernel options file for a Linux boot loader into memory. The LoaderPath
// and Volume variables identify the location of the options file, but not its name --
// you pass this function the filename of the Linux kernel, initial RAM disk, or other
// file in the target directory, and this function finds the file with a name in the
// comma-delimited list of names specified by LINUX_OPTIONS_FILENAMES within that
// directory and loads it. If a RefindPlus options file can't be found, try to generate
// minimal options from /etc/fstab on the same volume as the kernel. This typically
// works only if the kernel is being read from the Linux root filesystem.
//
// The return value is a pointer to the REFIT_FILE handle for the file, or NULL if
// it was not found.
REFIT_FILE * ReadLinuxOptionsFile (
    IN CHAR16       *LoaderPath,
    IN REFIT_VOLUME *Volume
) {
    EFI_STATUS   Status;
    CHAR16      *OptionsFilename;
    CHAR16      *FullFilename;
    UINTN        size;
    UINTN        i;
    BOOLEAN      GoOn;
    BOOLEAN      FileFound;
    REFIT_FILE  *File;

    #if REFIT_DEBUG > 1
    CHAR16 *FuncTag = L"ReadLinuxOptionsFile";
    #endif

    LOG_SEP(L"X");
    LOG_INCREMENT();
    BREAD_CRUMB(L"%s:  1 - START", FuncTag);

    BREAD_CRUMB(L"%s:  2", FuncTag);
    File = NULL;
    GoOn = TRUE;
    FileFound = FALSE;
    do {
        LOG_SEP(L"X");
        BREAD_CRUMB(L"%s:  2a 1 - DO LOOP:- START", FuncTag);
        i = 0;
        OptionsFilename = FindCommaDelimited (LINUX_OPTIONS_FILENAMES, i++);

        BREAD_CRUMB(L"%s:  2a 2", FuncTag);
        FullFilename = FindPath (LoaderPath);
        MY_FREE_FILE(File);

        BREAD_CRUMB(L"%s:  2a 3", FuncTag);
        if ((OptionsFilename == NULL) || (FullFilename == NULL)) {
            BREAD_CRUMB(L"%s:  2a 3a 1 - DO LOOP:- BREAK - Missing Params", FuncTag);
            LOG_SEP(L"X");

            MY_FREE_POOL(OptionsFilename);
            MY_FREE_POOL(FullFilename);

            break;
        }

        BREAD_CRUMB(L"%s:  2a 4", FuncTag);
        MergeStrings (&FullFilename, OptionsFilename, '\\');

        BREAD_CRUMB(L"%s:  2a 5", FuncTag);
        if (FileExists (Volume->RootDir, FullFilename)) {
            BREAD_CRUMB(L"%s:  2a 5a 1", FuncTag);
            File = AllocateZeroPool (sizeof (REFIT_FILE));
            if (File == NULL) {
                MY_FREE_POOL(OptionsFilename);
                MY_FREE_POOL(FullFilename);

                BREAD_CRUMB(L"%s:  2a 5a 1a 1 - DO LOOP:- BREAK - OUT OF MEMORY", FuncTag);
                BREAD_CRUMB(L"%s:  2a 5a 1a 2 - END:- return NULL", FuncTag);
                LOG_DECREMENT();
                LOG_SEP(L"X");

                return NULL;
            }

            BREAD_CRUMB(L"%s:  2a 5a 2", FuncTag);
            Status = RefitReadFile (Volume->RootDir, FullFilename, File, &size);

            BREAD_CRUMB(L"%s:  2a 5a 3", FuncTag);
            if (!CheckError (Status, L"While Loading the Linux Options File")) {
                BREAD_CRUMB(L"%s:  2a 5a 3a 1", FuncTag);
                GoOn      = FALSE;
                FileFound = TRUE;
            }
            BREAD_CRUMB(L"%s:  2a 5a 4", FuncTag);
        }
        BREAD_CRUMB(L"%s:  2a 6", FuncTag);

        MY_FREE_POOL(OptionsFilename);
        MY_FREE_POOL(FullFilename);

        BREAD_CRUMB(L"%s:  2a 7 - DO LOOP:- END", FuncTag);
        LOG_SEP(L"X");
    } while (GoOn);

    BREAD_CRUMB(L"%s:  3", FuncTag);
    if (!FileFound) {
        BREAD_CRUMB(L"%s:  3a 1", FuncTag);
        // No refindplus_linux.conf or refind_linux.conf file
        // Look for /etc/fstab and try to pull values from there
        MY_FREE_FILE(File);
        File = GenerateOptionsFromEtcFstab (Volume);

        BREAD_CRUMB(L"%s:  3a 2", FuncTag);
        // If still no joy, try to use Freedesktop.org Discoverable Partitions Spec
        if (!File) {
            BREAD_CRUMB(L"%s:  3a 2a 1", FuncTag);
            File = GenerateOptionsFromPartTypes();
        }
        BREAD_CRUMB(L"%s:  3a 3", FuncTag);
    } // if

    BREAD_CRUMB(L"%s:  4 - END:- return REFIT_FILE *File", FuncTag);
    LOG_DECREMENT();
    LOG_SEP(L"X");
    return File;
} // static REFIT_FILE * ReadLinuxOptionsFile()

// Retrieve a single line of options from a Linux kernel options file
CHAR16 * GetFirstOptionsFromFile (
    IN CHAR16       *LoaderPath,
    IN REFIT_VOLUME *Volume
) {
    UINTN         TokenCount;
    CHAR16       *Options;
    CHAR16      **TokenList;
    REFIT_FILE   *File;

    #if REFIT_DEBUG > 1
    CHAR16 *FuncTag = L"GetFirstOptionsFromFile";
    #endif

    LOG_SEP(L"X");
    LOG_INCREMENT();
    BREAD_CRUMB(L"%s:  1 - START", FuncTag);
    File = ReadLinuxOptionsFile (LoaderPath, Volume);

    BREAD_CRUMB(L"%s:  2", FuncTag);
    Options = NULL;
    if (File != NULL) {
        BREAD_CRUMB(L"%s:  2a 1", FuncTag);
        TokenCount = ReadTokenLine(File, &TokenList);

        BREAD_CRUMB(L"%s:  2a 2", FuncTag);
        if (TokenCount > 1) {
            Options = StrDuplicate(TokenList[1]);
        }

        BREAD_CRUMB(L"%s:  2a 3", FuncTag);
        FreeTokenLine (&TokenList, &TokenCount);

        BREAD_CRUMB(L"%s:  2a 4", FuncTag);
        MY_FREE_FILE(File);
    }

    BREAD_CRUMB(L"%s:  3 - END:- return CHAR16 *Options = '%s'", FuncTag,
        Options ? Options : L"NULL"
    );
    LOG_DECREMENT();
    LOG_SEP(L"X");

    return Options;
} // static CHAR16 * GetOptionsFile()
