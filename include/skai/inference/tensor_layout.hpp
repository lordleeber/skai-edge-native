#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace skai {

// A description read from an engine. Only fixed, linear, device tensors are
// accepted by the PR 8 loader; dynamic/profile and host-shape tensors need
// separate address and sizing rules in a later stage.
struct TensorDescription {
    std::string name;
    bool input = false;
    std::vector<std::int64_t> shape;
    std::string data_type;
    std::size_t component_bytes = 0;
    bool device = true;
    bool linear = true;
};

struct TensorInfo {
    std::string name;
    bool input = false;
    std::vector<std::int64_t> shape;
    std::string data_type;
    std::size_t bytes = 0;
};

bool validate_tensor_layout(const std::vector<TensorDescription>& descriptions,
                            std::vector<TensorInfo>& tensors, std::string& error);

} // namespace skai
