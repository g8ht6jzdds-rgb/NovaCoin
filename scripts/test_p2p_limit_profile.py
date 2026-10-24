#!/usr/bin/env python3
"""Fail closed when the documented P2P profile drifts from PeerService."""

from __future__ import annotations

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
DOCUMENT = ROOT / "docs" / "p2p.md"
CONSTRUCTION = ROOT / "src" / "node" / "peer_service.cpp"
PARAMETERS = ROOT / "src" / "net" / "p2p.hpp"

PROFILE_HEADING = "## Current production parser limit profile"
DOCUMENT_BINDINGS = (
    "`NetworkParams::network_magic`",
    "`NetworkParams::minimum_peer_protocol_version`",
    "`NetworkParams::block_limits.max_serialized_size`",
    "`max_user_agent` | `256`",
    "`max_addresses` | `1,000`",
    "`max_inventory` | `50,000`",
    "`max_locator_hashes` | `101`",
    "`max_headers` | `2,000`",
    "`NetworkParams::block_limits.transaction_limits`",
    "`NetworkParams::block_limits`",
    "Static bootstrap ingestion policy",
    "canonical bootstrap format remains limited to 16 KiB",
    "30/300/60-second",
    "64/16 peer caps",
    "64 KiB read buffer",
    "8 MiB pending send bound",
    "16 accepts per poll",
    "2,000-header response",
    "4,096-pending-header",
)
PARAMETER_DECLARATIONS = (
    "std::uint32_t network_magic{};",
    "std::int32_t minimum_protocol_version{};",
    "std::uint32_t max_payload{};",
    "std::uint32_t max_user_agent{};",
    "std::uint32_t max_addresses{};",
    "std::uint32_t max_inventory{};",
    "std::uint32_t max_locator_hashes{};",
    "std::uint32_t max_headers{};",
    "primitives::TransactionLimits transaction_limits;",
    "primitives::BlockLimits block_limits;",
)
CONSTRUCTION_BINDING = """return {network.network_magic,
            network.minimum_peer_protocol_version,
            network.block_limits.max_serialized_size,
            256U,
            1'000U,
            50'000U,
            101U,
            2'000U,
            network.block_limits.transaction_limits,
            network.block_limits};"""


def validate(document: str, construction: str, parameters: str) -> None:
    """Check the approved documentation against its exact source binding."""
    if PROFILE_HEADING not in document:
        raise ValueError("missing P2P profile heading")
    for binding in DOCUMENT_BINDINGS:
        if binding not in document:
            raise ValueError(f"missing documented P2P profile binding: {binding}")
    if CONSTRUCTION_BINDING not in construction:
        raise ValueError("PeerService P2PParams construction no longer matches the profile")
    for declaration in PARAMETER_DECLARATIONS:
        if declaration not in parameters:
            raise ValueError(f"P2PParams declaration no longer matches profile: {declaration}")


class P2PLimitProfileTest(unittest.TestCase):
    def test_checked_in_profile_matches_production_construction(self) -> None:
        validate(
            DOCUMENT.read_text(encoding="utf-8"),
            CONSTRUCTION.read_text(encoding="utf-8"),
            PARAMETERS.read_text(encoding="utf-8"),
        )

    def test_documentation_drift_fails_closed(self) -> None:
        document = DOCUMENT.read_text(encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "max_inventory"):
            validate(
                document.replace("`max_inventory` | `50,000`", "`max_inventory` | `49,999`"),
                CONSTRUCTION.read_text(encoding="utf-8"),
                PARAMETERS.read_text(encoding="utf-8"),
            )

    def test_construction_drift_fails_closed(self) -> None:
        construction = CONSTRUCTION.read_text(encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "P2PParams construction"):
            validate(
                DOCUMENT.read_text(encoding="utf-8"),
                construction.replace("50'000U", "49'999U", 1),
                PARAMETERS.read_text(encoding="utf-8"),
            )


if __name__ == "__main__":
    unittest.main()
