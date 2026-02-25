"""
mpq_reader.py — ctypes wrapper around StormLib.dll for MPQ archive access.

Matches the C++ MpqArchiveSet pattern from map-editor/src/mpq/mpq_archive.cpp:
opens MPQ archives in priority order (patches first, base archives last) so
ReadFile returns data from the highest-priority archive that contains a file.
"""

import ctypes
import ctypes.wintypes
import os
from pathlib import Path

# ---------------------------------------------------------------------------
# StormLib ctypes bindings
# ---------------------------------------------------------------------------

_STORMLIB_PATH = r"C:\vcpkg\installed\x64-windows\bin\StormLib.dll"

_storm = ctypes.WinDLL(_STORMLIB_PATH)

HANDLE = ctypes.wintypes.HANDLE
DWORD = ctypes.wintypes.DWORD
LPDWORD = ctypes.POINTER(DWORD)
BOOL = ctypes.wintypes.BOOL
LPVOID = ctypes.c_void_p

MPQ_OPEN_READ_ONLY = 0x00000100
SFILE_OPEN_FROM_MPQ = 0x00000000
SFILE_INVALID_SIZE = 0xFFFFFFFF

# SFileOpenArchive(szMpqName, dwPriority, dwFlags, phMpq) -> BOOL
_storm.SFileOpenArchive.argtypes = [ctypes.c_char_p, DWORD, DWORD, ctypes.POINTER(HANDLE)]
_storm.SFileOpenArchive.restype = BOOL

# SFileCloseArchive(hMpq) -> BOOL
_storm.SFileCloseArchive.argtypes = [HANDLE]
_storm.SFileCloseArchive.restype = BOOL

# SFileOpenFileEx(hMpq, szFileName, dwSearchScope, phFile) -> BOOL
_storm.SFileOpenFileEx.argtypes = [HANDLE, ctypes.c_char_p, DWORD, ctypes.POINTER(HANDLE)]
_storm.SFileOpenFileEx.restype = BOOL

# SFileGetFileSize(hFile, pdwFileSizeHigh) -> DWORD
_storm.SFileGetFileSize.argtypes = [HANDLE, LPDWORD]
_storm.SFileGetFileSize.restype = DWORD

# SFileReadFile(hFile, lpBuffer, dwToRead, pdwRead, lpOverlapped) -> BOOL
_storm.SFileReadFile.argtypes = [HANDLE, LPVOID, DWORD, LPDWORD, LPVOID]
_storm.SFileReadFile.restype = BOOL

# SFileCloseFile(hFile) -> BOOL
_storm.SFileCloseFile.argtypes = [HANDLE]
_storm.SFileCloseFile.restype = BOOL

# SFileHasFile(hMpq, szFileName) -> BOOL
_storm.SFileHasFile.argtypes = [HANDLE, ctypes.c_char_p]
_storm.SFileHasFile.restype = BOOL

# SFileFindFirstFile(hMpq, szMask, lpFindFileData, szListFile) -> HANDLE
# SFILE_FIND_DATA is 0x120 bytes: cFileName[260], szPlainName*, hashIndex, blockIndex,
# fileSize, fileFlags, compSize, fileTimeLo, fileTimeHi, locale
class SFILE_FIND_DATA(ctypes.Structure):
    _fields_ = [
        ("cFileName", ctypes.c_char * 1024),
        ("szPlainName", ctypes.c_char_p),
        ("dwHashIndex", DWORD),
        ("dwBlockIndex", DWORD),
        ("dwFileSize", DWORD),
        ("dwFileFlags", DWORD),
        ("dwCompSize", DWORD),
        ("dwFileTimeLo", DWORD),
        ("dwFileTimeHi", DWORD),
        ("lcLocale", DWORD),
    ]

_storm.SFileFindFirstFile.argtypes = [HANDLE, ctypes.c_char_p, ctypes.POINTER(SFILE_FIND_DATA), ctypes.c_char_p]
_storm.SFileFindFirstFile.restype = HANDLE

_storm.SFileFindNextFile.argtypes = [HANDLE, ctypes.POINTER(SFILE_FIND_DATA)]
_storm.SFileFindNextFile.restype = BOOL

_storm.SFileFindClose.argtypes = [HANDLE]
_storm.SFileFindClose.restype = BOOL


# ---------------------------------------------------------------------------
# MpqArchiveSet — mirrors C++ class
# ---------------------------------------------------------------------------

