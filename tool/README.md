# tool — オーサリング GUI（設計書 11 章）

スタンドアロンのオーサリングアプリ。ランタイムと同じ C++ の DSP コードを使い、
ライブリンク（`spec/interface/livelink-protocol.md`）でゲームに接続する。

- 依存（凍結、ルート `CMakeLists.txt` の `SND_MODULE_DEPS_tool`）: buildtool, dsp, core（プレビュー）, livelink
- UI 技術は未決（Dear ImGui が第一候補、設計書 15.2）。
- 段階 0 ではディレクトリのみ。ビルドは `-DSND_BUILD_TOOL=ON` で有効にするが、現時点ではターゲットを作らない。
- 着手は段階 2（ミキサーとプロファイラ）から。サードパーティ依存の導入方針は着手時に契約変更として提案する。
