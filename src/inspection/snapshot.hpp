#pragma once

namespace nova::inspection
{
// Linux-only boundary. Errors never produce a final report.json.
[[nodiscard]] int RunSnapshotInspection(int argc, char** argv) noexcept;
} // namespace nova::inspection
