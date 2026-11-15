# 变更说明：磁盘缓存的“可替代性”判定（test1_2 修复轮）

| 项 | 值 |
|---|---|
| 日期 | 2026-11-15 |
| 起点 | `bc0c9c9`（上一轮交付说明） |
| 代码提交 | `0c289e7 fix(disk-cache): 隔离条目与可替代性判定` |
| 文档提交 | `750f547 docs: 更新 test1_2 修复轮规划` |
| 推送 | `origin/main`：`bc0c9c9..750f547` |
| 产物 | `0.9.0-dev (build 13)`，CUDA shared，sm_86-real + sm_89-real |
| 规划依据 | `betterModification.md` 的 `2026-11-15 20-31 - test1_2` |
| 逐文件记录 | `change.md` 的 `2026-11-15 20-47` 条目 |

---

## 1. 一句话结论

把“已有缓存条目能不能替代待写请求”从**三处各自内联的 token 前缀判断**，收敛成一个**与 `load()` 前置过滤同构的纯元数据判定**；隔离条目和不经济的条目不再抑制写入、也不再删除短快照。

## 2. 修了什么

| 缺陷 | 根因 | 后果 |
|---|---|---|
| A 隔离条目阻止有效写入 | `quarantined` 只在 `load()` 候选循环里排除；`covered_cheaply()`、`store()`/`mirror()` 精确去重、`worker()` 冗余判断都没过滤 | 同一条目“加载时不可用、写入时却算已覆盖”，新快照被丢弃 |
| B 长条目未必能替代短条目 | 四条写入/删除路径只看纯 token 前缀，忽略媒体身份、`min_tokens` 与 `size_file > lcp × 768 KiB` 成本守卫 | 1.2 GiB 条目会抑制 1.4k token 请求的写入，并删掉唯一经济可用的短快照 |
| C hash + 长度被当成身份 | `e.hash == hash && e.tokens.size() == …` 即判“已保存” | 哈希冲突会被当成重复，既不核对 token/media，也可能覆盖文件 |

## 3. 新的判定模型

新增 `tools/server/server-disk-cache-match.{h,cpp}`（纯元数据，不读 KV、不取锁、不碰 `llama_context`）：

```text
identity      = tokens + media_indices/media_token_counts/media_ids + file_bytes
match(a, b)   = token LCP
                 → 双向媒体比较（A 的块必须在 B 同位置、同长度、同 ID，反之亦然）
                 → 边界钳制（前缀不得停在媒体块内部）
cost_ok(f, l) = f <= l × 768 KiB
suppress(a, w)= media 兼容 ∧ usable ≥ min_tokens ∧ 尾部在 4096 内 ∧ cost_ok(a.file_bytes, usable)
replace(L, S) = S 是 L 的严格前缀 ∧ 整段媒体兼容 ∧ cost_ok(L.file_bytes, |S|)
duplicate(a,w)= hash+长度命中 ∧ 全量身份相等 → identical，否则 → none（拒绝写入）
```

调用点全部统一：

| 路径 | 用到的判定 |
|---|---|
| `load()` 候选过滤 | `match()` + `cost_ok()`（替换原内联的 LCP + 三处媒体循环，行为等价） |
| `covered_cheaply()`（写抑制） | `suppress()`，并先过 `is_reusable_entry()` |
| `store()` / `mirror()` 精确去重 | `classify_duplicate()`，冲突则拒绝 + 计数 |
| `worker()` 冗余判断 | `replace()` |
| `drop_prefixes()` / `dedup_prefixes()` | `replace()` |

`is_reusable_entry(e) = !e.quarantined && !e.tokens.empty()`，六条路径统一先过滤。

## 4. 行为对照

| 场景 | 修复前 | 修复后（实测） |
|---|---|---|
| 隔离条目（505.77 MiB）覆盖 1456-token 请求 | 判“已覆盖”，丢弃写入 | `stored 1456 tokens, 672.49 MiB` |
| 短条目 505.77 MiB + 长条目 1059.80 MiB（同前缀） | 按纯前缀删除短条目 | 两者并存；短条目随后仍被命中：`source=disk, entry_tokens=1385, read_bytes=530337934` |
| hash 命中但身份不同 | 计为 dedup | `SRV_ERR` 拒绝写入，`cache_disk_hash_collision_total++`，不覆盖原文件 |
| 合法覆盖（1603 token 被 1392 条目覆盖） | 跳过 678.05 MiB 写入 | 仍然跳过（抑制未被削弱） |

## 5. 验证

| 检查 | 结果 |
|---|---|
| CUDA 构建 | 通过，`0 FAILED` |
| `test_disk_cache_match.cpp` 9 组（纯 CPU） | 通过（首轮 2 项失败为用例构造问题，已修夹具而非放宽实现） |
| 隔离场景（持句柄触发删除失败） | 隔离 + 新写入成功；`entries 2 / bytes 1.23549e+09 / quarantined 1 / enqueue_dedup 0` |
| 短/长条目并存 | 两文件共存，短条目命中一次 |
| 启动期去重 | `2 entries after scan, 2 after dedup/trim` |
| 标准回归 | `1385/0`、`4/1381`、`215/1381` 与上轮逐项相同；输出 sha 相同（`fd7c5bda…`、`03e0e517…`） |

## 6. 未改动 / 兼容性

- 条目格式 **v8**、清单 **v3**、CRC 覆盖范围、配置签名、target/draft/checkpoint 状态：不变，**无需数据迁移**。
- `DISK_CACHE_MAX_BYTES_PER_TOKEN = 768 KiB` 与 `DISK_CACHE_MAX_PREFIX_LAG_TOKENS = 4096` 数值不变，只是常量移到了匹配模块并成为唯一出处。
- 关闭态：修复在磁盘共用路径上，与 `--cache-prefix-discovery` 是否开启无关；`server-prefix-index.*` 仍是只读元数据目录。
- 删除仍只走 `remove_file()`，成功后才扣配额并撤销引用；隔离条目继续占账。

## 7. 未覆盖

- 媒体分支只有单元测试覆盖：测试机是纯文本 prompt，未启用 mmproj 输入。
- 真实 64 位 hash 冲突无法构造，端到端仅确认计数为 0。
- 多 slot、多用户、Linux 构建、取消/中断路径未做。

## 8. 回滚

回滚到 `bc0c9c9` 即可，无需清理缓存目录——新代码没有引入新的文件格式或索引文件；已写入的 v8 条目在旧代码下同样可读。

```bash
git revert --no-commit 0c289e7 750f547 && git commit -m "revert: 可替代性判定修复"
```
