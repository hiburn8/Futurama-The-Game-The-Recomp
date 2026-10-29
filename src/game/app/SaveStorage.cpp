// The U:\ save games: the boot oracle's model and its host-directory form; see SaveStorage.h.
#include "game/app/SaveStorage.h"

#include "core/Sha1.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <system_error>

namespace fut::game::app {

namespace {

constexpr u32 kSaveHandle = 0x5AF00000u, kFindHandle = 0x5AE00000u, kSigHandle = 0x5AD00000u;

std::string Hex8(u32 v) {
    char b[16];
    std::snprintf(b, sizeof b, "%08x", v);
    return b;
}
std::string HexX(u64 v) {                             // Python's "{:x}"
    char b[24];
    std::snprintf(b, sizeof b, "%llx", static_cast<unsigned long long>(v));
    return b;
}
std::string PyHex(u64 v) { return "0x" + HexX(v); }   // Python's "{:#x}"

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string Narrow(const std::u16string& w) {         // (the names are ASCII; the oracle decodes UTF-16)
    std::string s;
    for (char16_t c : w) s += c < 0x80 ? static_cast<char>(c) : '?';
    return s;
}

std::string Underscored(const std::u16string& w) {
    std::string s = Narrow(w);
    std::replace(s.begin(), s.end(), ' ', '_');
    return s;
}

bool OnU(const std::string& path) { return path.size() >= 3 && Lower(path.substr(0, 3)) == "u:\\"; }

// "U:\<dir>\<file>" -> (dir, file)
std::pair<std::string, std::string> Split(const std::string& path) {
    const std::string rest = path.size() > 3 ? path.substr(3) : std::string();
    const size_t k = rest.find('\\');
    if (k == std::string::npos) return {rest, std::string()};
    return {rest.substr(0, k), rest.substr(k + 1)};
}

}  // namespace

void MemorySaveStorage::Seed(const std::u16string& name, const std::vector<File>& files) {
    char d[16];
    std::snprintf(d, sizeof d, "%012X", nextDir_++);
    Save s{name, d, {}};
    for (const File& f : files) s.files[Lower(f.name)] = f;
    saves.push_back(std::move(s));
}

MemorySaveStorage::Save* MemorySaveStorage::FindSave(const std::u16string* name, const std::string* dir) {
    for (Save& s : saves)
        if ((name && s.name == *name) || (dir && Lower(s.dir) == Lower(*dir))) return &s;
    return nullptr;
}

u32 MemorySaveStorage::FindFirst(FindData& out, [[maybe_unused]] u32 site) {
    if (saves.empty()) {                              // (last error 18)
        return kInvalid;
    }
    const u32 h = kFindHandle + nextHandle_++;
    Handle& hd = handles_[h];
    hd.find = true;
    for (const Save& s : saves) hd.names.push_back(s.name);
    hd.pos = 1;
    const Save& s = saves.front();
    out = FindData{s.dir, "U:\\" + s.dir + "\\", s.name};
    return h;
}

bool MemorySaveStorage::FindNext(u32 find, FindData& out) {
    auto it = handles_.find(find);
    if (it == handles_.end() || !it->second.find) return false;   // (last error 6)
    Handle& hd = it->second;
    while (hd.pos < hd.names.size()) {
        const Save* s = FindSave(&hd.names[hd.pos], nullptr);
        ++hd.pos;
        if (s) {
            out = FindData{s->dir, "U:\\" + s->dir + "\\", s->name};
            return true;
        }
    }
    return false;
}

void MemorySaveStorage::FindClose(u32 find) {
    if (!handles_.erase(find)) return;                // (not the model's: the XAPI's own)
}

u32 MemorySaveStorage::CreateSaveGame(const std::u16string& name, u32 disposition, [[maybe_unused]] u32 flags,
                                      std::string& path, [[maybe_unused]] u32 site) {
    Save* s = FindSave(&name, nullptr);
    bool created = false;
    u32 ret;
    if (disposition == 1 && s) {
        ret = 183;                                    // ERROR_ALREADY_EXISTS
    } else if (disposition == 3 && !s) {
        ret = 3;                                      // ERROR_PATH_NOT_FOUND
    } else {
        if (!s) {
            char d[16];
            std::snprintf(d, sizeof d, "%012X", nextDir_++);
            saves.push_back(Save{name, d, {}});
            s = &saves.back();
            created = true;
        }
        ret = 0;
        path = "U:\\" + s->dir + "\\";
    }
    if (created) Changed(*s);
    return ret;
}

u32 MemorySaveStorage::DeleteSaveGame(const std::u16string& name) {
    auto it = std::find_if(saves.begin(), saves.end(), [&](const Save& s) { return s.name == name; });
    const bool found = it != saves.end();
    if (found) {
        Removed(*it);
        saves.erase(it);
    }
    return found ? 0 : 2;
}

u32 MemorySaveStorage::CreateFile(const std::string& path, [[maybe_unused]] u32 access, u32 disposition) {
    if (!OnU(path)) return kInvalid;                  // (not on U:\: the XAPI's own)
    const auto [d, fname] = Split(path);
    Save* s = FindSave(nullptr, &d);
    const std::string key = Lower(fname);
    u32 h = kInvalid;
    if (!s || fname.empty()) {
        // (last error 3)
    } else if (disposition == 3 && !s->files.count(key)) {
        // OPEN_EXISTING of a missing file (last error 2)
    } else {
        if (disposition == 1 || disposition == 2 || !s->files.count(key))   // CREATE_NEW / CREATE_ALWAYS: truncated
            s->files[key] = File{fname, {}};
        h = kSaveHandle + nextHandle_++;
        Handle& hd = handles_[h];
        hd.dir = s->dir;
        hd.key = key;
    }
    return h;
}

bool MemorySaveStorage::ReadFile(u32 h, u32 n, std::vector<u8>& out) {
    auto it = handles_.find(h);
    if (it == handles_.end() || it->second.find) return false;
    Handle& hd = it->second;
    Save* s = FindSave(nullptr, &hd.dir);
    const File* f = s && s->files.count(hd.key) ? &s->files[hd.key] : nullptr;
    const size_t size = f ? f->data.size() : 0;
    const size_t k = hd.filePos < size ? std::min<size_t>(n, size - hd.filePos) : 0;
    out.clear();
    if (k) out.assign(f->data.begin() + std::ptrdiff_t(hd.filePos), f->data.begin() + std::ptrdiff_t(hd.filePos + k));
    hd.filePos += k;
    return true;
}

u32 MemorySaveStorage::GetFileSize(u32 h) {
    auto it = handles_.find(h);
    if (it == handles_.end() || it->second.find) return kInvalid;
    Save* s = FindSave(nullptr, &it->second.dir);
    const u32 n = s && s->files.count(it->second.key) ? u32(s->files[it->second.key].data.size()) : 0;
    return n;
}

bool MemorySaveStorage::WriteFileEx(u32 h, const u8* data, u32 n, u32 routine) {
    auto it = handles_.find(h);
    if (it == handles_.end() || it->second.find) return false;
    Handle& hd = it->second;
    Save* s = FindSave(nullptr, &hd.dir);
    if (s && s->files.count(hd.key)) {
        std::vector<u8>& f = s->files[hd.key].data;
        if (f.size() < hd.filePos + n) f.resize(hd.filePos + n);
        std::copy(data, data + n, f.begin() + std::ptrdiff_t(hd.filePos));
        hd.filePos += n;
        hd.written = true;
    }
    completions_.push_back(Completion{routine, 0, n});   // (OVERLAPPED: success, n bytes)
    return true;
}

bool MemorySaveStorage::CloseHandle(u32 h) {
    auto it = handles_.find(h);
    if (it == handles_.end()) return false;           // (not the model's: the XAPI's own)
    if (!it->second.find) {
        Save* s = FindSave(nullptr, &it->second.dir);
        if (it->second.written && s && s->files.count(it->second.key)) {
            Changed(*s);
        }
    }
    handles_.erase(it);
    return true;
}

bool MemorySaveStorage::SetFileAttributes(const std::string& path, [[maybe_unused]] u32 attributes) {
    if (!OnU(path)) return false;
    const auto [d, fname] = Split(path);
    Save* s = FindSave(nullptr, &d);
    const bool ok = s && s->files.count(Lower(fname));
    return ok;
}

bool MemorySaveStorage::CopyFile(const std::string& source, const std::string& path) {
    if (!OnU(path)) return false;
    const auto [d, fname] = Split(path);
    Save* s = FindSave(nullptr, &d);
    std::string rel = source;
    std::replace(rel.begin(), rel.end(), '\\', '/');
    if (rel.size() >= 3 && Lower(rel.substr(0, 3)) == "d:/") rel = rel.substr(3);
    else rel.erase(0, rel.find_first_not_of('/'));
    std::vector<u8> data;
    const bool have = readDisc && readDisc(rel, data);
    const bool ok = s && !fname.empty() && have;
    if (ok) {
        [[maybe_unused]] File& f = s->files[Lower(fname)] = File{fname, std::move(data)};
        Changed(*s);
    }
    return ok;
}

u64 MemorySaveStorage::FreeBytes([[maybe_unused]] u32 site) {
    return freeBytes;
}

bool MemorySaveStorage::TakeCompletion(Completion& c) {
    if (completions_.empty()) return false;
    c = completions_.front();
    completions_.pop_front();
    return true;
}

u32 MemorySaveStorage::SignatureBegin() {
    const u32 h = kSigHandle + nextHandle_++;
    sigs_[h].clear();
    return h;
}

bool MemorySaveStorage::SignatureUpdate(u32 h, const u8* data, u32 n) {
    auto it = sigs_.find(h);
    if (it == sigs_.end()) return false;              // (ERROR_INVALID_HANDLE)
    it->second.insert(it->second.end(), data, data + n);
    return true;
}

bool MemorySaveStorage::SignatureEnd(u32 h, std::array<u8, 20>& digest) {
    std::vector<u8> data;
    if (auto it = sigs_.find(h); it != sigs_.end()) {
        data = std::move(it->second);
        sigs_.erase(it);
    }
    digest = crypto::Sha1::Of(data.data(), data.size());
    return true;
}

// ---- the host directory ---------------------------------------------------------------------------------------------

HostSaveStorage::HostSaveStorage(std::filesystem::path root) : root_(std::move(root)) {
    // Do not turn an inaccessible save store into an apparently empty card.
    std::filesystem::create_directories(root_);
    std::vector<std::filesystem::path> dirs;
    for (const auto& e : std::filesystem::directory_iterator(root_))
        if (e.is_directory()) dirs.push_back(e.path());
    std::sort(dirs.begin(), dirs.end());              // (12 hex digits: the creation order)
    for (const std::filesystem::path& p : dirs) {
        Save s;
        s.dir = p.filename().string();
        for (const auto& f : std::filesystem::directory_iterator(p)) {
            if (!f.is_regular_file()) continue;
            std::ifstream in;
            in.exceptions(std::ios::failbit | std::ios::badbit);
            in.open(f.path(), std::ios::binary);
            std::vector<u8> data((std::istreambuf_iterator<char>(in)), {});
            const std::string name = f.path().filename().string();
            if (name == "name.txt") {
                for (size_t i = 0; i < data.size();) {   // UTF-8 -> UTF-16
                    u32 c = data[i++];
                    if (c >= 0xE0 && i + 1 < data.size()) c = ((c & 15) << 12) | ((data[i] & 63u) << 6) | (data[i + 1] & 63u), i += 2;
                    else if (c >= 0xC0 && i < data.size()) c = ((c & 31) << 6) | (data[i] & 63u), ++i;
                    s.name += static_cast<char16_t>(c);
                }
                continue;
            }
            s.files[Lower(name)] = File{name, std::move(data)};
        }
        nextDir_ = std::max<u32>(nextDir_, static_cast<u32>(std::strtoul(s.dir.c_str(), nullptr, 16)) + 1);
        saves.push_back(std::move(s));
    }
}

u64 HostSaveStorage::FreeBytes(u32 site) {
    std::error_code ec;
    const auto sp = std::filesystem::space(root_, ec);
    freeBytes = ec ? 0 : static_cast<u64>(sp.available);
    return MemorySaveStorage::FreeBytes(site);
}

void HostSaveStorage::Changed(const Save& s) {
    std::error_code ec;
    const std::filesystem::path p = root_ / s.dir;
    std::filesystem::create_directories(p, ec);
    std::string name;
    for (char16_t c : s.name) {                       // UTF-16 -> UTF-8
        if (c < 0x80) name += static_cast<char>(c);
        else if (c < 0x800) name += static_cast<char>(0xC0 | (c >> 6)), name += static_cast<char>(0x80 | (c & 63));
        else
            name += static_cast<char>(0xE0 | (c >> 12)), name += static_cast<char>(0x80 | ((c >> 6) & 63)),
                name += static_cast<char>(0x80 | (c & 63));
    }
    std::ofstream(p / "name.txt", std::ios::binary).write(name.data(), std::streamsize(name.size()));
    for (const auto& [key, f] : s.files)
        std::ofstream(p / f.name, std::ios::binary)
            .write(reinterpret_cast<const char*>(f.data.data()), std::streamsize(f.data.size()));
}

void HostSaveStorage::Removed(const Save& s) {
    std::error_code ec;
    std::filesystem::remove_all(root_ / s.dir, ec);
}

}  // namespace fut::game::app
