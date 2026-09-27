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

} // namespace pisf
