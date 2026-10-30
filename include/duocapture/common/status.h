#pragma once

namespace duo {

enum class Status {
  Ok = 0,
  InvalidArgument,
  NotSupported,
  NotInitialized,
  IoError,
  Timeout,
  BufferFull,
  BufferEmpty,
  Closed,
  WouldBlock,
};

inline const char* to_string(Status s) noexcept {
  switch (s) {
    case Status::Ok: return "Ok";
    case Status::InvalidArgument: return "InvalidArgument";
    case Status::NotSupported: return "NotSupported";
    case Status::NotInitialized: return "NotInitialized";
    case Status::IoError: return "IoError";
    case Status::Timeout: return "Timeout";
    case Status::BufferFull: return "BufferFull";
    case Status::BufferEmpty: return "BufferEmpty";
    case Status::Closed: return "Closed";
    case Status::WouldBlock: return "WouldBlock";
  }
  return "Unknown";
}

inline bool ok(Status s) noexcept { return s == Status::Ok; }

}  // namespace duo
