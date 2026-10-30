// Row-parallel loops on the CPU compositor's worker pool (internal to fa_render).
#pragma once

#include <functional>

namespace fa {

// Runs fn(row) for every row in [begin, end) on the shared pool (serially when the pool is busy with
// another thread's job, or when called from inside a job).
void parallelRows(int begin, int end, const std::function<void(int)>& fn);

}  // namespace fa
