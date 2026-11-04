# AQCoin

AQCoin 是一条独立的工作量证明区块链，代码分叉自 Bitcoin Core v32.1。
UTXO 模型、脚本、隔离见证、Taproot、钱包、交易格式和奖励减半机制都与比特币相同，
只在以下几点有本质区别：

1. **挖矿条件（AQ 工作量证明）**：区块头哈希（仍为 SHA256d）的 *AQ 值* ——
   64 位十六进制各位数字之和（0–960）—— 必须 **≥ 门槛 x**，取代比特币的 “哈希 ≤ 目标值”。
2. **难度调整**：门槛 x 每个区块用 LWMA（线性加权移动平均，窗口 N = 60）调整一次。
3. **独立网络身份**：新的创世区块、网络标识字节、端口、地址前缀和数据目录。

## 共识规则摘要

| 项目 | 规则 |
|---|---|
| AQ(h) | `Σ (h[i] >> 4) + (h[i] & 0x0F)`，对 32 字节求和，与字节顺序无关 |
| 区块头有效 | `x_min ≤ nBits ≤ 960` 且 `AQ(hash) ≥ nBits` |
| `nBits` | 直接存放整数门槛 x（不再是压缩目标值格式） |
| 区块工作量 | `work(x) = min(floor(2^256 / C(x)), 2^256 − 1)`，`C(x)` = AQ ≥ x 的哈希个数；预先算好的表在 `src/pow_aq_table.h`（由 `contrib/aq/gen_work_table.py` 生成） |
| 难度调整 | 每块 LWMA：N = 60，T = 600 s，单块出块时间截断到 `[1, 6T]`；把目标工作量映射到对数意义上最近的整数 x，再截断到 `[x_min, 960]`。高度不足 N 时使用 x_init |
| 时间规则 | 时间戳 > 前 11 块中位时间；未来时间上限 30 分钟（`MAX_FUTURE_BLOCK_TIME = 1800`） |
| 奖励 | 初始 50 AQC，每 210,000 块减半，总量 2,100 万；coinbase 成熟期 100 块 |
| 软分叉 | BIP34/65/66、CSV、隔离见证、Taproot 从第 1 块（隔离见证从创世）起生效 |

参考数值（一个随机哈希满足 AQ ≥ x 的概率）：x = 560 约 1/65，600 约 1/1,800，
650 约 1/650,000，700 约 1/2.2×10⁹，800 约 1/1.6×10²⁰。
x 每加 1，难度在 600 附近约乘 1.10，在 700 附近约乘 1.21，在 800 附近约乘 1.39。

## 网络参数

| | 主网 | 测试网 | regtest |
|---|---|---|---|
| `-chain=` | `main` | `test`（`-testnet`） | `regtest` |
| 网络标识字节 | `b5decce6` | `a795a09c` | `b6b1a4fd` |
| P2P 端口 | 9433 | 19433 | 19544 |
| RPC 端口 | 9432 | 19432 | 19543 |
| Bech32 前缀 | `aq` | `taq` | `aqrt` |
| Base58 P2PKH / P2SH / WIF | 23 (`A`) / 83 (`a`) / 151 | 66 (`T`) / 127 (`t`) / 194 | 同测试网 |
| x_min / x_init | 560 / 700 | 560 / 600 | 0 / 0（不调整） |
| 创世区块哈希 | `b5c0d4ce…6ae2dfcd` | `2d799d8c…ae1baf0d` | `87b3ee6d…9770e8da` |

主网和测试网创世区块时间为 `1793750400`（2026-11-04 00:00 UTC），regtest 沿用比特币 regtest 的
`1296688602`（便于测试中的模拟时钟）。所有创世区块的 coinbase 留言为
`Nothing human makes it out of the near-future`，奖励 50 AQC 付给 `OP_RETURN`（不可花费）。
主网 nonce 为 `776123727`（AQ = 702 ≥ 700），测试网 nonce 为 `516`（AQ = 605 ≥ 600）。
可用 `contrib/aq/genesis.py` 加 `aq-genesis-grind` 重新生成或校验。

默认数据目录为 `~/.aqcoin`（macOS 下为 `~/Library/Application Support/AQCoin`，
Windows 下为 `%LOCALAPPDATA%\AQCoin`），配置文件为 `aqcoin.conf`，与比特币完全分开。
Signet、testnet3、testnet4 已删除。

## 编译

依赖与 Bitcoin Core 相同（见 `doc/build-unix.md`），例如在 Ubuntu 上：

```sh
sudo apt-get install build-essential cmake pkg-config python3 libevent-dev libboost-dev libsqlite3-dev libzmq3-dev
cmake -B build -DBUILD_GUI=OFF -DENABLE_IPC=OFF
cmake --build build -j$(nproc)
ctest --test-dir build -j$(nproc)                 # 单元测试
build/test/functional/test_runner.py -j$(nproc)    # 功能测试
```

