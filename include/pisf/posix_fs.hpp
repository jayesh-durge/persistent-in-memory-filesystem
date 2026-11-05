#ifndef PISF_POSIX_FS_HPP
#define PISF_POSIX_FS_HPP

#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace pisf::posix_fs {

inline std::string join(const std::string& directory, const std::string& name) {
    if (directory.empty() || directory.back() == '/') return directory + name;
    return directory + '/' + name;
}

inline bool ensure_directories(const std::string& path, std::string& err) {
    if (path.empty()) {
        err = "directory path cannot be empty";
        return false;
    }
    std::string current;
    size_t pos = 0;
    if (path.front() == '/') {
        current = "/";
        pos = 1;
    }
    while (pos < path.size()) {
        while (pos < path.size() && path[pos] == '/') ++pos;
        size_t end = path.find('/', pos);
        if (end == std::string::npos) end = path.size();
        if (end == pos) break;
        std::string part = path.substr(pos, end - pos);
        if (part == "." || part == "..") {
            err = "directory path cannot contain '.' or '..' components";
            return false;
        }
        if (!current.empty() && current.back() != '/') current += '/';
        current += part;
        if (::mkdir(current.c_str(), 0755) != 0 && errno != EEXIST) {
            err = "mkdir() failed for '" + current + "': " + std::strerror(errno);
            return false;
        }
        struct stat st{};
        if (::stat(current.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
            err = "path component '" + current + "' is not a directory";
            return false;
        }
        pos = end;
    }
    return true;
}

inline bool inspect(const std::string& path, struct stat& st, bool& exists, std::string& err) {
    if (::stat(path.c_str(), &st) == 0) {
        exists = true;
        return true;
    }
    if (errno == ENOENT) {
        exists = false;
        return true;
    }
    exists = false;
    err = "stat() failed for '" + path + "': " + std::strerror(errno);
    return false;
}

inline bool is_regular_file(const std::string& path, bool& result, std::string& err) {
    struct stat st{};
    bool exists = false;
    if (!inspect(path, st, exists, err)) return false;
    result = exists && S_ISREG(st.st_mode);
    return true;
}

inline bool is_directory(const std::string& path, bool& result, std::string& err) {
    struct stat st{};
    bool exists = false;
    if (!inspect(path, st, exists, err)) return false;
    result = exists && S_ISDIR(st.st_mode);
    return true;
}

inline bool list_directory(const std::string& path, std::vector<std::string>& names, std::string& err) {
    names.clear();
    DIR* dir = ::opendir(path.c_str());
    if (dir == nullptr) {
        err = "opendir() failed for '" + path + "': " + std::strerror(errno);
        return false;
    }
    errno = 0;
    while (dirent* entry = ::readdir(dir)) {
        std::string name(entry->d_name);
        if (name != "." && name != "..") names.push_back(std::move(name));
        errno = 0;
    }
    int read_error = errno;
    if (::closedir(dir) != 0 && read_error == 0) read_error = errno;
    if (read_error != 0) {
        err = "readdir() failed for '" + path + "': " + std::strerror(read_error);
        return false;
    }
    return true;
}

inline bool remove_tree(const std::string& path, std::string& err) {
    struct stat st{};
    if (::lstat(path.c_str(), &st) != 0) {
        if (errno == ENOENT) return true;
        err = "lstat() failed for '" + path + "': " + std::strerror(errno);
        return false;
    }
    if (!S_ISDIR(st.st_mode)) {
        if (::unlink(path.c_str()) == 0 || errno == ENOENT) return true;
        err = "unlink() failed for '" + path + "': " + std::strerror(errno);
        return false;
    }

    std::vector<std::string> names;
    if (!list_directory(path, names, err)) return false;
    for (const auto& name : names) {
        if (!remove_tree(join(path, name), err)) return false;
    }
    if (::rmdir(path.c_str()) != 0 && errno != ENOENT) {
        err = "rmdir() failed for '" + path + "': " + std::strerror(errno);
        return false;
    }
    return true;
}

inline bool unlink_if_exists(const std::string& path, std::string& err) {
    if (::unlink(path.c_str()) == 0 || errno == ENOENT) return true;
    err = "unlink() failed for '" + path + "': " + std::strerror(errno);
    return false;
}

} // namespace pisf::posix_fs

#endif // PISF_POSIX_FS_HPP
