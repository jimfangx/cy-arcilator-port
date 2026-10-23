#pragma once
namespace chipyard_arc::vpi_lifecycle {
// No registry allocation or per-step scheduler work when VPI is unused.
extern bool active;
void attach(void *owner);
void startup();
bool start(void *owner) noexcept;
bool end(void *owner) noexcept;
bool finish(void *owner, void (*finalize)(void *), void *state) noexcept;
bool is_finalized(void *owner) noexcept;
void mark_finalized(void *owner) noexcept;
void detach(void *owner) noexcept;
} // namespace chipyard_arc::vpi_lifecycle
