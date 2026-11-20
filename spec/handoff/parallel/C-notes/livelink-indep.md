# 組 L（livelink 1.1）独立テストのメモ（途中）

ブランチ `c/livelink`。性質の一覧は `livelink/test/indep_s7_properties.md`（実装を読む前にコミット: eb8ec4b）。

## 進み具合

- eb8ec4b 性質の一覧
- 719de09 D-170: 既存のテスト 3 か所の更新
- 50065f0 契約・検査・手で組んだ記録（unit）、本物の core での 1.1（live）
- 8915416 記録と再生（record）
- ccc5129 ファズ（fuzz）

gcc Debug で indep_s7 の 4 本は全通過（ここまで）。この後: 並行性、実装を読んでの追加、構成ごとの実行。
