#include "skai/inference/tensor_layout.hpp"

#include <limits>
#include <unordered_set>

namespace skai {

bool validate_tensor_layout(const std::vector<TensorDescription>& descriptions,
                            std::vector<TensorInfo>& tensors, std::string& error) {
    tensors.clear();
    error.clear();
    std::unordered_set<std::string> names;
    bool has_input = false;
    bool has_output = false;
    std::vector<TensorInfo> validated;
    validated.reserve(descriptions.size());

    for (const auto& binding : descriptions) {
        if (binding.name.empty()) {
            error = "engine contains an unnamed I/O tensor";
            return false;
        }
        if (!names.insert(binding.name).second) {
            error = "duplicate I/O tensor: " + binding.name;
            return false;
        }
        if (!binding.device) {
            error = "tensor '" + binding.name + "' is not device-resident";
            return false;
        }
        if (!binding.linear) {
            error = "tensor '" + binding.name + "' does not use a linear format";
            return false;
        }
        if (binding.component_bytes == 0 || binding.data_type.empty()) {
            error = "tensor '" + binding.name + "' has an unsupported data type";
            return false;
        }

        std::size_t bytes = binding.component_bytes;
        for (const auto dimension : binding.shape) {
            if (dimension < 0) {
                error = "tensor '" + binding.name + "' has a dynamic dimension; rebuild with fixed shapes";
                return false;
            }
            if (dimension == 0) {
                error = "tensor '" + binding.name + "' has a zero dimension";
                return false;
            }
            const auto count = static_cast<std::uint64_t>(dimension);
            if (count > std::numeric_limits<std::size_t>::max() / bytes) {
                error = "tensor '" + binding.name + "' byte size overflow";
                return false;
            }
            bytes *= static_cast<std::size_t>(count);
        }
        validated.push_back({binding.name, binding.input, binding.shape,
                             binding.data_type, bytes});
        has_input |= binding.input;
        has_output |= !binding.input;
    }
    if (!has_input || !has_output) {
        error = has_input ? "engine has no output tensor" : "engine has no input tensor";
        return false;
    }
    tensors = std::move(validated);
    return true;
}

} // namespace skai
