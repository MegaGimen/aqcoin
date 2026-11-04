# AQCoin 实施状态

基线：Bitcoin Core v32.1（上游 tag `v32.1`，提交 `f0de87f04`）。
本文件记录对照《AQ 链实施方案》各阶段的完成情况、测试结果和已知限制。
测试环境：临时云容器（4 核、15 GB 内存、x86 SHA-NI），不是种子服务器。

## 阶段完成情况

| 阶段 | 状态 | 说明 |
|---|---|---|
| 0 基线 | 完成 | v32.1 原版：编译 13 分钟；单元测试 374/374 通过；功能测试 275 通过、22 跳过、4 失败（3 个 bind 测试因容器缺少 IPv6/网卡；1 个是测试目录复制问题） |
| 1 AQ 函数与工作量表 | 完成 | `AQValue`、`AQWork`、`AQThresholdForWork`；`src/pow_aq_table.h` 由 `contrib/aq/gen_work_table.py` 生成；单元测试独立重算 x = 0, 1, 480, 537, 560, 600, 666, 700, 800, 900, 959, 960 |
| 2 共识 | 完成 | PoW 判定、`GetBlockProof`、每块 LWMA、时间规则、所有 `nBits` 代码路径、RPC 字段 |
| 3 链参数与网络身份 | 完成 | 主网/测试网/regtest 参数；删除 signet、testnet3、testnet4；`~/.aqcoin`、`aqcoin.conf` |
| 4 创世区块 | 完成 | 主网 x = 700（nonce 776123727，AQ 702，用 4 线程挖了 32 秒），测试网 x = 600，regtest x = 0；均有 `assert` 校验哈希和默克尔根 |
| 5 矿工 | 完成 | `contrib/aqminer`：多线程、getblocktemplate、自建 coinbase、提交后立即换模板、统计、`-benchmark` |
| 6 本机测试网 | 部分完成 | 场景 1–3 由 `feature_aq_pow.py` 覆盖并通过；在本机 3 节点测试网上用 aqminer 实测出块；场景 4 在 regtest 上验证（原因见下）；场景 5 只在初始阶段观察到门槛调整 |
| 7 主网上线 | 未做 | 按约定暂缓（需要在种子服务器上执行） |
| 8 发布准备 | 部分完成 | 写了 `README_AQ.md` 和本文件；程序改名和打标签未做 |

## 测试结果（当前代码）

* 单元测试（`ctest`）：373/373 通过（删除了 `testnet4_miner_tests`；新增 AQ 相关用例）。
* 功能测试（`test_runner.py -j4`）：最近一次全量运行 273 通过、22 跳过、4 失败。
  * 3 个 `rpc_bind.py --ipv4/--nonloopback`、`feature_bind_extra.py`：与基线相同，属于容器环境问题（没有 `/proc/net/tcp6`）。
  * 1 个 `feature_loadblock.py`：测试里写死了比特币 regtest 的网络标识字节，已修复，单独运行通过。
* `feature_aq_pow.py`（新增，跑在测试网参数上）：通过。

### LWMA 模拟（`pow_tests`，主网参数）

| 场景 | 结果 |
|---|---|
| 算力稳定（x = 650 附近） | 平均出块 607 s；门槛保持在 648–654 |
| 算力突增 10 倍 | 49 块后到达新门槛 666，之后平均出块 604 s |
| 算力降到 1/10 | 76 块后到达新门槛 632，之后平均出块 608 s |
| 从 x_init = 700 出发，算力只有其假设的 1/100 | 第 175 块到达 673，从不低于 x_min = 560，之后平均出块接近 600 s |
| 几乎没有算力 | 门槛降到 x_min = 560 后停在那里 |

### 本机 3 节点测试网（aqminer，2 线程，约 14.7 MH/s）

* 三个节点互相 `addnode`，最佳区块保持一致。
* 第 1–60 块使用 x_init = 600（每块约 1,800 次哈希，几乎瞬间出块）。第 61 块起 LWMA 生效：
  因为前 60 块的间隔都被截断为 1 s，门槛跳到 705；按 14.7 MH/s 估算每块约 6.3 分钟，
  之后会逐渐收敛到使出块间隔接近 600 s 的门槛（约 707）。
* 早期版本的矿工在一个模板已被挖出后仍继续在上面挖，产生大量重复提交；已改为提交后立即刷新模板。