# Priority order: highest-numbered patches first
_MPQ_ORDER = [
    "patch-3.MPQ",
    "patch-2.MPQ",
    "patch.MPQ",
    "lichking.MPQ",
    "expansion.MPQ",
    "common-2.MPQ",
    "common.MPQ",
]

# Locale MPQ order (format strings with %s = locale name)
_LOCALE_MPQ_ORDER = [
    "patch-%s-3.MPQ",
    "patch-%s-2.MPQ",
    "patch-%s.MPQ",
    "locale-%s.MPQ",
]


class MpqArchiveSet:
    """Opens multiple MPQ archives from a WoW Data directory and reads files
    by internal path, returning data from the highest-priority archive."""

    def __init__(self):
        self._archives: list[HANDLE] = []
        self._data_dir: str = ""

    def open(self, data_dir: str) -> bool:
        """Open all MPQ archives in the given directory (or parent with Data/ subfolder)."""
        self.close()

        data_dir = str(data_dir)

        # Auto-detect Data subfolder
        data_sub = os.path.join(data_dir, "Data")
        if os.path.isdir(data_sub):
            has_mpq = any(
                f.lower().endswith(".mpq")
                for f in os.listdir(data_sub)
                if os.path.isfile(os.path.join(data_sub, f))
            )
            if has_mpq:
                data_dir = data_sub

        self._data_dir = data_dir

        # Open main archives in priority order
        for name in _MPQ_ORDER:
            path = os.path.join(data_dir, name)
            if not os.path.exists(path):
                continue
            h = HANDLE()
            if _storm.SFileOpenArchive(path.encode(), 0, MPQ_OPEN_READ_ONLY, ctypes.byref(h)):
                self._archives.append(h)

        # Open locale archives
        try:
            for entry in os.scandir(data_dir):
                if not entry.is_dir() or len(entry.name) != 4:
                    continue
                locale = entry.name
                for fmt in _LOCALE_MPQ_ORDER:
                    name = fmt % locale
                    path = os.path.join(entry.path, name)
                    if not os.path.exists(path):
                        continue
                    h = HANDLE()
                    if _storm.SFileOpenArchive(path.encode(), 0, MPQ_OPEN_READ_ONLY, ctypes.byref(h)):
                        self._archives.append(h)
        except OSError:
            pass

        return len(self._archives) > 0

    def close(self):
        for h in self._archives:
            _storm.SFileCloseArchive(h)
        self._archives.clear()

    def read_file(self, internal_path: str) -> bytes | None:
        """Read a file from the highest-priority archive that contains it."""
        path_bytes = internal_path.encode()
        for h in self._archives:
            hFile = HANDLE()
            if not _storm.SFileOpenFileEx(h, path_bytes, SFILE_OPEN_FROM_MPQ, ctypes.byref(hFile)):
                continue

            file_size = _storm.SFileGetFileSize(hFile, None)
            if file_size == SFILE_INVALID_SIZE or file_size == 0:
                _storm.SFileCloseFile(hFile)
                continue

            buf = ctypes.create_string_buffer(file_size)
            bytes_read = DWORD(0)
            if _storm.SFileReadFile(hFile, buf, file_size, ctypes.byref(bytes_read), None):
                _storm.SFileCloseFile(hFile)
                if bytes_read.value == file_size:
                    return buf.raw
            _storm.SFileCloseFile(hFile)

        return None

    def has_file(self, internal_path: str) -> bool:
        """Check if a file exists in any archive."""
        path_bytes = internal_path.encode()
        for h in self._archives:
            if _storm.SFileHasFile(h, path_bytes):
                return True
        return False

    def list_files(self, mask: str, max_results: int = 10000) -> list[str]:
        """List files matching a mask (e.g. 'World\\Maps\\Azeroth\\*.adt')."""
        results = []
        find_data = SFILE_FIND_DATA()
        mask_bytes = mask.encode()
        for h in self._archives:
            hFind = _storm.SFileFindFirstFile(h, mask_bytes, ctypes.byref(find_data), None)
            if not hFind:
                continue
            try:
                while True:
                    name = find_data.cFileName.decode("utf-8", errors="replace").rstrip("\x00")
                    if name and name not in results:
                        results.append(name)
                    if len(results) >= max_results:
                        break
                    if not _storm.SFileFindNextFile(hFind, ctypes.byref(find_data)):
                        break
            finally:
                _storm.SFileFindClose(hFind)
            if len(results) >= max_results:
                break
        return results

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()

    def __del__(self):
        self.close()
