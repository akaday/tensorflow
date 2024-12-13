/* Copyright 2024 The OpenXLA Authors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "xla/stream_executor/cuda/nvjitlink.h"

#include <sys/types.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include "absl/status/status.h"
#include "absl/strings/str_replace.h"
#include "absl/types/span.h"
#include "xla/stream_executor/cuda/nvjitlink_support.h"
#include "xla/stream_executor/device_description.h"
#include "xla/stream_executor/gpu/gpu_asm_opts.h"
#include "tsl/platform/status_matchers.h"
#include "tsl/platform/test.h"

namespace {

// Generated by the following command:
//
// echo "__device__ int magic() { return 42; }" |
//   nvcc -o - -rdc true --ptx --x cu -
//
constexpr const char kDependeePtx[] = R"(
.version 8.0
.target sm_52
.address_size 64

        // .globl       _Z5magicv

.visible .func  (.param .b32 func_retval0) _Z5magicv()
{
        .reg .b32       %r<2>;

        mov.u32         %r1, 42;
        st.param.b32    [func_retval0+0], %r1;
        ret;
})";

// Generated by the following command:
//
// echo "__device__ int magic(); __global__ void kernel(int* output) \
//   { *output = magic(); }" | nvcc -o - -rdc true --ptx --x cu -
//
constexpr const char kDependentPtx[] = R"(
.version 8.0
.target sm_52
.address_size 64

        // .globl       _Z6kernelPi
.extern .func  (.param .b32 func_retval0) _Z5magicv
()
;

.visible .entry _Z6kernelPi(
        .param .u64 _Z6kernelPi_param_0
)
// Insert .maxnreg directive here!
{
        .reg .b32       %r<2>;
        .reg .b64       %rd<3>;

        ld.param.u64    %rd1, [_Z6kernelPi_param_0];
        cvta.to.global.u64      %rd2, %rd1;
        { // callseq 0, 0
        .reg .b32 temp_param_reg;
        .param .b32 retval0;
        call.uni (retval0), 
        _Z5magicv, 
        (
        );
        ld.param.b32    %r1, [retval0+0];
        } // callseq 0
        st.global.u32   [%rd2], %r1;
        ret;
})";

// Generated by the following command:
//
// echo "__global__ void kernel(int* output) { *output = 42; }" |
//   nvcc -o - -rdc true --ptx --x cu -
//
constexpr const char kStandalonePtx[] = R"(
.version 8.0
.target sm_52
.address_size 64

        // .globl       _Z6kernelPi

.visible .entry _Z6kernelPi (
        .param .u64 _Z6kernelPi_param_0
)
{
        .reg .b32       %r<16>;
        .reg .b64       %rd<3>;


        ld.param.u64    %rd1, [_Z6kernelPi_param_0];
        cvta.to.global.u64      %rd2, %rd1;
        mov.u32         %r1, 42;
        st.global.u32   [%rd2], %r15;
        ret;

})";

constexpr stream_executor::CudaComputeCapability kDefaultComputeCapability{5,
                                                                           2};

// Just a helper function that wraps `CompileAndLinkUsingLibNvJitLink`. It makes
// it easier to deal with C string PTX literals.
auto CompileAndLinkHelper(stream_executor::CudaComputeCapability cc,
                          absl::Span<const char* const> ptx_inputs,
                          bool disable_gpuasm_optimizations = false,
                          bool cancel_if_reg_spill = false) {
  std::vector<stream_executor::NvJitLinkInput> inputs;
  inputs.reserve(ptx_inputs.size());
  for (const char* ptx_input : ptx_inputs) {
    inputs.emplace_back(stream_executor::NvJitLinkInput{
        stream_executor::NvJitLinkInput::Type::kPtx,
        absl::Span<const uint8_t>{reinterpret_cast<const uint8_t*>(ptx_input),
                                  std::strlen(ptx_input) + 1}});
  }

  stream_executor::GpuAsmOpts options{};
  options.disable_gpuasm_optimizations = disable_gpuasm_optimizations;

  return stream_executor::CompileAndLinkUsingLibNvJitLink(cc, inputs, options,
                                                          cancel_if_reg_spill);
}

class NvJitLinkTest : public ::testing::Test {
  void SetUp() override {
    if (!stream_executor::IsLibNvJitLinkSupported()) {
      GTEST_SKIP();
    }
  }
};

TEST_F(NvJitLinkTest, GetVersion) {
  EXPECT_THAT(stream_executor::GetNvJitLinkVersion(),
              tsl::testing::IsOkAndHolds(
                  testing::Ge(stream_executor::NvJitLinkVersion{12, 0})));
}

TEST_F(NvJitLinkTest, IdentifiesUnsupportedArchitecture) {
  EXPECT_THAT(
      CompileAndLinkHelper(stream_executor::CudaComputeCapability{100, 0},
                           {kStandalonePtx}),
      tsl::testing::StatusIs(absl::StatusCode::kUnimplemented));
}

TEST_F(NvJitLinkTest, LinkingTwoCompilationUnitsSucceeds) {
  EXPECT_THAT(CompileAndLinkHelper(kDefaultComputeCapability,
                                   {kDependentPtx, kDependeePtx}),
              tsl::testing::IsOk());
}

TEST_F(NvJitLinkTest, LinkingFailsWhenDependeeIsMissing) {
  EXPECT_THAT(CompileAndLinkHelper(kDefaultComputeCapability, {kDependentPtx}),
              tsl::testing::StatusIs(absl::StatusCode::kUnknown));
}

TEST_F(NvJitLinkTest, CanAlsoJustCompileSingleCompilationUnit) {
  EXPECT_THAT(CompileAndLinkHelper(kDefaultComputeCapability, {kStandalonePtx}),
              tsl::testing::IsOk());
}

TEST_F(NvJitLinkTest, CancelsOnRegSpill) {
  std::string dependent_ptx = absl::StrReplaceAll(
      kDependentPtx, {{"// Insert .maxnreg directive here!", ".maxnreg 16"}});

  // We have to disable optimization here, otherwise PTXAS will optimize our
  // trivial register usages away and we don't spill as intended.
  EXPECT_THAT(CompileAndLinkHelper(kDefaultComputeCapability,
                                   {dependent_ptx.c_str(), kDependeePtx},
                                   /*disable_gpuasm_optimizations=*/true,
                                   /*cancel_if_reg_spill=*/true),
              tsl::testing::StatusIs(absl::StatusCode::kCancelled));

  // We also test the converse to ensure our test case isn't broken.
  EXPECT_THAT(CompileAndLinkHelper(kDefaultComputeCapability,
                                   {dependent_ptx.c_str(), kDependeePtx},
                                   /*disable_gpuasm_optimizations=*/true,
                                   /*cancel_if_reg_spill=*/false),
              tsl::testing::IsOk());
}

}  // namespace
