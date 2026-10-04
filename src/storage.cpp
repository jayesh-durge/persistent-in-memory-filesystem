#include <pisf/storage.hpp>
#include <pisf/common.hpp>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace pisf {

BlobStorage::BlobStorage()
    : BlobStorage(get_default_storage_dir(), "data.blob") {
}

BlobStorage::BlobStorage(std::string data_dir, std::string blob_filename)
    : data_dir_(std::move(data_dir)),
      blob_filename_(std::move(blob_filename)),
      blob_path_((std::filesystem::path(data_dir_) / blob_filename_).string()) {
}

BlobStorage::~BlobStorage() {
    close();
}

BlobStorage::BlobStorage(BlobStorage&& other) noexcept {
    std::lock_guard<std::mutex> lock(other.mutex_);
    data_dir_ = std::move(other.data_dir_);
    blob_filename_ = std::move(other.blob_filename_);
    blob_path_ = std::move(other.blob_path_);
    fd_ = other.fd_;
    simulate_sync_failure_ = other.simulate_sync_failure_;
    other.fd_ = -1;
}

BlobStorage& BlobStorage::operator=(BlobStorage&& other) noexcept {
    if (this != &other) {
        std::scoped_lock lock(mutex_, other.mutex_);
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        data_dir_ = std::move(other.data_dir_);
        blob_filename_ = std::move(other.blob_filename_);
        blob_path_ = std::move(other.blob_path_);
        fd_ = other.fd_;
        simulate_sync_failure_ = other.simulate_sync_failure_;
        other.fd_ = -1;
    }
    return *this;
}

bool BlobStorage::init(std::string& err_msg) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (fd_ >= 0) {
        return true;
    }

    if (data_dir_.empty()) {
        err_msg = "Storage directory path cannot be empty";
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(data_dir_, ec);
    if (ec) {
        err_msg = "Failed to create storage directory '" + data_dir_ + "': " + ec.message();
        return false;
    }

    if (!std::filesystem::is_directory(data_dir_, ec)) {
        err_msg = "Storage path '" + data_dir_ + "' exists but is not a directory";
        return false;
    }

    blob_path_ = (std::filesystem::path(data_dir_) / blob_filename_).string();

    int fd = ::open(blob_path_.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) {
        err_msg = "Failed to open or create blob file '" + blob_path_ + "': " + std::strerror(errno);
        return false;
    }

    fd_ = fd;
    return true;
}

void BlobStorage::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool BlobStorage::is_open() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return fd_ >= 0;
}

const std::string& BlobStorage::data_dir() const {
    return data_dir_;
}

const std::string& BlobStorage::blob_path() const {
    return blob_path_;
}

uint64_t BlobStorage::get_size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (fd_ < 0) {
        return 0;
    }
    struct stat st{};
    if (::fstat(fd_, &st) != 0) {
        return 0;
    }
    return static_cast<uint64_t>(st.st_size);
}

bool BlobStorage::append(const std::string& data, uint64_t& out_offset, uint64_t& out_length, std::string& err_msg) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (fd_ < 0) {
        err_msg = "Storage blob is not open";
        return false;
    }

    struct stat st{};
    if (::fstat(fd_, &st) != 0) {
        err_msg = "fstat() failed: " + std::string(std::strerror(errno));
        return false;
    }

    uint64_t current_size = static_cast<uint64_t>(st.st_size);
    out_offset = current_size;
    out_length = static_cast<uint64_t>(data.size());

    if (data.empty()) {
        return true;
    }

    if (current_size > UINT64_MAX - out_length) {
        err_msg = "Append would overflow maximum file size limit";
        return false;
    }

    off_t write_off = static_cast<off_t>(current_size);
    const char* ptr = data.data();
    size_t remaining = data.size();

    while (remaining > 0) {
        ssize_t written = ::pwrite(fd_, ptr, remaining, write_off);
        if (written > 0) {
            ptr += written;
            remaining -= static_cast<size_t>(written);
            write_off += written;
        } else if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            err_msg = "pwrite() failed on blob: " + std::string(std::strerror(errno));
            return false;
        } else {
            err_msg = "pwrite() returned 0 unexpectedly";
            return false;
        }
    }

    if (simulate_sync_failure_) {
        err_msg = "fdatasync() failed on blob: simulated sync failure (appended bytes may remain orphaned in blob)";
        return false;
    }

    while (true) {
        if (::fdatasync(fd_) == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        err_msg = "fdatasync() failed on blob: " + std::string(std::strerror(errno)) +
                  " (appended bytes may remain orphaned in blob)";
        return false;
    }

    return true;
}

void BlobStorage::set_simulate_sync_failure(bool simulate) {
    std::lock_guard<std::mutex> lock(mutex_);
    simulate_sync_failure_ = simulate;
}

bool BlobStorage::read_range(uint64_t offset, uint64_t length, std::string& out_data, std::string& err_msg) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (fd_ < 0) {
        err_msg = "Storage blob is not open";
        return false;
    }

    if (offset > UINT64_MAX - length) {
        err_msg = "ERR invalid range: offset + length overflows";
        return false;
    }

    struct stat st{};
    if (::fstat(fd_, &st) != 0) {
        err_msg = "fstat() failed: " + std::string(std::strerror(errno));
        return false;
    }

    uint64_t total_size = static_cast<uint64_t>(st.st_size);
    if (offset > total_size || offset + length > total_size) {
        err_msg = "ERR invalid range: offset " + std::to_string(offset) + " + length " +
                  std::to_string(length) + " exceeds blob size " + std::to_string(total_size);
        return false;
    }

    if (length == 0) {
        out_data.clear();
        return true;
    }

    out_data.resize(length);
    char* ptr = out_data.data();
    size_t remaining = length;
    off_t read_off = static_cast<off_t>(offset);

    while (remaining > 0) {
        ssize_t n_read = ::pread(fd_, ptr, remaining, read_off);
        if (n_read > 0) {
            ptr += n_read;
            remaining -= static_cast<size_t>(n_read);
            read_off += n_read;
        } else if (n_read == 0) {
            err_msg = "pread() reached unexpected EOF";
            return false;
        } else {
            if (errno == EINTR) {
                continue;
            }
            err_msg = "pread() failed on blob: " + std::string(std::strerror(errno));
            return false;
        }
    }

    return true;
}

} // namespace pisf
