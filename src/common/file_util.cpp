// Copyright 2013 Dolphin Emulator Project / 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <array>
#include <filesystem>
#include <limits>
#include <memory>
#include <sstream>
#include <unordered_map>
#include "common/assert.h"
#include "common/common_funcs.h"
#include "common/common_paths.h"
#include "common/file_util.h"
#include "common/logging/log.h"

#ifdef _WIN32
#include <windows.h>
// windows.h needs to be included before other windows headers
#include <direct.h> // getcwd
#include <io.h>
#include <shellapi.h>
#include <shlobj.h> // for SHGetFolderPath
#include <tchar.h>
#include "common/string_util.h"

#ifdef _MSC_VER
// 64 bit offsets for MSVC
#define fseeko _fseeki64
#define ftello _ftelli64
#define fileno _fileno
#endif

// 64 bit offsets for MSVC and MinGW. MinGW also needs this for using _wstat64
#define stat _stat64
#define fstat _fstat64

#else
#ifdef __APPLE__
#include <sys/param.h>
#endif
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <pwd.h>
#include <unistd.h>
#endif

#if defined(__APPLE__)
// CFURL contains __attribute__ directives that gcc does not know how to parse, so we need to just
// ignore them if we're not using clang. The macro is only used to prevent linking against
// functions that don't exist on older versions of macOS, and the worst case scenario is a linker
// error, so this is perfectly safe, just inconvenient.
#ifndef __clang__
#define availability(...)
#endif
#include <CoreFoundation/CFBundle.h>
#include <CoreFoundation/CFString.h>
#include <CoreFoundation/CFURL.h>
#ifdef availability
#undef availability
#endif

#endif

#include <algorithm>
#include <sys/stat.h>

namespace Common::FS {
namespace fs = std::filesystem;

bool Exists(const fs::path& path) {
    std::error_code ec;
    return fs::exists(path, ec);
}

bool IsDirectory(const fs::path& path) {
    std::error_code ec;
    return fs::is_directory(path, ec);
}

bool Delete(const fs::path& path) {
    LOG_TRACE(Common_Filesystem, "file {}", path.string());

    // Return true because we care about the file no
    // being there, not the actual delete.
    if (!Exists(path)) {
        LOG_DEBUG(Common_Filesystem, "{} does not exist", path.string());
        return true;
    }

    std::error_code ec;
    return fs::remove(path, ec);
}

bool CreateDir(const fs::path& path) {
    LOG_TRACE(Common_Filesystem, "directory {}", path.string());

    if (Exists(path)) {
        LOG_DEBUG(Common_Filesystem, "path exists {}", path.string());
        return true;
    }

    std::error_code ec;
    const bool success = fs::create_directory(path, ec);

    if (!success) {
        LOG_ERROR(Common_Filesystem, "Unable to create directory: {}", ec.message());
        return false;
    }

    return true;
}

bool CreateDirs(const fs::path& path) {
    LOG_TRACE(Common_Filesystem, "path {}", path.string());

    if (Exists(path)) {
        LOG_DEBUG(Common_Filesystem, "path exists {}", path.string());
        return true;
    }

    std::error_code ec;
    const bool success = fs::create_directories(path, ec);

    if (!success) {
        LOG_ERROR(Common_Filesystem, "Unable to create directories: {}", ec.message());
        return false;
    }

    return true;
}

bool CreateFullPath(const fs::path& path) {
    LOG_TRACE(Common_Filesystem, "path {}", path);

    // Removes trailing slashes and turns any '\' into '/'
    const auto new_path = SanitizePath(path.string(), DirectorySeparator::ForwardSlash);

    if (new_path.rfind('.') == std::string::npos) {
        // The path is a directory
        return CreateDirs(new_path);
    } else {
        // The path is a file
        // Creates directory preceding the last '/'
        return CreateDirs(new_path.substr(0, new_path.rfind('/')));
    }
}

bool Rename(const fs::path& src, const fs::path& dst) {
    LOG_TRACE(Common_Filesystem, "{} --> {}", src.string(), dst.string());

    std::error_code ec;
    fs::rename(src, dst, ec);

    if (ec) {
        LOG_ERROR(Common_Filesystem, "Unable to rename file from {} to {}: {}", src.string(),
                  dst.string(), ec.message());
        return false;
    }

    return true;
}

bool Copy(const fs::path& src, const fs::path& dst) {
    LOG_TRACE(Common_Filesystem, "{} --> {}", src.string(), dst.string());

    std::error_code ec;
    const bool success = fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);

