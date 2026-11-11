// Virtual file system + cellFs / sys_fs_* (syscalls 801-834).
//   /dev_bdvd/...  -> <game root>/...
//   /app_home/...  -> <game root>/PS3_GAME/USRDIR/...
//   /dev_hdd0/...  -> <user root>/dev_hdd0/...  (also dev_hdd1, dev_usb000)
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>

#include <filesystem>
#include <map>
#include <mutex>
#include <vector>

#include "kernel.h"

namespace fs = std::filesystem;

static std::string g_game_root, g_user_root;

// UTF-8 std::string <-> path helpers (C++20 changed u8path/u8string to char8_t)
static fs::path P(const std::string& s) { return fs::path((const char8_t*)s.c_str()); }
static std::string S(const fs::path& p) {
    auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

void vfs_init(const std::string& game_root, const std::string& user_root) {
    g_game_root = S(fs::absolute(P(game_root)));
    g_user_root = S(fs::absolute(P(user_root)));
    for (const char* d : {"dev_hdd0", "dev_hdd1", "dev_usb000"}) fs::create_directories(P(g_user_root) / d);
    LOG_I("vfs", "game root: %s", g_game_root.c_str());
    LOG_I("vfs", "user root: %s", g_user_root.c_str());
}

std::string vfs_host_path(const std::string& guest) {
    auto starts = [&](const char* p) { return guest.rfind(p, 0) == 0; };
    std::string rest;
    if (starts("/dev_bdvd")) return g_game_root + guest.substr(9);
    if (starts("/app_home")) return g_game_root + "/PS3_GAME/USRDIR" + guest.substr(9);
    for (const char* d : {"/dev_hdd0", "/dev_hdd1", "/dev_usb000"})
        if (starts(d)) return g_user_root + guest;
    return {};
}

// ---------------------------------------------------------------- descriptors
struct FsHandle {
    int host_fd = -1;  // file
    bool is_dir = false;
    std::vector<std::pair<std::string, bool>> entries;  // dir: (name, is_dir)
    size_t dir_pos = 0;
    std::string guest_path;
};
static std::mutex g_fs_mutex;
static std::map<uint32_t, FsHandle> g_handles;
static uint32_t g_next_fd = 3;

enum : uint32_t {
    CELL_FS_O_RDONLY = 0, CELL_FS_O_WRONLY = 1, CELL_FS_O_RDWR = 2, CELL_FS_O_ACCMODE = 3,
    CELL_FS_O_CREAT = 0x40, CELL_FS_O_EXCL = 0x80, CELL_FS_O_TRUNC = 0x200, CELL_FS_O_APPEND = 0x400,
};

static uint32_t fs_open(const std::string& path, uint32_t flags, uint32_t fd_ptr) {
    std::string host = vfs_host_path(path);
    if (host.empty()) {
        LOG_W("fs", "open '%s': unmapped device", path.c_str());
        return CELL_ENOENT;
    }
    int oflag = _O_BINARY;
    switch (flags & CELL_FS_O_ACCMODE) {
        case CELL_FS_O_RDONLY: oflag |= _O_RDONLY; break;
        case CELL_FS_O_WRONLY: oflag |= _O_WRONLY; break;
        default: oflag |= _O_RDWR; break;
    }
    if (flags & CELL_FS_O_CREAT) oflag |= _O_CREAT;
    if (flags & CELL_FS_O_EXCL) oflag |= _O_EXCL;
    if (flags & CELL_FS_O_TRUNC) oflag |= _O_TRUNC;
    if (flags & CELL_FS_O_APPEND) oflag |= _O_APPEND;
    std::error_code ec;
    if (fs::is_directory(P(host), ec)) return CELL_EISDIR;
    int fd = _wopen(P(host).wstring().c_str(), oflag, _S_IREAD | _S_IWRITE);
    if (fd < 0) {
        LOG_I("fs", "open '%s' (flags 0x%X) -> not found", path.c_str(), flags);
        return errno == EEXIST ? CELL_EEXIST : CELL_ENOENT;
    }
    std::lock_guard<std::mutex> lk(g_fs_mutex);
    uint32_t gfd = g_next_fd++;
    g_handles[gfd].host_fd = fd;
    g_handles[gfd].guest_path = path;
    vm::write32(fd_ptr, gfd);
    LOG_I("fs", "open '%s' (flags 0x%X) -> fd %u", path.c_str(), flags, gfd);
    return CELL_OK;
}

static FsHandle* get_handle(uint32_t fd) {
    auto it = g_handles.find(fd);
    return it == g_handles.end() ? nullptr : &it->second;
}

static uint32_t fs_read(uint32_t fd, uint32_t buf, uint64_t n, uint32_t nread_ptr) {
    FsHandle* h;
    {
        std::lock_guard<std::mutex> lk(g_fs_mutex);
        h = get_handle(fd);
    }
    if (!h || h->is_dir) return CELL_EBADF;
    uint64_t total = 0;
    while (total < n) {
        unsigned chunk = (unsigned)std::min<uint64_t>(n - total, 1u << 30);
        int r = _read(h->host_fd, vm::ptr(buf + (uint32_t)total), chunk);
        if (r <= 0) break;
        total += r;
    }
    if (nread_ptr) vm::write64(nread_ptr, total);
    LOG_T("fs", "read fd %u -> 0x%llX/0x%llX bytes", fd, (unsigned long long)total, (unsigned long long)n);
    return CELL_OK;
}

static uint32_t fs_write(uint32_t fd, uint32_t buf, uint64_t n, uint32_t nwrite_ptr) {
    FsHandle* h;
    {
        std::lock_guard<std::mutex> lk(g_fs_mutex);
        h = get_handle(fd);
    }
    if (!h || h->is_dir) return CELL_EBADF;
    int r = _write(h->host_fd, vm::ptr(buf), (unsigned)n);
    if (nwrite_ptr) vm::write64(nwrite_ptr, r > 0 ? r : 0);
    return CELL_OK;
}

static uint32_t fs_close(uint32_t fd) {
    std::lock_guard<std::mutex> lk(g_fs_mutex);
    FsHandle* h = get_handle(fd);
    if (!h) return CELL_EBADF;
    if (h->host_fd >= 0) _close(h->host_fd);
    g_handles.erase(fd);
    return CELL_OK;
}

static uint32_t fs_lseek(uint32_t fd, int64_t off, uint32_t whence, uint32_t pos_ptr) {
    FsHandle* h;
    {
        std::lock_guard<std::mutex> lk(g_fs_mutex);
        h = get_handle(fd);
    }
    if (!h || h->is_dir) return CELL_EBADF;
    int64_t r = _lseeki64(h->host_fd, off, whence);  // SEEK_SET/CUR/END match (0/1/2)
    if (r < 0) return CELL_EINVAL;
    if (pos_ptr) vm::write64(pos_ptr, (uint64_t)r);
    return CELL_OK;
}

// CellFsStat (52 bytes, 4-byte aligned): mode uid gid atime mtime ctime size blksize
static void write_stat(uint32_t sb, bool is_dir, uint64_t size, int64_t mtime) {
    vm::write32(sb + 0, is_dir ? (0040000 | 0777) : (0100000 | 0666));
    vm::write32(sb + 4, 0);
    vm::write32(sb + 8, 0);
    vm::write64(sb + 12, mtime);
    vm::write64(sb + 20, mtime);
    vm::write64(sb + 28, mtime);
    vm::write64(sb + 36, is_dir ? 4096 : size);
    vm::write64(sb + 44, 4096);
}

static uint32_t fs_stat(const std::string& path, uint32_t sb) {
    std::string host = vfs_host_path(path);
    struct _stat64 st;
    if (host.empty() || _wstat64(P(host).wstring().c_str(), &st) != 0) {
        LOG_I("fs", "stat '%s' -> not found", path.c_str());
        return CELL_ENOENT;
    }
    write_stat(sb, (st.st_mode & _S_IFDIR) != 0, (uint64_t)st.st_size, st.st_mtime);
    LOG_I("fs", "stat '%s' -> %s size %lld", path.c_str(), (st.st_mode & _S_IFDIR) ? "dir" : "file",
          (long long)st.st_size);
    return CELL_OK;
}

static uint32_t fs_fstat(uint32_t fd, uint32_t sb) {
    FsHandle* h;
    {
        std::lock_guard<std::mutex> lk(g_fs_mutex);
        h = get_handle(fd);
    }
    if (!h) return CELL_EBADF;
    if (h->is_dir) {
        write_stat(sb, true, 0, 0);
        return CELL_OK;
    }
    struct _stat64 st;
    if (_fstat64(h->host_fd, &st) != 0) return CELL_EBADF;
    write_stat(sb, false, (uint64_t)st.st_size, st.st_mtime);
    return CELL_OK;
}

static uint32_t fs_opendir(const std::string& path, uint32_t fd_ptr) {
    std::string host = vfs_host_path(path);
    std::error_code ec;
    if (host.empty() || !fs::is_directory(P(host), ec)) {
        LOG_I("fs", "opendir '%s' -> not found", path.c_str());
        return CELL_ENOENT;
    }
    FsHandle h;
    h.is_dir = true;
    h.guest_path = path;
    h.entries.push_back({".", true});
    h.entries.push_back({"..", true});
    for (auto& e : fs::directory_iterator(P(host), ec)) h.entries.push_back({S(e.path().filename()), e.is_directory()});
    std::lock_guard<std::mutex> lk(g_fs_mutex);
    uint32_t gfd = g_next_fd++;
    g_handles[gfd] = std::move(h);
    vm::write32(fd_ptr, gfd);
    LOG_I("fs", "opendir '%s' -> fd %u", path.c_str(), gfd);
    return CELL_OK;
}

// CellFsDirent: u8 d_type (1 dir, 2 regular), u8 d_namlen, char d_name[256]
static uint32_t fs_readdir(uint32_t fd, uint32_t dirent, uint32_t nread_ptr) {
    std::lock_guard<std::mutex> lk(g_fs_mutex);
    FsHandle* h = get_handle(fd);
    if (!h || !h->is_dir) return CELL_EBADF;
    if (h->dir_pos >= h->entries.size()) {
        vm::write64(nread_ptr, 0);
        return CELL_OK;
    }
    auto& [name, is_dir] = h->entries[h->dir_pos++];
    vm::write8(dirent, is_dir ? 1 : 2);
    vm::write8(dirent + 1, (uint8_t)std::min<size_t>(name.size(), 255));
    memset(vm::ptr(dirent + 2), 0, 256);
    memcpy(vm::ptr(dirent + 2), name.c_str(), std::min<size_t>(name.size(), 255));
    vm::write64(nread_ptr, 258);
    return CELL_OK;
}

static uint32_t fs_mkdir(const std::string& path) {
    std::string host = vfs_host_path(path);
    if (host.empty()) return CELL_ENOENT;
    std::error_code ec;
    if (fs::exists(P(host), ec)) return CELL_EEXIST;
    return fs::create_directory(P(host), ec) ? CELL_OK : CELL_ENOENT;
}

static uint32_t fs_remove(const std::string& path, bool dir) {
    std::string host = vfs_host_path(path);
    std::error_code ec;
    if (host.empty() || !fs::exists(P(host), ec)) return CELL_ENOENT;
    if (fs::is_directory(P(host), ec) != dir) return dir ? CELL_ENOTDIR : CELL_EISDIR;
    return fs::remove(P(host), ec) ? CELL_OK : CELL_EBUSY;
}

static uint32_t fs_rename(const std::string& from, const std::string& to) {
    std::string a = vfs_host_path(from), b = vfs_host_path(to);
    std::error_code ec;
    if (a.empty() || b.empty()) return CELL_ENOENT;
    fs::rename(P(a), P(b), ec);
    return ec ? CELL_ENOENT : CELL_OK;
}

static uint32_t fs_truncate_fd(uint32_t fd, uint64_t size) {
    std::lock_guard<std::mutex> lk(g_fs_mutex);
    FsHandle* h = get_handle(fd);
    if (!h || h->is_dir) return CELL_EBADF;
    return _chsize_s(h->host_fd, (int64_t)size) == 0 ? CELL_OK : CELL_EINVAL;
}

// ---------------------------------------------------------------- cellFs (sys_fs library)
#define GSTR(n) vm::read_cstr(ARG32(n))
HLE_FUNC(sys_fs, cellFsOpen) { RET(fs_open(GSTR(0), ARG32(1), ARG32(2))); }
HLE_FUNC(sys_fs, cellFsRead) { RET(fs_read(ARG32(0), ARG32(1), ARG(2), ARG32(3))); }
HLE_FUNC(sys_fs, cellFsWrite) { RET(fs_write(ARG32(0), ARG32(1), ARG(2), ARG32(3))); }
HLE_FUNC(sys_fs, cellFsClose) { RET(fs_close(ARG32(0))); }
HLE_FUNC(sys_fs, cellFsLseek) { RET(fs_lseek(ARG32(0), (int64_t)ARG(1), ARG32(2), ARG32(3))); }
HLE_FUNC(sys_fs, cellFsStat) { RET(fs_stat(GSTR(0), ARG32(1))); }
HLE_FUNC(sys_fs, cellFsFstat) { RET(fs_fstat(ARG32(0), ARG32(1))); }
HLE_FUNC(sys_fs, cellFsOpendir) { RET(fs_opendir(GSTR(0), ARG32(1))); }
HLE_FUNC(sys_fs, cellFsReaddir) { RET(fs_readdir(ARG32(0), ARG32(1), ARG32(2))); }
HLE_FUNC(sys_fs, cellFsClosedir) { RET(fs_close(ARG32(0))); }
HLE_FUNC(sys_fs, cellFsMkdir) { RET(fs_mkdir(GSTR(0))); }
HLE_FUNC(sys_fs, cellFsRmdir) { RET(fs_remove(GSTR(0), true)); }
HLE_FUNC(sys_fs, cellFsUnlink) { RET(fs_remove(GSTR(0), false)); }
HLE_FUNC(sys_fs, cellFsRename) { RET(fs_rename(GSTR(0), GSTR(1))); }

// ---------------------------------------------------------------- direct syscalls
SYSCALL(801, sys_fs_open) { RET(fs_open(GSTR(0), ARG32(1), ARG32(2))); }
SYSCALL(802, sys_fs_read) { RET(fs_read(ARG32(0), ARG32(1), ARG(2), ARG32(3))); }
SYSCALL(803, sys_fs_write) { RET(fs_write(ARG32(0), ARG32(1), ARG(2), ARG32(3))); }
SYSCALL(804, sys_fs_close) { RET(fs_close(ARG32(0))); }
SYSCALL(805, sys_fs_opendir) { RET(fs_opendir(GSTR(0), ARG32(1))); }
SYSCALL(806, sys_fs_readdir) { RET(fs_readdir(ARG32(0), ARG32(1), ARG32(2))); }
SYSCALL(807, sys_fs_closedir) { RET(fs_close(ARG32(0))); }
SYSCALL(808, sys_fs_stat) { RET(fs_stat(GSTR(0), ARG32(1))); }
SYSCALL(809, sys_fs_fstat) { RET(fs_fstat(ARG32(0), ARG32(1))); }
SYSCALL(811, sys_fs_mkdir) { RET(fs_mkdir(GSTR(0))); }
SYSCALL(812, sys_fs_rename) { RET(fs_rename(GSTR(0), GSTR(1))); }
SYSCALL(813, sys_fs_rmdir) { RET(fs_remove(GSTR(0), true)); }
SYSCALL(814, sys_fs_unlink) { RET(fs_remove(GSTR(0), false)); }
SYSCALL(817, sys_fs_fcntl) {
    LOG_W("fs", "fcntl(fd/op 0x%X, 0x%X, arg 0x%X, size 0x%X) - ignored", ARG32(0), ARG32(1), ARG32(2), ARG32(3));
    RET(CELL_OK);
}
SYSCALL(818, sys_fs_lseek) { RET(fs_lseek(ARG32(0), (int64_t)ARG(1), ARG32(2), ARG32(3))); }
SYSCALL(820, sys_fs_fsync) { RET(CELL_OK); }
SYSCALL(832, sys_fs_ftruncate) { RET(fs_truncate_fd(ARG32(0), ARG(1))); }
SYSCALL(834, sys_fs_chmod) { RET(CELL_OK); }
