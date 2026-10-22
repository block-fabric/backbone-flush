// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_GRPC_DESCRIPTORS_H
#define BITCOIN_GRPC_DESCRIPTORS_H

#include <cstdint>
#include <span>

namespace grpcsrv {
/** The FileDescriptorSet of src/grpc/proto, with the files it imports (descriptors.cpp). */
std::span<const uint8_t> DescriptorSet();
} // namespace grpcsrv

#endif // BITCOIN_GRPC_DESCRIPTORS_H