    if (!success) {
        LOG_ERROR(Common_Filesystem, "Unable to copy file {} to {}: {}", src.string(), dst.string(),
                  ec.message());
        return false;
    }

    return true;
}

u64 GetSize(const fs::path& path) {
    std::error_code ec;
    const auto size = fs::file_size(path, ec);

    if (ec) {
        LOG_ERROR(Common_Filesystem, "Unable to retrieve file size ({}): {}", path.string(),
                  ec.message());
        return 0;
    }

    return size;
}

u64 GetSize(FILE* f) {
    // can't use off_t here because it can be 32-bit
    u64 pos = ftello(f);
    if (fseeko(f, 0, SEEK_END) != 0) {
        LOG_ERROR(Common_Filesystem, "GetSize: seek failed {}: {}", fmt::ptr(f), GetLastErrorMsg());
        return 0;
    }
    u64 size = ftello(f);
    if ((size != pos) && (fseeko(f, pos, SEEK_SET) != 0)) {
        LOG_ERROR(Common_Filesystem, "GetSize: seek failed {}: {}", fmt::ptr(f), GetLastErrorMsg());
        return 0;
    }
    return size;
}

bool CreateEmptyFile(const std::string& filename) {
    LOG_TRACE(Common_Filesystem, "{}", filename);

    if (!IOFile(filename, "wb").IsOpen()) {
        LOG_ERROR(Common_Filesystem, "failed {}: {}", filename, GetLastErrorMsg());
        return false;
    }

    return true;
}

bool ForeachDirectoryEntry(u64* num_entries_out, const std::string& directory,
                           DirectoryEntryCallable callback) {
    LOG_TRACE(Common_Filesystem, "directory {}", directory);

    // How many files + directories we found
    u64 found_entries = 0;

    // Save the status of callback function
    bool callback_error = false;

#ifdef _WIN32
    // Find the first file in the directory.
    WIN32_FIND_DATAW ffd;

    HANDLE handle_find = FindFirstFileW(Common::UTF8ToUTF16W(directory + "\\*").c_str(), &ffd);
    if (handle_find == INVALID_HANDLE_VALUE) {
        FindClose(handle_find);
        return false;
    }
    // windows loop
    do {
        const std::string virtual_name(Common::UTF16ToUTF8(ffd.cFileName));
#else
    DIR* dirp = opendir(directory.c_str());
    if (!dirp)
        return false;

    // non windows loop
    while (struct dirent* result = readdir(dirp)) {
        const std::string virtual_name(result->d_name);
#endif

        if (virtual_name == "." || virtual_name == "..")
            continue;

        u64 ret_entries = 0;
        if (!callback(&ret_entries, directory, virtual_name)) {
            callback_error = true;
            break;
        }
        found_entries += ret_entries;

#ifdef _WIN32
    } while (FindNextFileW(handle_find, &ffd) != 0);
    FindClose(handle_find);
#else
    }
    closedir(dirp);
#endif

    if (callback_error)
        return false;

    // num_entries_out is allowed to be specified nullptr, in which case we shouldn't try to set it
    if (num_entries_out != nullptr)
        *num_entries_out = found_entries;
    return true;
}

bool DeleteDirRecursively(const fs::path& path) {
    std::error_code ec;
    fs::remove_all(path, ec);

    if (ec) {
        LOG_ERROR(Common_Filesystem, "Unable to completely delete directory {}: {}", path.string(),
                  ec.message());
        return false;
    }

    return true;
}

void CopyDir(const fs::path& src, const fs::path& dst) {
    constexpr auto copy_flags = fs::copy_options::skip_existing | fs::copy_options::recursive;

    std::error_code ec;
    fs::copy(src, dst, copy_flags, ec);

    if (ec) {
        LOG_ERROR(Common_Filesystem, "Error copying directory {} to {}: {}", src.string(),
                  dst.string(), ec.message());
        return;
    }

    LOG_TRACE(Common_Filesystem, "Successfully copied directory.");
}

