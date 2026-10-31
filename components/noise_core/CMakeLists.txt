# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# bitEngine 本地改动（components/muse_link/UPSTREAM.md 登记为 modified）：源文件只在直连型
# （BITENGINE_ENABLE_MUSE_LINK）里编，HA 型里不编任何源文件（docs/15 架构 5.8 R-3）；依赖照上游无条件声明。
# SRCS 为空时 IDF 建的是 INTERFACE 库，不能加 PRIVATE / PUBLIC 编译选项，所以下面两条一并放进条件里。
set(_noise_core_srcs "")
if(CONFIG_BITENGINE_ENABLE_MUSE_LINK)
    set(_noise_core_srcs
        "src/ClientSession.cpp"
        "src/InitiatorHandshake.cpp"
        "src/PsaCryptoBackend.cpp"
        "src/ServiceCodec.cpp"
        "src/Status.cpp"
        "src/Transport.cpp"
        "src/TransportFrameCodec.cpp")
endif()

idf_component_register(
    SRCS ${_noise_core_srcs}
    INCLUDE_DIRS "include"
    PRIV_REQUIRES mbedtls
)

if(CONFIG_BITENGINE_ENABLE_MUSE_LINK)
    target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
    target_compile_options(${COMPONENT_LIB} PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
        $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
    )
endif()
