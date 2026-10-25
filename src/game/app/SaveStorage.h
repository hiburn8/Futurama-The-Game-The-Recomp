// The U:\ save games (orig: the statically linked XAPI the memory-card code 0x000E0000..0x000E1700 calls:
// XFindFirstSaveGame 0x000F6D4A / XFindNextSaveGame 0x000F6E51 / XFindClose 0x000F6E98, XCreateSaveGame 0x000F6A17,
// XDeleteSaveGame 0x000F6C7A, CreateFileA 0x000F7770, ReadFile 0x000F7225, GetFileSize 0x000F7735, WriteFileEx
// 0x000F76E4 (its completion routine runs at the next alertable SleepEx 0x000F654E), CloseHandle 0x000F7207,
// SetFileAttributesA 0x000F7BFC, CopyFileA 0x000F7BDD, GetDiskFreeSpaceExA 0x000F7CED, XCalculateSignatureBegin /
// Update / End 0x000F71F3 / 0x000F717C / 0x000F7196). docs/analysis/frontend_port.md 1.8, KNOWN_ISSUES #66.
//
// The game code (MemoryCards.cpp) calls this surface where the original calls the XAPI, with the same paths
// ("U:\<dir>\<file>"), dispositions and handles. MemorySaveStorage is the boot oracle's model (tools/difftest/
// boot_oracle.py "THE SAVE-STORAGE MODEL"): the saves in creation order, directories named by a counter, one handle
// counter for find / file / signature handles (0x5AE00000 / 0x5AF00000 / 0x5AD00000 + n), a write's completion queued
// for the next alertable wait.
// HostSaveStorage is the same model kept in a host directory (fut_game: %APPDATA%\FuturamaRecomp\saves).
//
// The signature: the console's XCalculateSignature* is an HMAC-SHA1 keyed by the console / title, which the port does
// not have; both storages use the oracle's STAND-IN, SHA-1 of the updated bytes (so a written file verifies when read
// back, and the files are byte-identical to the oracle's; they are not exchangeable with an Xbox: intentional).
#pragma once

#include "core/Types.h"

#include <array>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace fut::game::app {

class SaveStorage {
public:
    static constexpr u32 kInvalid = 0xFFFFFFFFu;      // INVALID_HANDLE_VALUE
    struct FindData {                                 // XGAME_FIND_DATA (the parts the game reads)
        std::string dir;                              // cFileName (the directory)
        std::string directory;                        // szSaveGameDirectory "U:\<dir>\"
        std::u16string name;                          // szSaveGameName
    };
    virtual ~SaveStorage() = default;
    // XFindFirstSaveGame("U:\", &data): a find handle, kInvalid when there is none. `site` = the caller's return
    // address (the trace's "from").
    virtual u32 FindFirst(FindData& out, u32 site) = 0;
    virtual bool FindNext(u32 find, FindData& out) = 0;
    virtual void FindClose(u32 find) = 0;
    // XCreateSaveGame("U:\", name, disposition (1 CREATE_NEW, 3 OPEN_EXISTING, 4 OPEN_ALWAYS), flags, path): 0 or the
    // error (183 exists, 3 not found); `path` = "U:\<dir>\".
    virtual u32 CreateSaveGame(const std::u16string& name, u32 disposition, u32 flags, std::string& path, u32 site) = 0;
    virtual u32 DeleteSaveGame(const std::u16string& name) = 0;
    // CreateFileA(path, access, 0, 0, disposition (1 CREATE_NEW, 2 CREATE_ALWAYS, 3 OPEN_EXISTING, 4 OPEN_ALWAYS),
    // flags, 0): a handle or kInvalid.
    virtual u32 CreateFile(const std::string& path, u32 access, u32 disposition) = 0;
    // ReadFile(h, buf, n, &read, 0): false = the call failed; `out` = the bytes read.
    virtual bool ReadFile(u32 h, u32 n, std::vector<u8>& out) = 0;
    virtual u32 GetFileSize(u32 h) = 0;
    // WriteFileEx(h, data, n, overlapped, routine): its completion (error, bytes) is queued for the next SleepEx.
    virtual bool WriteFileEx(u32 h, const u8* data, u32 n, u32 routine) = 0;
    virtual bool CloseHandle(u32 h) = 0;              // (false for a handle not of the storage: the XAPI's own)
    virtual bool SetFileAttributes(const std::string& path, u32 attributes) = 0;
    // CopyFileA(source, path, FALSE): `source` a disc path ("d:\SaveGameIcons\saveimage.xbx").
    virtual bool CopyFile(const std::string& source, const std::string& path) = 0;
    // GetDiskFreeSpaceExA("U:\", &free, &total, &totalFree): lpFreeBytesAvailable.
    virtual u64 FreeBytes(u32 site) = 0;
    // SleepEx(0, TRUE): the oldest queued completion (false: none; the wait returns at once).
    struct Completion {
        u32 routine = 0, error = 0, bytes = 0;
    };
    virtual bool TakeCompletion(Completion& c) = 0;
    // XCalculateSignatureBegin(0) / Update / End (the stand-in: SHA-1 of the updated bytes).
    virtual u32 SignatureBegin() = 0;
    virtual bool SignatureUpdate(u32 h, const u8* data, u32 n) = 0;
    virtual bool SignatureEnd(u32 h, std::array<u8, 20>& digest) = 0;
};