实测的门槛序列（节点 2 上的 `getblockheader`）：

| 高度 | 门槛 x | 距上一块 |
|---|---|---|
| 1–60 | 600 | 1–2 s（第 1 块距创世时间 84418 s） |
| 61 | 646 | 1 s |
| 62 | 663 | 1 s |
| 63 | 678 | 2 s |
| 64 | 692 | 1 s |
| 65 | 705 | 1428 s（这期间矿工停下，让功能测试独占 CPU） |
| 66 | 697 | 8 s |
| 67 | 698 | 25 s |
| 68 | 699 | 56 s |
| 69 | 700 | 65 s |
| 下一块 | 701 | |

可以看到：快块时门槛逐块上升；矿工停止造成一个长间隔后门槛下降；之后又随出块速度回升。
三个节点的最佳区块哈希保持一致。

### 场景 4（钱包）

在测试网上，coinbase 要成熟 100 块；以本机算力在门槛约 705 时需要约 10 小时，所以在 regtest 上用两个节点验证：

* 第 1 块后 coinbase 为未成熟（immature 50），第 100 块时可用余额仍为 0，第 101 块时变为 50。
* n1→n2 转账 12.5 AQC，确认数 1→2→3 正常增长；n2→n1 回转 2 AQC 成功；两节点最佳区块一致。

## 实现中发现并修复的问题

1. **`CBlockHeader::IsNull()`** 只检查 `nBits == 0`。regtest 的门槛就是 0，所以所有 regtest 区块头都会被当作空的，紧凑区块中继（`PartiallyDownloadedBlock::InitData`）会失败。现在改为同时要求时间戳为 0、默克尔根为空。
2. **base58 / bech32 前缀歧义**：主网 P2SH 地址以 `a` 开头，bech32 前缀是 `aq`。`DecodeDestination` 原来按“前缀是否等于 HRP（不区分大小写）”来判断是不是 bech32，于是以 `aq`/`aQ` 开头的 P2SH 地址（约 3.4%）无法解析。现在合法的 Base58Check 字符串一律按 Base58 解析，并新增单元测试 `key_io_base58_hrp_prefix`。
3. **规格中的一处说明有误**（第 3.4 节）：“x < 537 时工作量只有 1 或 2”并不准确，实际只有 x ≲ 500 时才是 1 或 2（work(512) = 5，work(536) = work(537) = 15）。从 537 起严格递增的结论成立，`x_min ≥ 540` 的要求也成立。

## 第 4 节关键词的处理

| 关键词 | 处理 |
|---|---|
| `SetCompact` / `GetCompact` | 共识和 RPC 代码中已全部移除；`arith_uint256` 本身的实现和它的单元测试保留 |
| `nBits` | 语义改为整数门槛：`pow.cpp`、`chain.cpp`、`miner.cpp`、`rpc/mining.cpp`、`rpc/blockchain.cpp`、`rpc/util.cpp`、`bitcoin-util.cpp`、`primitives/block.h`、测试 |
| `powLimit` | 删除，改为 `nAQMinThreshold` |
| `CheckProofOfWork` / `DeriveTarget` | 改为 AQ 判定；删除 `DeriveTarget` |
| `GetBlockProof` | 返回 `work(nBits)` |
| `GetNextWorkRequired` / `CalculateNextWorkRequired` | 每块 LWMA |
| `PermittedDifficultyTransition` | 只检查 `[x_min, 960]`（regtest 要求不变），理由写在注释里 |
| `GetDifficulty` | `work(x) / work(x_min)` |
| `fPowAllowMinDifficultyBlocks`、`enforce_BIP94` | 删除（同时删除了测试网 20 分钟最低难度规则、BIP94 时间扭曲规则和挖矿端的 BIP54 时间规则） |
| `MAX_FUTURE_BLOCK_TIME` | 2 h → 30 min |
| `nMinimumChainWork` / `defaultAssumeValid` | 全部为 0 |
| `pchMessageStart` | 新的随机标识字节 |

### 3.6 节要求列出的配套时间常量

