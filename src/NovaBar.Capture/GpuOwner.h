#pragma once
#include "GpuTransfer.h"
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")

namespace novabar::capture::gpu
{
    inline std::wstring OwnerPath(std::wstring const& root)
    { return (std::filesystem::path(root) / L"gpu-owner.bin").wstring(); }
    inline uint64_t RandomNonce() noexcept {
        uint64_t nonce = 0;
        if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&nonce), sizeof(nonce), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return 0;
        return nonce;
    }
    inline bool TryReadFreshGpuOwner(std::wstring const& root, uint64_t now, GpuOwnerRecord& out) {
        out = {};
        GpuOwnerRecord owner{};
        if (!detail::ReadSmall(OwnerPath(root), &owner, sizeof(owner)) || !ValidOwner(owner, now)) return false;
        out = owner; return true;
    }
    namespace owner_detail {
        // Every writer uses the same existing-directory file lock for comparison
        // and atomic replacement. No named kernel synchronization object.
        class WriterLock {
            HANDLE m_file = INVALID_HANDLE_VALUE;
            OVERLAPPED m_overlapped{};
        public:
            explicit WriterLock(std::wstring const& root) {
                const auto path = (std::filesystem::path(root) / L"gpu-owner.lock").wstring();
                m_file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (m_file != INVALID_HANDLE_VALUE && !LockFileEx(m_file, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                    0, 1, 0, &m_overlapped)) { CloseHandle(m_file); m_file = INVALID_HANDLE_VALUE; }
            }
            WriterLock(WriterLock const&) = delete;
            WriterLock& operator=(WriterLock const&) = delete;
            ~WriterLock() {
                if (m_file != INVALID_HANDLE_VALUE) { UnlockFileEx(m_file, 0, 1, 0, &m_overlapped); CloseHandle(m_file); }
            }
            explicit operator bool() const noexcept { return m_file != INVALID_HANDLE_VALUE; }
        };
        inline bool ReadOpenedOwner(HANDLE file, GpuOwnerRecord& out) noexcept {
            LARGE_INTEGER length{}; DWORD read = 0;
            return GetFileSizeEx(file, &length) && length.QuadPart == sizeof(out)
                && ReadFile(file, &out, sizeof(out), &read, nullptr) && read == sizeof(out);
        }
        // Compare and disposition refer to the same file object, even if its path
        // is replaced. A stale writer cannot delete the replacement file.
        inline bool DeleteOpenedOwner(HANDLE file, GpuOwnerRecord const& expected) noexcept {
            LARGE_INTEGER begin{};
            GpuOwnerRecord current{};
            if (!SetFilePointerEx(file, begin, nullptr, FILE_BEGIN) || !ReadOpenedOwner(file, current)
                || current.Magic != OwnerMagic || current.Version != GpuProtocolVersion
                || current.Bytes != sizeof(current) || !SameOwner(current, expected)) return false;
            FILE_DISPOSITION_INFO disposition{TRUE};
            return !!SetFileInformationByHandle(file, FileDispositionInfo, &disposition, sizeof(disposition));
        }
    }
    class GpuOwnerWriter {
        std::wstring m_root;
        GpuOwnerRecord m_record{};
        bool m_published = false;
    public:
        GpuOwnerWriter() = default;
        GpuOwnerWriter(GpuOwnerWriter const&) = delete;
        GpuOwnerWriter& operator=(GpuOwnerWriter const&) = delete;
        ~GpuOwnerWriter() { Withdraw(); }
        bool Publish(std::wstring const& root, ProcessIdentity self, uint64_t nonce, LUID adapter) {
            if (m_published || !SameIdentity(self, CurrentIdentity()) || !nonce || (!adapter.LowPart && !adapter.HighPart))
            { SetLastError(ERROR_INVALID_PARAMETER); return false; }
            owner_detail::WriterLock lock(root);
            if (!lock) return false;
            GpuOwnerRecord previous{};
            DWORD error = ERROR_SUCCESS;
            const auto path = OwnerPath(root);
            const bool exists = detail::ReadSmall(path, &previous, sizeof(previous), &error);
            // Freshness alone is not proof of death. A different live/unqueryable
            // exact process retains ownership even after its heartbeat expires.
            if (exists && previous.Magic == OwnerMagic && previous.Version == GpuProtocolVersion
                && previous.Bytes == sizeof(previous) && ValidIdentity({previous.Pid, 0, previous.CreationFileTime})
                && ProbeExactProcess({previous.Pid, 0, previous.CreationFileTime}) != ExactProcessState::Dead)
            { SetLastError(ERROR_BUSY); return false; }
            if (!exists && error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
            { SetLastError(error); return false; }
            GpuOwnerRecord record{};
            record.Pid = self.Pid; record.CreationFileTime = self.CreationFileTime; record.Nonce = nonce;
            record.AdapterLow = adapter.LowPart; record.AdapterHigh = adapter.HighPart; record.HeartbeatTick = GetTickCount64();
            if (!ValidOwner(record, record.HeartbeatTick) || !detail::WriteSmall(path, &record, sizeof(record))) return false;
            m_root = root; m_record = record; m_published = true; return true;
        }
        bool Refresh(uint64_t now) {
            if (!m_published || !now) { SetLastError(ERROR_INVALID_HANDLE); return false; }
            owner_detail::WriterLock lock(m_root);
            if (!lock) return false;
            GpuOwnerRecord current{};
            if (!detail::ReadSmall(OwnerPath(m_root), &current, sizeof(current)) || !SameOwner(current, m_record)
                || current.Magic != OwnerMagic || current.Version != GpuProtocolVersion || current.Bytes != sizeof(current))
            { SetLastError(ERROR_INVALID_DATA); return false; }
            auto next = m_record; next.HeartbeatTick = now;
            if (!detail::WriteSmall(OwnerPath(m_root), &next, sizeof(next))) return false;
            m_record = next; return true;
        }
        void Withdraw() {
            if (!m_published) return;
            owner_detail::WriterLock lock(m_root);
            if (!lock) return; // Retain identity so a later attempt can withdraw.
            HANDLE file = CreateFileW(OwnerPath(m_root).c_str(), DELETE | GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE) { owner_detail::DeleteOpenedOwner(file, m_record); CloseHandle(file); }
            m_published = false; m_root.clear(); m_record = {};
        }
        GpuOwnerRecord const& Record() const noexcept { return m_record; }
    };
}