// The boot oracle's model of U:\ (an empty card at the start, 0x10000000 bytes free).
class MemorySaveStorage : public SaveStorage {
public:
    struct File {
        std::string name;                             // (as first created; the key is its lower-case form)
        std::vector<u8> data;
    };
    struct Save {
        std::u16string name;
        std::string dir;                              // 12 upper-case hex digits of a counter from 1
        std::map<std::string, File> files;
    };

    u32 FindFirst(FindData& out, u32 site) override;
    bool FindNext(u32 find, FindData& out) override;
    void FindClose(u32 find) override;
    u32 CreateSaveGame(const std::u16string& name, u32 disposition, u32 flags, std::string& path, u32 site) override;
    u32 DeleteSaveGame(const std::u16string& name) override;
    u32 CreateFile(const std::string& path, u32 access, u32 disposition) override;
    bool ReadFile(u32 h, u32 n, std::vector<u8>& out) override;
    u32 GetFileSize(u32 h) override;
    bool WriteFileEx(u32 h, const u8* data, u32 n, u32 routine) override;
    bool CloseHandle(u32 h) override;
    bool SetFileAttributes(const std::string& path, u32 attributes) override;
    bool CopyFile(const std::string& source, const std::string& path) override;
    u64 FreeBytes(u32 site) override;
    bool TakeCompletion(Completion& c) override;
    u32 SignatureBegin() override;
    bool SignatureUpdate(u32 h, const u8* data, u32 n) override;
    bool SignatureEnd(u32 h, std::array<u8, 20>& digest) override;

    std::vector<Save> saves;                          // creation order
    u64 freeBytes = 0x10000000;
    // (port) a save game present before the game starts (the oracle's CARD seed: the next directory number, no
    // trace line, no Changed call); `files` keyed as the model keys them (lower case).
    void Seed(const std::u16string& name, const std::vector<File>& files);
    // The disc's files (CopyFile's source, "d:\" stripped, '\' as given); unset = every copy fails.
    std::function<bool(const std::string& path, std::vector<u8>& out)> readDisc;

protected:
    // A save game or one of its files changed (HostSaveStorage persists it).
    virtual void Changed(const Save& s) { (void)s; }
    virtual void Removed(const Save& s) { (void)s; }
    u32 nextDir_ = 1;                                 // the directory counter

private:
    struct Handle {
        bool find = false;
        std::vector<std::u16string> names;            // (find) the names at FindFirst
        size_t pos = 0;
        std::string dir, key;                         // (file)
        size_t filePos = 0;
        bool written = false;
    };
    Save* FindSave(const std::u16string* name, const std::string* dir);
    std::map<u32, Handle> handles_;
    std::map<u32, std::vector<u8>> sigs_;
    std::deque<Completion> completions_;
    u32 nextHandle_ = 1;
};

// The model in a host directory: one sub-directory per save game (its files, and its display name in name.txt as
// UTF-8), loaded at construction, written back on every change. Its directory counter continues after the highest
// existing one.
class HostSaveStorage : public MemorySaveStorage {
public:
    explicit HostSaveStorage(std::filesystem::path root);
    u64 FreeBytes(u32 site) override;

protected:
    void Changed(const Save& s) override;
    void Removed(const Save& s) override;

private:
    std::filesystem::path root_;
};

}  // namespace fut::game::app
