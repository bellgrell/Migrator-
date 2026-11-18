#pragma once

#include <string>

#ifdef _WIN32
#include <windows.h>
#include <process.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace mig {

inline long mig_pid() {
#ifdef _WIN32
  return (long)_getpid();
#else
  return (long)getpid();
#endif
}

// 单实例锁：POSIX 用 flock 文件锁；Windows 用同名互斥体。
// 只依赖头文件，避免给两个构建系统各加一个源文件。
class InstanceLock {
 public:
  bool acquire(const std::string& path, std::string& err) {
    if (path.empty()) return true;
#ifdef _WIN32
    const std::string name =
        "migrator-" + std::to_string(std::hash<std::string>{}(path));
    handle_ = (void*)CreateMutexA(nullptr, TRUE, name.c_str());
    if (!handle_) {
      err = "CreateMutex failed";
      return false;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
      CloseHandle((HANDLE)handle_);
      handle_ = nullptr;
      err = "another migrator instance already holds " + path;
      return false;
    }
    return true;
#else
    fd_ = ::open(path.c_str(), O_CREAT | O_RDWR, 0600);
    if (fd_ < 0) {
      err = "cannot open instance lock " + path;
      return false;
    }
    if (::flock(fd_, LOCK_EX | LOCK_NB) != 0) {
      ::close(fd_);
      fd_ = -1;
      err = "another migrator instance already holds " + path;
      return false;
    }
    if (::ftruncate(fd_, 0) == 0) {
      const std::string pid = std::to_string(mig_pid()) + "\n";
      (void)!::write(fd_, pid.data(), pid.size());
    }
    return true;
#endif
  }

  void release() {
#ifdef _WIN32
    if (handle_) {
      ReleaseMutex((HANDLE)handle_);
      CloseHandle((HANDLE)handle_);
      handle_ = nullptr;
    }
#else
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
#endif
  }

  int fd() const {
#ifdef _WIN32
    return -1;
#else
    return fd_;
#endif
  }

  ~InstanceLock() { release(); }

 private:
#ifdef _WIN32
  void* handle_ = nullptr;
#else
  int fd_ = -1;
#endif
};

}  // namespace mig