生成的程序在 `build/bin/` 下。为了不影响上游测试框架，第一版仍沿用 `bitcoind`、`bitcoin-cli`
等程序名。另外生成：

* `aqminer`：独立 CPU 矿工（`contrib/aqminer/`）
* `aq-genesis-grind`：创世区块 nonce 搜索工具（`contrib/aq/genesis_grind.cpp`）

## 运行节点

```sh
build/bin/bitcoind -daemon                # 主网，数据目录 ~/.aqcoin
build/bin/bitcoin-cli getblockchaininfo
build/bin/bitcoind -testnet -daemon       # 测试网
```

建议的 `~/.aqcoin/aqcoin.conf`：

```ini
server=1
listen=1
rpcbind=127.0.0.1
rpcallowip=127.0.0.1
# 使用默认的 cookie 认证，不要设置 rpcpassword
txindex=1
```

RPC 只应绑定在 `127.0.0.1`，不要对公网开放 RPC 端口。

### RPC 中与比特币不同的字段

* `getblocktemplate`：新增 `aq_threshold`（下一块的门槛 x）。`bits` 是 8 位十六进制的 x；
  `target` 仅供参考（等效哈希目标 `floor((2^256−1)/work(x))`），矿工必须使用 `aq_threshold`。
* `getblockheader` / `getblock` / `getblockchaininfo` / `getmininginfo`：新增 `aq_threshold`；
  `difficulty` = `work(x) / work(x_min)`。
* `mintime` 就是中位时间 + 1（没有 BIP94 规则）。

## 挖矿

```sh
build/bin/bitcoin-cli createwallet miner
ADDR=$(build/bin/bitcoin-cli -rpcwallet=miner getnewaddress)
build/bin/aqminer -address=$ADDR -threads=$(($(nproc)-1))             # 主网，使用 ~/.aqcoin/.cookie
build/bin/aqminer -testnet -address=<taq1...> -threads=2               # 测试网
build/bin/aqminer -benchmark=20 -threads=4                              # 只测哈希速率
```

`aqminer` 通过 `getblocktemplate` 获取模板，自行构造 coinbase（BIP34 高度、随机 extranonce、
隔离见证承诺），遍历 nonce（每个线程使用不同的 extranonce），AQ ≥ 门槛时调用 `submitblock` 提交。
它每秒轮询 `getbestblockhash`，出现新块时立即换模板，并定期输出哈希速率、当前门槛和预计出块时间。
`aqminer -help` 查看全部参数。

`generatetoaddress` 等 RPC 同样使用 AQ 规则（在 regtest 上可瞬间出块）。

## 种子节点

DNS 种子已清空。主网固定种子（`src/chainparamsseeds.h`）要在种子服务器的公网地址确定后
用 `contrib/seeds/` 的工具生成；在此之前，新节点需要用 `-addnode=<host>:9433` 手动连接。

## 与比特币的代码差异

* `src/pow.{h,cpp}`、`src/pow_aq_table.h`：AQ 值、工作量表、LWMA、PoW 判定
* `src/chain.cpp`：`GetBlockProof` 返回 `work(nBits)`
* `src/consensus/params.h`：`nAQMinThreshold`、`nAQInitThreshold`、`nLWMAWindow`
  （删除了 `powLimit`、`fPowAllowMinDifficultyBlocks`、`enforce_BIP94`）
* `src/kernel/chainparams.cpp`：三个网络的全部参数和创世区块；比特币的历史数据
  （检查点、assumevalid、最小链工作量、assumeutxo、chainTxData、BIP30/BIP16/Taproot 例外）已清空
* `src/chain.h`、`src/node/timeoffsets.h`：未来时间上限 30 分钟，节点时间偏移警告阈值 5 分钟
* `src/primitives/block.h`：`IsNull()` 不再只看 `nBits == 0`（regtest 的门槛就是 0）
* `src/rpc/*`、`src/node/miner.cpp`、`src/bitcoin-util.cpp`：RPC 输出、模板、grind 改用 AQ
* `src/common/args.cpp`、`src/chainparamsbase.cpp`：数据目录、配置文件、RPC 端口、删除的链
* `test/functional/test_framework/`：区块求解、`nBits`、网络标识、地址/私钥版本、配置文件名
* `test/functional/feature_aq_pow.py`：AQ 工作量证明 / LWMA / 分叉重组 / 无效区块的功能测试

测试结果和已知限制见 `doc/aq/STATUS.md`。

## 已知风险

* **51% 攻击**：初期全网算力集中在少数机器上，算力更强的人可以重写历史。
* **难度刻度不均匀**：x 每步的难度倍数随 x 增大（见上表），出块时间有 10%–20% 级别的波动，属于预期行为。
* **时间戳操纵**：每块调整的算法对时间戳更敏感，这正是采用 `[1, 6T]` 截断和 30 分钟未来时间上限的原因。

## 许可证

MIT，见 `COPYING`。保留了 Bitcoin Core 的版权声明。
