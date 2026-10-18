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
    BlobStorage(std::string data_dir, uint64_t generation);
    ~BlobStorage();

    // Movable but non-copyable to ensure single ownership of file descriptor
    BlobStorage(const BlobStorage&) = delete;
    BlobStorage& operator=(const BlobStorage&) = delete;
    BlobStorage(BlobStorage&& other) noexcept;
    BlobStorage& operator=(BlobStorage&& other) noexcept;

    // Helper to format standard generation-based blob filename
    static std::string format_blob_filename(uint64_t generation);

    // Initializes storage: ensures storage directory exists and opens/creates blob file.
    // Returns true on success; sets err_msg and returns false on error.
    bool init(std::string& err_msg);

    // Closes the open file descriptor safely.
    void close();

    // Operations
    // Appends bytes to the blob file, returning the byte offset and length.
    bool append(const std::string& data, uint64_t& out_offset, uint64_t& out_length, std::string& err_msg);

    // Reads an exact byte range [offset, offset + length) from the blob file.
    bool read_range(uint64_t offset, uint64_t length, std::string& out_data, std::string& err_msg) const;

    // State inspection
    bool is_open() const;
    const std::string& data_dir() const;
    const std::string& blob_path() const;
    const std::string& blob_filename() const;
    uint64_t get_size() const;
    uint64_t generation() const;

    // Test helper to simulate sync failure
    void set_simulate_sync_failure(bool simulate);

private:
    std::string data_dir_;
    std::string blob_filename_;
    std::string blob_path_;
    uint64_t generation_{0};
    int fd_{-1};
    bool simulate_sync_failure_{false};
    mutable std::mutex mutex_;
};

} // namespace pisf

#endif // PISF_STORAGE_HPP
