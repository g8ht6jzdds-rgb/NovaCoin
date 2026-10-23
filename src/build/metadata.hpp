#pragma once

#include "build/version.hpp"
#include "consensus/network_params.hpp"

#include <initializer_list>
#include <ostream>

namespace nova::build
{

inline void PrintBuildMetadata(std::ostream& output)
{
    // Generated strings use restricted public alphabets; no paths or credentials.
    output << "{\"version\":\"" << kApplicationVersion << "\",\"git_commit\":\"" << kSourceRevision
           << "\",\"git_tree\":\"" << kSourceTree << "\",\"source_state\":\"" << kSourceState
           << "\",\"compiler\":\"" << kCompiler << "\",\"target_platform\":\"" << kTargetPlatform
           << "\",\"configuration\":\"" << kBuildConfiguration << "\",\"build_timestamp\":";
    if (kBuildTimestamp.empty()) {
        output << "null";
    } else {
        output << kBuildTimestamp;
    }
    output << ",\"networks\":{";
    bool first = true;
    for (const auto network : {consensus::NetworkId::kRegtest, consensus::NetworkId::kTestnet,
                               consensus::NetworkId::kMainnet}) {
        const auto& parameters = consensus::GetNetworkParams(network);
        const auto name = network == consensus::NetworkId::kRegtest   ? "regtest"
                          : network == consensus::NetworkId::kTestnet ? "testnet"
                                                                      : "mainnet";
        output << (first ? "" : ",") << '"' << name
               << "\":{\"protocol_version\":" << parameters.protocol_version
               << ",\"minimum_peer_version\":" << parameters.minimum_peer_protocol_version << '}';
        first = false;
    }
    output << "}}\n";
}

} // namespace nova::build
