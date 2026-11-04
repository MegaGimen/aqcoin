# Various test vectors

## mainnet_alt.json (removed)

AQCoin removed `mainnet_alt.json` and `mining_mainnet.py`. The file held Bitcoin
mainnet-difficulty blocks, which are not valid under the AQ proof of work, and
regenerating an equivalent chain at AQCoin's mainnet x_init = 700 (about 2e9
hashes per block) is not practical for a test fixture. Per-block LWMA
retargeting is covered by `feature_aq_pow.py` (testnet) and the `pow_tests`
unit tests instead.
