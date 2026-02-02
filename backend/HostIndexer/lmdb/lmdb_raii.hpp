//
// Created by Dell on 2.02.2026.
//

// lmdb_raii.hpp
#pragma once
#include <lmdb.h>

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

namespace lmdb {

inline void throw_on(int rc, const char* what) {
  if (rc != MDB_SUCCESS) {
    throw std::runtime_error(std::string(what) + ": " + mdb_strerror(rc));
  }
}

class Env {
public:
  Env() {
    MDB_env* e = nullptr;
    throw_on(mdb_env_create(&e), "mdb_env_create");
    env_ = e;
  }

  // Move-only
  Env(Env&& other) noexcept : env_(std::exchange(other.env_, nullptr)) {}
  Env& operator=(Env&& other) noexcept {
    if (this != &other) {
      close();
      env_ = std::exchange(other.env_, nullptr);
    }
    return *this;
  }

  Env(const Env&) = delete;
  Env& operator=(const Env&) = delete;

  ~Env() { close(); }

  // Configuration (call before open)
  void set_mapsize(std::size_t bytes) { throw_on(mdb_env_set_mapsize(env_, bytes), "mdb_env_set_mapsize"); }
  void set_maxdbs(unsigned dbs)        { throw_on(mdb_env_set_maxdbs(env_, dbs), "mdb_env_set_maxdbs"); }
  void set_maxreaders(unsigned r)      { throw_on(mdb_env_set_maxreaders(env_, r), "mdb_env_set_maxreaders"); }

  void open(const char* path, unsigned flags = 0, mdb_mode_t mode = 0664) {
    throw_on(mdb_env_open(env_, path, flags, mode), "mdb_env_open");
    is_open_ = true;
  }

  MDB_env* raw() const noexcept { return env_; }
  bool is_open() const noexcept { return is_open_; }

private:
  void close() noexcept {
    if (env_) {
      // Safe even if not opened; LMDB docs allow closing env created but not opened.
      mdb_env_close(env_);
      env_ = nullptr;
      is_open_ = false;
    }
  }

  MDB_env* env_ = nullptr;
  bool is_open_ = false;
};

class Txn {
public:
  enum class Mode { ReadOnly, ReadWrite };

  Txn(Env& env, Mode mode, Txn* parent = nullptr)
      : env_(&env), mode_(mode) {
    unsigned flags = (mode == Mode::ReadOnly) ? MDB_RDONLY : 0;
    throw_on(mdb_txn_begin(env.raw(), parent ? parent->txn_ : nullptr, flags, &txn_), "mdb_txn_begin");
    active_ = true;
  }

  // Move-only
  Txn(Txn&& other) noexcept
      : env_(other.env_), txn_(std::exchange(other.txn_, nullptr)),
        mode_(other.mode_), active_(std::exchange(other.active_, false)) {}

  Txn& operator=(Txn&& other) noexcept {
    if (this != &other) {
      abort(); // abort if still active
      env_ = other.env_;
      txn_ = std::exchange(other.txn_, nullptr);
      mode_ = other.mode_;
      active_ = std::exchange(other.active_, false);
    }
    return *this;
  }

  Txn(const Txn&) = delete;
  Txn& operator=(const Txn&) = delete;

  ~Txn() { abort(); }

  void commit() {
    if (!active_) return;
    int rc = mdb_txn_commit(txn_);
    txn_ = nullptr;
    active_ = false;
    throw_on(rc, "mdb_txn_commit");
  }

  void abort() noexcept {
    if (active_ && txn_) {
      mdb_txn_abort(txn_);
      txn_ = nullptr;
      active_ = false;
    }
  }

  MDB_txn* raw() const noexcept { return txn_; }
  Mode mode() const noexcept { return mode_; }

private:
  Env* env_ = nullptr;
  MDB_txn* txn_ = nullptr;
  Mode mode_;
  bool active_ = false;
};

class Dbi {
public:
  Dbi() = default;

  // Open DBI within a transaction (LMDB rule)
  static Dbi open(Txn& txn, const char* name = nullptr, unsigned flags = 0) {
    MDB_dbi dbi{};
    throw_on(mdb_dbi_open(txn.raw(), name, flags, &dbi), "mdb_dbi_open");
    return Dbi{dbi};
  }

  MDB_dbi raw() const noexcept { return dbi_; }
  explicit operator bool() const noexcept { return dbi_ != 0; }

private:
  explicit Dbi(MDB_dbi dbi) : dbi_(dbi) {}
  MDB_dbi dbi_ = 0;
};

class Cursor {
public:
  Cursor(Txn& txn, Dbi dbi)
      : txn_(&txn) {
    throw_on(mdb_cursor_open(txn.raw(), dbi.raw(), &cur_), "mdb_cursor_open");
  }

  // Move-only
  Cursor(Cursor&& other) noexcept
      : txn_(other.txn_), cur_(std::exchange(other.cur_, nullptr)) {}

  Cursor& operator=(Cursor&& other) noexcept {
    if (this != &other) {
      close();
      txn_ = other.txn_;
      cur_ = std::exchange(other.cur_, nullptr);
    }
    return *this;
  }

  Cursor(const Cursor&) = delete;
  Cursor& operator=(const Cursor&) = delete;

  ~Cursor() { close(); }

  MDB_cursor* raw() const noexcept { return cur_; }

private:
  void close() noexcept {
    if (cur_) {
      mdb_cursor_close(cur_);
      cur_ = nullptr;
    }
  }

  Txn* txn_ = nullptr;
  MDB_cursor* cur_ = nullptr;
};

} // namespace lmdb
