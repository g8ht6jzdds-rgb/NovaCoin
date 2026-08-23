# NovaCoin monetary consensus

**Status:** implemented subsidy schedule, parameter validation, theoretical
issuance bound, and coinbase reward ceiling. Transaction input-value and fee
calculation remain part of the future state-dependent transaction-validation
pipeline; this module accepts only the already-validated aggregate fee amount.

## Atomic units and chain parameters

All amounts use signed 64-bit `Amount` atomic units. Floating point is never
used in monetary consensus.

```text
COIN            = 100,000,000 atomic units
MAX_MONEY       = 21,000,000 * COIN
INITIAL_SUBSIDY = 50 * COIN
```

`ChainParams` carries these three protocol constants and an explicit
`halving_interval`. Parameter validation rejects any different monetary
constant, a zero interval, integer-overflowing issuance, or a schedule whose
theoretical issuance exceeds `MAX_MONEY`. There is no implicit default network
parameter.

## Subsidy and issuance

For height `h` and a valid interval `I`, the block subsidy is:

```text
era      = floor(h / I)
subsidy  = floor(INITIAL_SUBSIDY / 2^era)
```

The calculation uses unsigned fixed-width shifts only after checking the shift
bound. The final nonzero era is era 32, which pays one atomic unit; era 33 and
later pay zero.

The theoretical issuance is calculated once per subsidy era with checked
integer multiplication and addition:

```text
sum(subsidy(era) * halving_interval) for every nonzero subsidy era
```

Any `ChainParams` value for which that sum is greater than `MAX_MONEY` is
invalid and must not be used by a validator.

## Coinbase reward ceiling

After structural block validation and state-dependent fee validation have
produced the exact non-negative aggregate `transaction_fees`, the coinbase must
satisfy:

```text
sum(coinbase outputs) <= GetBlockSubsidy(height) + transaction_fees
```

All output values, the output sum, the fee amount, and the reward limit are
checked before arithmetic. A coinbase that exceeds the limit is rejected as an
inflation attempt. A smaller coinbase is valid; the difference is unclaimed
subsidy or fees.