* `MAX_FUTURE_BLOCK_TIME`（`src/chain.h`）：7200 → 1800 秒。
* `TimeOffsets::WARN_THRESHOLD`（`src/node/timeoffsets.h`）：10 → 5 分钟。v32 已经没有 `-maxtimeadjustment`（网络时间调整），这个阈值是唯一与节点时间偏移相关的常量。
* `TIMESTAMP_WINDOW`（`src/chain.h`，钱包重扫的宽限窗口）：保持 2 小时，并加了 `static_assert(TIMESTAMP_WINDOW >= MAX_FUTURE_BLOCK_TIME)`。它更大只会让重扫多看一些块。
* `headerssync.cpp` 自动使用新的 `MAX_FUTURE_BLOCK_TIME`。

## 测试改动清单（因参数变化而修改的上游测试）

单元测试：

* `pow_tests`：重写为 AQ 测试
* `blockchain_tests`：难度定义改了
* `blockencodings_tests`、`headers_sync_chainwork_tests`：regtest nBits = 0，每块工作量为 1
* `miner_tests`：改在 regtest 上运行，激活高度设为不生效，以还原原测试在比特币主网低高度下的前提
* `bip324_tests`：用比特币主网网络标识字节校验 BIP324 官方向量（新增 `BIP324Cipher::Initialize` 的一个重载，只供测试使用）
* `key_io`、`descriptor`、`util`、`script_standard`、`rpc`、`key`、`bloom` 等测试：写死的比特币私钥和地址换成 AQ 版本
* `argsman_tests`、`validation_*_tests`：链类型、回归哈希、assumeutxo 区块哈希
* 删除 `testnet4_miner_tests.cpp`、`fuzz/signet.cpp`

功能测试（主要类别）：

* 配置文件和 pid 文件名、默认数据目录
* regtest 创世哈希、地址与私钥前缀、网络标识字节
* 每块工作量 1（`feature_minchainwork`、`p2p_headers_sync_with_minchainwork`、`networkhashps`）
* 30 分钟未来时间上限：`feature_bip68_sequence`、`feature_taproot`、`mempool_limit`、`feature_block`、`wallet_anchor` 中的模拟时钟处理
* AQ `nBits`：`feature_block`、`mining_template_verification`、`p2p_invalid_messages`、`interface_http`、`mining_basic`
* 为 `aq`/`aqrt` 前缀重新生成的无效 bech32 测试向量
* assumeutxo、`dumptxoutset`、`getblockstats` 等的确定性数据重新生成
* `rpc_scanblocks`：创世块付给 OP_RETURN，假阳性检查改用第 1 块
* BIP94/BIP54 时间规则测试不再运行（AQCoin 没有这些规则）
* 删除 `feature_signet.py`、`tool_signet_miner.py`、`mining_mainnet.py`（及其比特币主网区块数据 `data/mainnet_alt.json`，在 AQ 规则下无效，且无法在合理时间内重新生成）

## 已知限制和未做的事

* **程序名**仍为 `bitcoind`、`bitcoin-cli` 等（阶段 8 可选项）。
* **固定种子**：`chainparamsseeds.h` 还是上游的比特币节点列表，所以主网目前**不使用**固定种子（`vFixedSeeds.clear()`），DNS 种子也已清空。上线前要用 `contrib/seeds/` 根据种子服务器地址重新生成这个文件，并恢复 `chainparams.cpp` 中被注释掉的那一行（阶段 7）。在此之前，新节点需要用 `-addnode`/`-connect` 手动连接。
* **内核 C API**（`libbitcoinkernel`，默认不编译）：`btck_ChainType_TESTNET_4` / `SIGNET` 现在返回空指针；`test_kernel` 依赖比特币主网区块数据，未做适配。
* **fuzz 目标**（默认不编译）：修改了 `pow`、`p2p_headers_presync`、`versionbits` 让它们能编译，但没有运行；`utxo_snapshot` fuzz 用的 regtest 第 200 块 assumeutxo 数据没有重新生成。
* signet 的 BIP325 代码（`src/signet.cpp`）还在，但没有任何网络启用它。
* 场景 5（停止矿工或改线程数后观察门槛）：本机算力下 LWMA 一个窗口要 60 块 × 约 10 分钟，在会话时间内只观察到初始阶段的调整；稳态行为由单元测试的模拟和 `feature_aq_pow.py` 覆盖。
* 风险提示见 `README_AQ.md`（51% 攻击、难度刻度不均匀、时间戳操纵）。