std::optional<fs::path> GetCurrentDir() {
    std::error_code ec;
    auto path = fs::current_path(ec);

    if (ec) {
        LOG_ERROR(Common_Filesystem, "Unable to retrieve current working directory: {}",
                  ec.message());
        return std::nullopt;
    }

    return {std::move(path)};
}

bool SetCurrentDir(const fs::path& path) {
    std::error_code ec;
    fs::current_path(path, ec);

    if (ec) {
        LOG_ERROR(Common_Filesystem, "Unable to set {} as working directory: {}", path.string(),
                  ec.message());
        return false;
    }

    return true;
}

#if defined(__APPLE__)
std::string GetBundleDirectory() {
    CFURLRef BundleRef;
    char AppBundlePath[MAXPATHLEN];
    // Get the main bundle for the app
    BundleRef = CFBundleCopyBundleURL(CFBundleGetMainBundle());
    CFStringRef BundlePath = CFURLCopyFileSystemPath(BundleRef, kCFURLPOSIXPathStyle);
    CFStringGetFileSystemRepresentation(BundlePath, AppBundlePath, sizeof(AppBundlePath));
    CFRelease(BundleRef);
    CFRelease(BundlePath);

    return AppBundlePath;
}
#endif

#ifdef _WIN32
const std::string& GetExeDirectory() {
    static std::string exe_path;
    if (exe_path.empty()) {
        wchar_t wchar_exe_path[2048];
        GetModuleFileNameW(nullptr, wchar_exe_path, 2048);
        exe_path = Common::UTF16ToUTF8(wchar_exe_path);
        exe_path = exe_path.substr(0, exe_path.find_last_of('\\'));
    }
    return exe_path;
}

std::string AppDataRoamingDirectory() {
    PWSTR pw_local_path = nullptr;
    // Only supported by Windows Vista or later
    SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &pw_local_path);
    std::string local_path = Common::UTF16ToUTF8(pw_local_path);
    CoTaskMemFree(pw_local_path);
    return local_path;
}
#else
/**
 * @return The user’s home directory on POSIX systems
 */
static const std::string& GetHomeDirectory() {
    static std::string home_path;
    if (home_path.empty()) {
        const char* envvar = getenv("HOME");
        if (envvar) {
            home_path = envvar;
        } else {
            auto pw = getpwuid(getuid());
            ASSERT_MSG(pw,
                       "$HOME isn’t defined, and the current user can’t be found in /etc/passwd.");
            home_path = pw->pw_dir;
        }
    }
    return home_path;
}

/**
 * Follows the XDG Base Directory Specification to get a directory path
 * @param envvar The XDG environment variable to get the value from
 * @return The directory path
 * @sa http://standards.freedesktop.org/basedir-spec/basedir-spec-latest.html
 */
static const std::string GetUserDirectory(const std::string& envvar) {
    const char* directory = getenv(envvar.c_str());

    std::string user_dir;
    if (directory) {
        user_dir = directory;
    } else {
        std::string subdirectory;
        if (envvar == "XDG_DATA_HOME")
            subdirectory = DIR_SEP ".local" DIR_SEP "share";
        else if (envvar == "XDG_CONFIG_HOME")
            subdirectory = DIR_SEP ".config";
        else if (envvar == "XDG_CACHE_HOME")
            subdirectory = DIR_SEP ".cache";
        else
            ASSERT_MSG(false, "Unknown XDG variable {}.", envvar);
        user_dir = GetHomeDirectory() + subdirectory;
    }

    ASSERT_MSG(!user_dir.empty(), "User directory {} mustn’t be empty.", envvar);
    ASSERT_MSG(user_dir[0] == '/', "User directory {} must be absolute.", envvar);

    return user_dir;
}
#endif

