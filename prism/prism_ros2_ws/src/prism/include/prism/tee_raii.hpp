// RAII wrappers to handle automatic resource managment of objects
// implemented in <tee_client_api.h>, which was written in C.
#ifndef PRISM_TEE_RAII_HPP_
#define PRISM_TEE_RAII_HPP_

#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>

#include <tee_client_api.h>

namespace tee {

inline std::string to_hex(uint32_t v) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%x", v);
  return buf;
}

class TEEContext {
public:
  TEEContext() {
    TEEC_Result res = TEEC_InitializeContext(nullptr, &ctx_);
    if (res != TEEC_SUCCESS) {
      throw std::runtime_error(
        "TEEC_InitializeContext failed with code 0x" + to_hex(res));
    }
  }
  ~TEEContext() { TEEC_FinalizeContext(&ctx_); }

  // Disable copy and assignment operators
  TEEContext(const TEEContext&) = delete;
  TEEContext& operator=(const TEEContext&) = delete;

  TEEC_Context& get() { return ctx_; }

private:
  TEEC_Context ctx_{};
};

class TEESession {
public:
  TEESession(TEEContext& ctx, const TEEC_UUID& uuid) {
    uint32_t err_origin = 0;
    TEEC_Result res = TEEC_OpenSession(
      &ctx.get(), &sess_, &uuid, TEEC_LOGIN_PUBLIC, nullptr, nullptr, 
      &err_origin);
    if (res != TEEC_SUCCESS) {
      throw std::runtime_error(
        "TEEC_OpenSession failed with code 0x" + to_hex(res) + 
        " origin 0x" + to_hex(err_origin));
    }
  }
  ~TEESession() { TEEC_CloseSession(&sess_); }

  TEESession(const TEESession&) = delete;
  TEESession& operator=(const TEESession&) = delete;

  TEEC_Session& get() { return sess_; }

private:
  TEEC_Session sess_{};
};

class TEESharedMemory {
public:
  TEESharedMemory(TEEContext& ctx, size_t size, uint32_t flags) {
    shm_.size = size;
    shm_.flags = flags;
    TEEC_Result res = TEEC_AllocateSharedMemory(&ctx.get(), &shm_);
    if (res != TEEC_SUCCESS) {
      throw std::runtime_error(
        "TEEC_AllocateSharedMemory failed with code 0x" + to_hex(res));
    }
  }
  ~TEESharedMemory() { TEEC_ReleaseSharedMemory(&shm_); }

  TEESharedMemory(const TEESharedMemory&) = delete;
  TEESharedMemory& operator=(const TEESharedMemory&) = delete;

  TEEC_SharedMemory& get() { return shm_; }
  void* buffer() { return shm_.buffer; }
  size_t size() const { return shm_.size; }

private:
  TEEC_SharedMemory shm_{};
};

} // namespace tee

#endif