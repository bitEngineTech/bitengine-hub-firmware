/* 只强制包含进 esp-matter 的 esp_matter_controller_pairing_command.cpp（CMakeLists.txt）。
 * esp-matter 1.6.0 在那里不带命名空间地写 PeerAddress，只有编进 CHIP 的 commissioner discovery（server 端功能）时才解析得到；
 * 我们不带 server。做法同 #159 的 esp_matter_compat.h。升级 esp-matter 时看还要不要。 */
#pragma once
#ifdef __cplusplus
#include <transport/raw/PeerAddress.h>
using chip::Transport::PeerAddress;
#endif