std::string GetSysDirectory() {
    std::string sysDir;

#if defined(__APPLE__)
    sysDir = GetBundleDirectory();
    sysDir += DIR_SEP;
    sysDir += SYSDATA_DIR;
#else
    sysDir = SYSDATA_DIR;
#endif
    sysDir += DIR_SEP;

    LOG_DEBUG(Common_Filesystem, "Setting to {}:", sysDir);
    return sysDir;
}

const std::string& GetUserPath(UserPath path, const std::string& new_path) {
    static std::unordered_map<UserPath, std::string> paths;
    auto& user_path = paths[UserPath::UserDir];

    // Set up all paths and files on the first run
    if (user_path.empty()) {
#ifdef _WIN32
        user_path = GetExeDirectory() + DIR_SEP USERDATA_DIR DIR_SEP;
        if (!IsDirectory(user_path)) {
            user_path = AppDataRoamingDirectory() + DIR_SEP EMU_DATA_DIR DIR_SEP;
        } else {
            LOG_INFO(Common_Filesystem, "Using the local user directory");
        }

        paths.emplace(UserPath::ConfigDir, user_path + CONFIG_DIR DIR_SEP);
        paths.emplace(UserPath::CacheDir, user_path + CACHE_DIR DIR_SEP);
#else
        if (Exists(ROOT_DIR DIR_SEP USERDATA_DIR)) {
            user_path = ROOT_DIR DIR_SEP USERDATA_DIR DIR_SEP;
            paths.emplace(UserPath::ConfigDir, user_path + CONFIG_DIR DIR_SEP);
            paths.emplace(UserPath::CacheDir, user_path + CACHE_DIR DIR_SEP);
        } else {
            std::string data_dir = GetUserDirectory("XDG_DATA_HOME");
            std::string config_dir = GetUserDirectory("XDG_CONFIG_HOME");
            std::string cache_dir = GetUserDirectory("XDG_CACHE_HOME");

            user_path = data_dir + DIR_SEP EMU_DATA_DIR DIR_SEP;
            paths.emplace(UserPath::ConfigDir, config_dir + DIR_SEP EMU_DATA_DIR DIR_SEP);
            paths.emplace(UserPath::CacheDir, cache_dir + DIR_SEP EMU_DATA_DIR DIR_SEP);
        }
#endif
        paths.emplace(UserPath::SDMCDir, user_path + SDMC_DIR DIR_SEP);
        paths.emplace(UserPath::NANDDir, user_path + NAND_DIR DIR_SEP);
        paths.emplace(UserPath::LoadDir, user_path + LOAD_DIR DIR_SEP);
        paths.emplace(UserPath::DumpDir, user_path + DUMP_DIR DIR_SEP);
        paths.emplace(UserPath::ScreenshotsDir, user_path + SCREENSHOTS_DIR DIR_SEP);
        paths.emplace(UserPath::ShaderDir, user_path + SHADER_DIR DIR_SEP);
        paths.emplace(UserPath::SysDataDir, user_path + SYSDATA_DIR DIR_SEP);
        paths.emplace(UserPath::KeysDir, user_path + KEYS_DIR DIR_SEP);
        // TODO: Put the logs in a better location for each OS
        paths.emplace(UserPath::LogDir, user_path + LOG_DIR DIR_SEP);
    }

    if (!new_path.empty()) {
        if (!IsDirectory(new_path)) {
            LOG_ERROR(Common_Filesystem, "Invalid path specified {}", new_path);
            return paths[path];
        } else {
            paths[path] = new_path;
        }

        switch (path) {
        case UserPath::RootDir:
            user_path = paths[UserPath::RootDir] + DIR_SEP;
            break;
        case UserPath::UserDir:
            user_path = paths[UserPath::RootDir] + DIR_SEP;
            paths[UserPath::ConfigDir] = user_path + CONFIG_DIR DIR_SEP;
            paths[UserPath::CacheDir] = user_path + CACHE_DIR DIR_SEP;
            paths[UserPath::SDMCDir] = user_path + SDMC_DIR DIR_SEP;
            paths[UserPath::NANDDir] = user_path + NAND_DIR DIR_SEP;
            break;
        default:
            break;
        }
    }

    return paths[path];
}

