#include "readiness_fixture.hpp"

#include <iostream>

int main()
{
    for (const auto depth : {1U, 10U, 100U}) {
        const auto result = nova::readiness::Reorganize(depth);
        if (!result.has_value()) {
            std::cerr << "deterministic REGTEST fixture failed at depth " << depth << '\n';
            return 1;
        }
        std::cout << depth << '|' << result->height << '|' << result->coins << '|' << result->blocks
                  << '|' << result->transactions << '|' << result->utxos << '|' << result->tip
                  << '|' << result->chainwork << '\n';
    }
    return 0;
}
