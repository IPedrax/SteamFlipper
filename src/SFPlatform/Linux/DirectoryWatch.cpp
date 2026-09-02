#include "include/DirectoryWatch.h"

#include "include/Log.h"

#include <poll.h>
#include <sys/inotify.h>
#include <unistd.h>

#include <cstring>
#include <vector>

namespace SFPlatform::DirectoryWatch {
namespace {

ChangeAction FromInotifyMask(uint32_t mask) {
    if (mask & IN_CREATE) return ChangeAction::Added;
    if (mask & IN_DELETE) return ChangeAction::Removed;
    if (mask & IN_MOVED_FROM) return ChangeAction::RenamedOldName;
    if (mask & IN_MOVED_TO) return ChangeAction::RenamedNewName;
    return ChangeAction::Modified;
}

} // namespace

struct Watch::Impl {
    std::string directory;
    int inotifyFd = -1;
    int watchDescriptor = -1;
    std::vector<uint8_t> buffer;

    void Close() {
        if (inotifyFd >= 0) {
            if (watchDescriptor >= 0) {
                inotify_rm_watch(inotifyFd, watchDescriptor);
                watchDescriptor = -1;
            }
            close(inotifyFd);
            inotifyFd = -1;
        }
        buffer.clear();
        directory.clear();
    }
};

Watch::Watch() : impl_(std::make_unique<Impl>()) {}
Watch::~Watch() = default;
Watch::Watch(Watch&&) noexcept = default;
Watch& Watch::operator=(Watch&&) noexcept = default;

bool Watch::Open(const std::string& directory, uint32_t bufferSize) {
    impl_->Close();

    impl_->inotifyFd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (impl_->inotifyFd < 0) {
        SFP_LOG_WARN("DirectoryWatch: inotify_init1 failed for '{}' (errno={})", directory, errno);
        return false;
    }

    impl_->watchDescriptor = inotify_add_watch(
        impl_->inotifyFd,
        directory.c_str(),
        IN_CREATE | IN_DELETE | IN_MODIFY | IN_MOVED_FROM | IN_MOVED_TO | IN_CLOSE_WRITE);

    if (impl_->watchDescriptor < 0) {
        SFP_LOG_WARN("DirectoryWatch: inotify_add_watch failed for '{}' (errno={})", directory, errno);
        impl_->Close();
        return false;
    }

    impl_->directory = directory;
    impl_->buffer.resize(bufferSize > 0 ? bufferSize : 4096);
    return true;
}

bool Watch::IssueRead() {
    return IsOpen();
}

std::vector<Change> Watch::Drain() {
    std::vector<Change> changes;
    if (!IsOpen()) return changes;

    ssize_t len = read(impl_->inotifyFd, impl_->buffer.data(), impl_->buffer.size());
    if (len <= 0) return changes;

    ssize_t i = 0;
    while (i < len) {
        auto* event = reinterpret_cast<struct inotify_event*>(&impl_->buffer[i]);
        if (event->len > 0 && event->name[0] != '\0') {
            changes.push_back({
                std::string(event->name),
                FromInotifyMask(event->mask)
            });
        }
        i += sizeof(struct inotify_event) + event->len;
    }

    return changes;
}

void Watch::Cancel() {
    impl_->Close();
}

bool Watch::IsOpen() const {
    return impl_ && impl_->inotifyFd >= 0;
}

const std::string& Watch::Directory() const {
    return impl_->directory;
}

WaitResult WaitAny(std::span<Watch*> watches, uint32_t timeoutMs) {
    std::vector<struct pollfd> pfds;
    std::vector<size_t> originalIndexes;
    pfds.reserve(watches.size());
    originalIndexes.reserve(watches.size());

    for (size_t i = 0; i < watches.size(); ++i) {
        Watch* watch = watches[i];
        if (!watch || !watch->IsOpen()) continue;
        struct pollfd pfd{};
        pfd.fd = watch->impl_->inotifyFd;
        pfd.events = POLLIN;
        pfds.push_back(pfd);
        originalIndexes.push_back(i);
    }

    if (pfds.empty()) {
        return {WaitStatus::Failed, 0};
    }

    int ret = poll(pfds.data(), pfds.size(), static_cast<int>(timeoutMs));
    if (ret == 0) return {WaitStatus::Timeout, 0};
    if (ret < 0) {
        if (errno == EINTR) return {WaitStatus::Timeout, 0};
        SFP_LOG_WARN("DirectoryWatch: poll failed (errno={})", errno);
        return {WaitStatus::Failed, 0};
    }

    for (size_t i = 0; i < pfds.size(); ++i) {
        if (pfds[i].revents & (POLLIN | POLLERR | POLLHUP)) {
            return {WaitStatus::Signaled, originalIndexes[i]};
        }
    }

    return {WaitStatus::Failed, 0};
}

} // namespace SFPlatform::DirectoryWatch