std::string GetHactoolConfigurationPath() {
#ifdef _WIN32
    PWSTR pw_local_path = nullptr;
    if (SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &pw_local_path) != S_OK)
        return "";
    std::string local_path = Common::UTF16ToUTF8(pw_local_path);
    CoTaskMemFree(pw_local_path);
    return local_path + "\\.switch";
#else
    return GetHomeDirectory() + "/.switch";
#endif
}

std::string GetNANDRegistrationDir(bool system) {
    if (system)
        return GetUserPath(UserPath::NANDDir) + "system/Contents/registered/";
    return GetUserPath(UserPath::NANDDir) + "user/Contents/registered/";
}

std::size_t WriteStringToFile(bool text_file, const std::string& filename, std::string_view str) {
    return IOFile(filename, text_file ? "w" : "wb").WriteString(str);
}

std::size_t ReadFileToString(bool text_file, const std::string& filename, std::string& str) {
    IOFile file(filename, text_file ? "r" : "rb");

    if (!file.IsOpen())
        return 0;

    str.resize(static_cast<u32>(file.GetSize()));
    return file.ReadArray(&str[0], str.size());
}

void SplitFilename83(const std::string& filename, std::array<char, 9>& short_name,
                     std::array<char, 4>& extension) {
    static constexpr std::string_view forbidden_characters = ".\"/\\[]:;=, ";

    // On a FAT32 partition, 8.3 names are stored as a 11 bytes array, filled with spaces.
    short_name = {{' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', '\0'}};
    extension = {{' ', ' ', ' ', '\0'}};

    auto point = filename.rfind('.');
    if (point == filename.size() - 1) {
        point = filename.rfind('.', point);
    }

    // Get short name.
    int j = 0;
    for (char letter : filename.substr(0, point)) {
        if (forbidden_characters.find(letter, 0) != std::string::npos) {
            continue;
        }
        if (j == 8) {
            // TODO(Link Mauve): also do that for filenames containing a space.
            // TODO(Link Mauve): handle multiple files having the same short name.
            short_name[6] = '~';
            short_name[7] = '1';
            break;
        }
        short_name[j++] = static_cast<char>(std::toupper(letter));
    }

    // Get extension.
    if (point != std::string::npos) {
        j = 0;
        for (char letter : filename.substr(point + 1, 3)) {
            extension[j++] = static_cast<char>(std::toupper(letter));
        }
    }
}

std::vector<std::string> SplitPathComponents(std::string_view filename) {
    std::string copy(filename);
    std::replace(copy.begin(), copy.end(), '\\', '/');
    std::vector<std::string> out;

    std::stringstream stream(copy);
    std::string item;
    while (std::getline(stream, item, '/')) {
        out.push_back(std::move(item));
    }

    return out;
}

std::string_view GetParentPath(std::string_view path) {
    const auto name_bck_index = path.rfind('\\');
    const auto name_fwd_index = path.rfind('/');
    std::size_t name_index;

    if (name_bck_index == std::string_view::npos || name_fwd_index == std::string_view::npos) {
        name_index = std::min(name_bck_index, name_fwd_index);
    } else {
        name_index = std::max(name_bck_index, name_fwd_index);
    }

    return path.substr(0, name_index);
}

std::string_view GetPathWithoutTop(std::string_view path) {
    if (path.empty()) {
        return path;
    }

    while (path[0] == '\\' || path[0] == '/') {
        path.remove_prefix(1);
        if (path.empty()) {
            return path;
        }
    }

    const auto name_bck_index = path.find('\\');
    const auto name_fwd_index = path.find('/');
    return path.substr(std::min(name_bck_index, name_fwd_index) + 1);
}

std::string_view GetFilename(std::string_view path) {
    const auto name_index = path.find_last_of("\\/");

    if (name_index == std::string_view::npos) {
        return {};
    }

    return path.substr(name_index + 1);
}

std::string_view GetExtensionFromFilename(std::string_view name) {
    const std::size_t index = name.rfind('.');

    if (index == std::string_view::npos) {
        return {};
    }

    return name.substr(index + 1);
}

