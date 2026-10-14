/* SPDX-FileCopyrightText: 2026 fubukisama
 * SPDX-License-Identifier: MIT */
#include <torch/extension.h>
#include <vector>

torch::Tensor fused_loss_forward(torch::Tensor, torch::Tensor, double,
                                 std::vector<torch::Tensor>);
torch::Tensor fused_loss_backward(torch::Tensor, torch::Tensor, double,
                                  torch::Tensor, std::vector<torch::Tensor>);

PYBIND11_MODULE(TORCH_EXTENSION_NAME, module) {
  module.attr("SOURCE_COMMIT") = "1272e21a282342e89537159e4bad508b19b34157";
  module.attr("LICENSE_ID") = "MIT";
  module.attr("KERNEL_ABI") = 2;
  module.attr("KERNEL_VERSION") = "0.2.2";
  module.def("forward", &fused_loss_forward);
  module.def("backward", &fused_loss_backward);
}
