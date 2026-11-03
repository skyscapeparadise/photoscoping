#pragma once

#include <algorithm>
#include <cstddef>
#include <thread>
#include <type_traits>
#include <vector>

#if defined(__APPLE__)
#include <dispatch/dispatch.h>
#endif

namespace ps::matting {

// Runs body(begin, end) over [0, count) split into chunks, on every core.
// Chunks are independent: the body must only write what its own range owns.
template <class Body>
void parallel_for(std::size_t count, Body&& body, std::size_t min_chunk = 16) {
    if (count == 0) return;
    const std::size_t cores = std::max(1u, std::thread::hardware_concurrency());
    const std::size_t chunks = std::clamp<std::size_t>(count / std::max<std::size_t>(min_chunk, 1), 1, cores * 4);
    const std::size_t per = (count + chunks - 1) / chunks;
    if (chunks == 1) {
        body(std::size_t{0}, count);
        return;
    }
#if defined(__APPLE__)
    struct Context {
        std::remove_reference_t<Body>* body;
        std::size_t count, per;
    } context{&body, count, per};
    dispatch_apply_f(chunks, DISPATCH_APPLY_AUTO, &context, [](void* data, std::size_t k) {
        const Context& c = *static_cast<const Context*>(data);
        const std::size_t begin = k * c.per, end = std::min(c.count, begin + c.per);
        if (begin < end) (*c.body)(begin, end);
    });
#else
    std::vector<std::thread> threads;
    threads.reserve(chunks);
    for (std::size_t k = 0; k < chunks; ++k) {
        const std::size_t begin = k * per, end = std::min(count, begin + per);
        if (begin < end) threads.emplace_back([&body, begin, end] { body(begin, end); });
    }
    for (std::thread& t : threads) t.join();
#endif
}

}  // namespace ps::matting
