#ifndef PISF_STORAGE_HPP
#define PISF_STORAGE_HPP

#include <cstdint>
#include <mutex>
#include <string>

namespace pisf {

// Manages persistent append-only blob file storage.
class BlobStorage {
public:
    BlobStorage();
    explicit BlobStorage(std::string data_dir, std::string blob_filename = "data.blob");
    ~BlobStorage();

    // Movable but non-copyable to ensure single ownership of file descriptor
    BlobStorage(const BlobStorage&) = delete;
    BlobStorage& operator=(const BlobStorage&) = delete;
    BlobStorage(BlobStorage&& other) noexcept;
    BlobStorage& operator=(BlobStorage&& other) noexcept;

    // Initializes storage: ensures storage directory exists and opens/creates blob file.
    // Returns true on success; sets err_msg and returns false on error.
    bool init(std::string& err_msg);

    // Closes the open file descriptor safely.
    void close();

    // State inspection
    bool is_open() const;
    const std::string& data_dir() const;
    const std::string& blob_path() const;
    uint64_t get_size() const;

private:
    std::string data_dir_;
    std::string blob_filename_;
    std::string blob_path_;
    int fd_{-1};
    mutable std::mutex mutex_;
};

} // namespace pisf

#endif // PISF_STORAGE_HPP
