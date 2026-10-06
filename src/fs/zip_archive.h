#pragma once

#include "fs/file_system.h"

#include <memory>

class ZipArchive final : public Archive {
public:
    ZipArchive();
    ~ZipArchive() override;

    bool open(const char* path) override;
    bool open(const char* path, const char* rootPrefix);
    bool openMember(ZipArchive& parent, const char* memberPath);
    bool readFile(const char* path, std::vector<uint8_t>& data) override;
    bool fileExists(const char* path) const override;
    void listFiles(const char* pattern, std::vector<std::string>& out) const override;

private:
    struct Impl;
    std::shared_ptr<Impl> impl;
};