std::string_view RemoveTrailingSlash(std::string_view path) {
    if (path.empty()) {
        return path;
    }

    if (path.back() == '\\' || path.back() == '/') {
        path.remove_suffix(1);
        return path;
    }

    return path;
}

std::string SanitizePath(std::string_view path_, DirectorySeparator directory_separator) {
    std::string path(path_);
    char type1 = directory_separator == DirectorySeparator::BackwardSlash ? '/' : '\\';
    char type2 = directory_separator == DirectorySeparator::BackwardSlash ? '\\' : '/';

    if (directory_separator == DirectorySeparator::PlatformDefault) {
#ifdef _WIN32
        type1 = '/';
        type2 = '\\';
#endif
    }

    std::replace(path.begin(), path.end(), type1, type2);

    auto start = path.begin();
#ifdef _WIN32
    // allow network paths which start with a double backslash (e.g. \\server\share)
    if (start != path.end())
        ++start;
#endif
    path.erase(std::unique(start, path.end(),
                           [type2](char c1, char c2) { return c1 == type2 && c2 == type2; }),
               path.end());
    return std::string(RemoveTrailingSlash(path));
}

IOFile::IOFile() = default;

IOFile::IOFile(const std::string& filename, const char openmode[], int flags) {
    void(Open(filename, openmode, flags));
}

IOFile::~IOFile() {
    Close();
}

IOFile::IOFile(IOFile&& other) noexcept {
    Swap(other);
}

IOFile& IOFile::operator=(IOFile&& other) noexcept {
    Swap(other);
    return *this;
}

void IOFile::Swap(IOFile& other) noexcept {
    std::swap(m_file, other.m_file);
}

bool IOFile::Open(const std::string& filename, const char openmode[], int flags) {
    Close();
    bool m_good;
#ifdef _WIN32
    if (flags != 0) {
        m_file = _wfsopen(Common::UTF8ToUTF16W(filename).c_str(),
                          Common::UTF8ToUTF16W(openmode).c_str(), flags);
        m_good = m_file != nullptr;
    } else {
        m_good = _wfopen_s(&m_file, Common::UTF8ToUTF16W(filename).c_str(),
                           Common::UTF8ToUTF16W(openmode).c_str()) == 0;
    }
#else
    m_file = std::fopen(filename.c_str(), openmode);
    m_good = m_file != nullptr;
#endif

    return m_good;
}

bool IOFile::Close() {
    if (!IsOpen() || 0 != std::fclose(m_file)) {
        return false;
    }

    m_file = nullptr;
    return true;
}

u64 IOFile::GetSize() const {
    if (IsOpen()) {
        return FS::GetSize(m_file);
    }
    return 0;
}

bool IOFile::Seek(s64 off, int origin) const {
    return IsOpen() && 0 == fseeko(m_file, off, origin);
}

u64 IOFile::Tell() const {
    if (IsOpen()) {
        return ftello(m_file);
    }
    return std::numeric_limits<u64>::max();
}

bool IOFile::Flush() {
    return IsOpen() && 0 == std::fflush(m_file);
}

std::size_t IOFile::ReadImpl(void* data, std::size_t length, std::size_t data_size) const {
    if (!IsOpen()) {
        return std::numeric_limits<std::size_t>::max();
    }

    if (length == 0) {
        return 0;
    }

    DEBUG_ASSERT(data != nullptr);

    return std::fread(data, data_size, length, m_file);
}

std::size_t IOFile::WriteImpl(const void* data, std::size_t length, std::size_t data_size) {
    if (!IsOpen()) {
        return std::numeric_limits<std::size_t>::max();
    }

    if (length == 0) {
        return 0;
    }

    DEBUG_ASSERT(data != nullptr);

    return std::fwrite(data, data_size, length, m_file);
}

bool IOFile::Resize(u64 size) {
    return IsOpen() && 0 ==
#ifdef _WIN32
                           // ector: _chsize sucks, not 64-bit safe
                           // F|RES: changed to _chsize_s. i think it is 64-bit safe
                           _chsize_s(_fileno(m_file), size)
#else
                           // TODO: handle 64bit and growing
                           ftruncate(fileno(m_file), size)
#endif
        ;
}

} // namespace Common::FS
