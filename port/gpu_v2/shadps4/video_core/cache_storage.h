// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/path_util.h"
#include "common/singleton.h"
#include "common/types.h"

#include <functional>
#include <atomic>
#include <thread>
#include <vector>

namespace Storage {

enum class BlobType : u32 {
    ShaderMeta,
    ShaderBinary,
    PipelineKey,
    ShaderProfile,
};

class DataBase {
public:
    static DataBase& Instance() {
        return *Common::Singleton<DataBase>::Instance();
    }

    void Open();
    void Close();
    void SetBeforeClose(std::function<void()> callback) { before_close = std::move(callback); }
    /// Moves the current (incompatible) cache aside and reopens an empty one.
    void Reset();
    [[nodiscard]] bool IsOpened() const {
        return opened;
    }
    void FinishPreload();

    bool Save(BlobType type, const std::string& name, std::vector<u8>&& data);
    bool Save(BlobType type, const std::string& name, std::vector<u32>&& data);

    void Load(BlobType type, const std::string& name, std::vector<u8>& data);
    void Load(BlobType type, const std::string& name, std::vector<u32>& data);

    void ForEachBlob(BlobType type, const std::function<void(std::vector<u8>&& data)>& func);
    /// iss: as ForEachBlob, with each blob's name (the one Save and Remove take).
    void ForEachNamedBlob(BlobType type,
                          const std::function<void(const std::string& name, std::vector<u8>&& data)>& func);
    /// iss: deletes a blob (queued after the writes already accepted). Not in an archived cache.
    void Remove(BlobType type, const std::string& name);

private:
    std::jthread io_worker{};
    std::filesystem::path cache_path{};
    std::atomic<bool> opened{};
    std::function<void()> before_close;
};

} // namespace Storage
