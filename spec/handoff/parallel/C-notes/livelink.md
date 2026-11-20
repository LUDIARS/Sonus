# 組 L（livelink）のメモ

作業中。区切りごとに更新する。

## 進み具合

- [x] プロトコル 1.1 のヘッダ、wire、runtime、applier、server、client、record_format、replayer の実装（コミット 9c15a1e）
- [ ] livelink/test の新しいテスト（単体、結合、ファズ）
- [ ] livelink-protocol.md を 1.1 に
- [ ] 依頼（requests/C-01〜）
- [ ] 全構成

## 既存のテストとの食い違い（直していない。受け入れ側の判断）

- `tests/integration/livelink_override_test.cpp:249` 試聴の全停止は UNSUPPORTED を期待。1.1 で全停止を動かしたので SND_OK になる。
- `tests/integration/indep_livelink_protocol_test.cpp:246` HelloReply.protocolMinor == 0 を期待。1.1 に上げたので 1。
- `tests/integration/indep_livelink_profile_record_test.cpp:380-381` 再生で LiveReplaceChunk を外す（skipped）前提。1.1 は記録にデータ（RecordLiveData）を入れて再生するので、skipped が 1 減り commands が 1 増える。
