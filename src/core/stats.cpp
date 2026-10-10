#include "core/stats.hpp"

#include <format>

#include "chaos/rules.hpp"

namespace chaos {

std::string Stats::line() const {
    return std::format(
        "conns {} active / {} total ({} upstream failures) | up {} down {} | chunks {} delayed {} "
        "stalled {} blackholed {} | resets {}",
        get(conns_active), get(conns_total), get(connect_failures), format_bytes(get(bytes_up)),
        format_bytes(get(bytes_down)), get(chunks), get(chunks_delayed), get(chunks_stalled),
        get(chunks_blackholed), get(resets_injected));
}

}  // namespace chaos
