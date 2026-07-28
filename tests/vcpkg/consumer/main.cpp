#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/status.h>
#include <rocksdb/version.h>

namespace {

bool CheckStatus(const rocksdb::Status& status, const char* operation) {
  if (status.ok()) {
    return true;
  }
  std::cerr << operation << " failed: " << status.ToString() << '\n';
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  if (ROCKSDB_MAJOR != 11 || ROCKSDB_MINOR != 1 || ROCKSDB_PATCH != 2) {
    std::cerr << "Unexpected compile-time RocksDB version\n";
    return EXIT_FAILURE;
  }
  if (rocksdb::GetRocksVersionAsString() != "11.1.2") {
    std::cerr << "Unexpected runtime RocksDB version: "
              << rocksdb::GetRocksVersionAsString() << '\n';
    return EXIT_FAILURE;
  }
  if (argc != 2) {
    std::cerr << "Usage: rocksdb_consumer <database-directory>\n";
    return EXIT_FAILURE;
  }

  rocksdb::Options options;
  options.create_if_missing = true;
  std::unique_ptr<rocksdb::DB> database;
  if (!CheckStatus(rocksdb::DB::Open(options, argv[1], &database), "DB::Open")) {
    return EXIT_FAILURE;
  }

  const auto cleanup = [&database, &argv]() {
    database.reset();
    std::error_code cleanup_error;
    std::filesystem::remove_all(argv[1], cleanup_error);
    return cleanup_error;
  };
  const auto finish = [&cleanup](int result) {
    const std::error_code cleanup_error = cleanup();
    if (cleanup_error) {
      std::cerr << "Failed to clean up probe database: " << cleanup_error.message() << '\n';
      return EXIT_FAILURE;
    }
    return result;
  };

  const rocksdb::WriteOptions write_options;
  const rocksdb::ReadOptions read_options;
  if (!CheckStatus(database->Put(write_options, "sitos-key", "sitos-value"), "Put")) {
    return finish(EXIT_FAILURE);
  }

  std::string value;
  const rocksdb::Status get_status = database->Get(read_options, "sitos-key", &value);
  if (!CheckStatus(get_status, "Get") || value != "sitos-value") {
    std::cerr << "Get returned an unexpected value\n";
    return finish(EXIT_FAILURE);
  }
  return finish(EXIT_SUCCESS);
}
