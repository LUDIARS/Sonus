# セッション D・ソース組 2: Codex に渡す指示書

本社で Codex に貼る指示をまとめたもの。作業の中身（範囲、実装の条件、受け入れテストの条件）は `D-hq-codex.md` にあり、この文書はそれを Codex のセッションごとの指示に切り分けたもの。

## 0. 使い方

セッションは 3 つに分ける。**実装と独立テストを同じセッションにしない**（独立テストは、実装の会話を知らないことに意味がある）。

| 順 | セッション | 貼る指示 | 終わりの成果 |
| --- | --- | --- | --- |
| 1 | 実装 | 1 章 | `int/s7-ext-src2` にコード、決定 D-200〜D-207、依頼 D-06 以降、報告 |
| 2 | 独立テスト（新しいセッション） | 2 章 | `source/test/indep_*` のテスト、欠陥と仕様の穴の報告 |
| 3 | 受け入れ（人、または 3 つ目のセッション） | 5 章 | 欠陥と仕様の穴への決定 |
| 4 | 実装（1 のセッションを再開） | 3 章 | 修正、落ちる独立テストの一覧 |
| 5 | 独立テスト（2 のセッションを再開） | 4 章 | テストの更新、再確認の報告 |
| 6 | 受け入れ | 5 章 | 全構成の結果、`report-D-src2.md` |

- 4 と 5 は、新しい欠陥が出なくなるまで繰り返す。ソース組 1 では 1 往復で済んだ。
- 始める前に、ブランチを作る。
  ```
  git fetch origin int/s7-ext
  git checkout -b int/s7-ext-src2 origin/int/s7-ext
  git push -u origin int/s7-ext-src2
  ```
- Codex には `AGENTS.md` を作らせない（LUDIARS の運用ファイルは後で別に作る決まり）。指示は、この文書の該当する章をそのまま貼る。
- `<...>` の所は、貼る前に埋める。

## 1. 実装のセッションに貼る指示

```
C++ のサウンドライブラリ Sonus（LUDIARS/sonus）の、セッション D・ソース組 2 の実装を担当してください。
ブランチは int/s7-ext-src2 です。ここにだけコミットして push します。main、int/s2-integration、int/s7-ext には push しません。

最初に、次を順に全部読んでください。読み終えるまでコードを書かないでください。
1. spec/handoff/parallel/README.md（共通の決まり）
2. spec/handoff/parallel/D-extensions.md（セッション D の指示書。2.1 と 3 章）
3. spec/handoff/parallel/D-hq-codex.md（今回の作業。2〜5 章があなたの範囲。6 章は受け入れの条件で、あなたのテストの目安にもなる）
4. spec/handoff/README.md の 3 章
5. spec/architecture/design.md の 2.5、3.3、3.4、5 章、11.1、12 章、14 章
6. spec/interface/CONTRACTS.md の、source と core⇄source に関する決定（D-43、D-44、D-91、D-94）と、末尾の「段階 7-D」の「ソース組 1」の小節（D-190〜D-199）
7. contract/internal/include/snd/internal/source_if.hpp（冒頭のコメントを全部）、common.hpp、contract/internal/include/snd/format/bank_format.hpp の SYNP と PTCH
8. source/ の既存の実装。特に oscillator_kind.*、noise_kind.*、input_kind.*、runtime.*、sample_kind.*、pcm_cache.*、テストの補助 source/test/synth_driver.hpp
9. core が各種別をどう起動しているか: core/src/sys_event.cpp の SYNP の分岐、core/src/bank_registry.cpp。buildtool/src/build.cpp の synth の所。core と buildtool は読むだけで、変えません。

やること（D-hq-codex.md の 4 章。この順で）:
- H1: 決定 D-200 の仕上げ。書きかけのコミット 3 つ（0adf38e、d5d09d1、de01480）が入っています。読んで、使うか書き直すかを決めてください。
- H2: Granular の種別。
- H3: Patch の種別と PTCH の形式、組み立てと検証の API。
- H4: Runtime への配線、公開 API の検討、他の持ち場への依頼（requests/D-06 以降）。

守ること:
- 書いてよいのは、source/、contract/ への追加だけの変更、api/compose/c_api.cpp の末尾への追記、CONTRACTS.md の「段階 7-D」の自分の小節、spec/handoff/parallel/requests/ だけです。それ以外（core、dsp、buildtool、bank、tests、ルートの CMakeLists.txt、cmake、bank-format.md など）は変えません。持ち場の外に変更が要るときは、変えずに依頼のファイルに書きます。
- source/test/indep_* は独立テストの担当のものです。書き換えません。あなたの変更で落ちるものがあれば、どのテストがなぜ落ちるかを報告に書きます。
- 既存のテストのアサーションは書き換えません。
- 契約は追加だけです。新しいコマンド種別は足しません。版の数字は上げません。
- 例外と RTTI を使いません。警告ゼロ（-Werror）。サードパーティの依存を足しません。source は dsp に依存できません。
- RT で安全に（RenderBatch などでロック、確保、IO をしない）。初期化のあとに確保しません。
- AGENTS.md、CLAUDE.md、CI の設定などの運用ファイルは作りません。
- 最小の縦切りで止めません。D-hq-codex.md の 4 章に書いた項目を全部、配線とテストまで入れます。

仕様に無いことを決めたら、CONTRACTS.md の「段階 7-D」に決定として書きます。番号は D-201〜D-207（D-200 は H1 用）。書く場所と書き方は D-hq-codex.md の 5.3 です。

テスト: source/test/ に新しいファイルを足します（CMake は末尾に追記）。解析的に期待値が分かる検査にします。何を確かめるかは D-hq-codex.md の 6.2〜6.5 を目安にしてください。独立テストの担当が別に付くので、網羅よりも、決めた仕様が動いていることの確認を優先してかまいません。

完了条件: D-hq-codex.md の 6.1 の 7 構成で、警告ゼロ・全件通過（D-200 で落ちる indep_* と、dsp の Defect_ の実行ファイルは除く）。並行性のあるテストは TSan と Release で 20 回の繰り返し。新しいファイルと変えたファイルを MinGW の構文チェックに通す。ビルドディレクトリはリポジトリの外に作ります。

コミットは区切りごとに。メッセージは日本語で、末尾にあなたのツールが求める署名を付けます。書きかけでも、題に「書きかけ」と書いて push してかまいません。

終わったら、次を報告してください。
- 作ったファイルと設計の要点（種別ごと。PTCH の形式の要約）
- 下した決定（D-番号ごと）
- 契約、公開ヘッダ、c_api.cpp の変更点の一覧
- 他の持ち場への依頼（ファイル名と要旨）
- 構成ごとの結果（回していない構成は「未実行」と書く）
- 処理時間（Release、128 本のステレオ、256 フレームあたり。仮想からの一斉復帰のブロックも）
- 落ちる indep_* のテストの一覧と理由
- 未検証の箇所、自信の無い箇所、仕様の穴だと思った点（隠さずに）
- ブランチの先頭のハッシュ
```

## 2. 独立テストのセッションに貼る指示

実装のセッションが終わって push されてから、**新しいセッション**に貼る。

```
C++ のサウンドライブラリ Sonus（LUDIARS/sonus）の、セッション D・ソース組 2 の独立テストを担当してください。
実装（決定 D-200 の仕上げ、Granular と Patch の 2 つのソース種別）は別の担当が済ませています。あなたは実装者とは独立に、仕様から性質を導いて検査し、欠陥を見つけます。実装は直しません。
ブランチは int/s7-ext-src2 です。ここにだけコミットして push します。

手順（この順を守ってください）:

1. 実装を読む前に、次だけを読んで、満たすべき性質を箇条書きに書き出します。書き出したものは最後の報告に入れます。
   - spec/handoff/parallel/README.md、D-extensions.md（2.1 と 3 章）、D-hq-codex.md（3 章、4 章、6 章）
   - spec/architecture/design.md の 2.5、3.3、5 章、7 章の「動的生成・入力」の行、14 章
   - spec/interface/CONTRACTS.md の末尾の「段階 7-D」のうち、D-200 と「ソース組 2」の小節（D-201〜D-207）。実装者が下した決定は仕様として扱います。ただし、決定そのものが設計書や指示書と食い違う、不合理、危険だと思えば、「決定への異議」として報告します。
   - 公開ヘッダと契約: contract/internal/include/snd/internal/source_if.hpp（冒頭のコメントを全部）、common.hpp、source/include/snd/source/ のヘッダ（runtime.hpp、パッチの組み立てのヘッダなど）
   - spec/handoff/parallel/requests/ の D-06 以降（実装者が「持ち場の外なので出来ていない」とした範囲。ここは欠陥ではなく依頼として扱います）
   この段階では、source/src/ の実装と、実装者のテスト（source/test/source_*）を開かないでください。

2. そのあとで実装と実装者のテストを読み、実装者のテストが見ていない性質、甘い許容差、机上で見つかる欠陥を探します。並行部分（遅れたチャンク D-43 の規則、Start / Release と描画の関係、素材の PCM の寿命）は、コードを読んで机上でも検証します。

3. source/test/ に新しいテストファイルだけを足します。名前は indep_ で始めます（indep_granular_test.cpp、indep_patch_test.cpp など）。CMake は source/test/CMakeLists.txt の末尾に追記します。期待値の計算は自分で持ち、実装の関数を期待値に使いません。ゴールデン音声に頼らず、解析的に期待値が分かる検査にします。確かめる性質は D-hq-codex.md の 6.2〜6.5 が最低限で、あなたが 1 で書き出した性質を足します。

4. D-200 で落ちている既存の独立テスト（IndepOsc.ExtremeFrequenciesAndRatiosStayFinite、ZeroFrequencyIsSilentAndKeepsThePhase、IndepInput.ThreadedWriterExactAccounting など。実装者の報告に一覧があります）は、あなたが直します。落ちる理由が決定の変更なのか実装の誤りなのかを、1 件ずつ確かめてください。決定の変更なら決定どおりの検査に書き直し、実装の誤りなら直さずに欠陥として残します。

欠陥と仕様の穴の扱い:
- 欠陥を見つけたら、失敗するテストを Defect_ で始まる名前で残し、直さずに報告します。Defect_ のテストは source/test/indep_defects_src2_test.cpp（別の実行ファイル）にまとめ、他の indep_ のテストは全部通る状態にします。
- 仕様が曖昧で、どちらが正しいとも言えないものは、「仕様の穴」として欠陥と分けて報告します。選択肢と、あなたの推奨を付けます。
- 実装、実装者のテスト、既存のテスト、契約、core など、自分の新しいテストファイルと CMake の末尾への追記以外は変更しません（4 の、ソース組 1 の独立テストの更新は例外です）。

回す構成: gcc Debug、-DSND_SANITIZE=address,undefined、-DSND_SANITIZE=thread、Release。ふだんは ctest -R で source と自分のテストに絞り、最後に gcc Debug で全体を 1 回。並行性のテストは TSan（-j1）と Release で ctest --repeat until-fail:20。自分の新しいファイルを MinGW の構文チェック（x86_64-w64-mingw32-g++ -std=c++20 -fsyntax-only -Werror）に通します。警告ゼロ（-Werror）。ビルドディレクトリはリポジトリの外に作ります。

コミットは区切りごとに。メッセージは日本語で、末尾にあなたのツールが求める署名を付けます。

終わったら、次を報告してください。
- 実装を読む前に書き出した性質
- 足したテストファイルと件数
- 欠陥の一覧（番号、症状、再現する Defect_ のテスト名、原因の見立て（ファイルと行）、重さ）
- 仕様の穴と決定への異議の一覧（選択肢と推奨）
- 机上で検証した並行部分の結論
- 構成ごとの結果、処理時間の測定値
- 検査できなかった性質とその理由
- ブランチの先頭のハッシュ
```

## 3. 差し戻しの指示（実装のセッションを再開して貼る）

受け入れ側が 5.1 で決定を下してから貼る。

```
ソース組 2 の差し戻しです。独立テストで、欠陥が <件数> 件、仕様の穴が <件数> 件出ました。下の決定どおりに、実装と決定の文書を直してください。
ブランチ int/s7-ext-src2 の先頭は <ハッシュ> です（あなたの実装に、独立テストの担当のテスト source/test/indep_* が加わっています）。

欠陥（Defect_ のテストは source/test/indep_defects_src2_test.cpp）:
<番号、症状、テスト名、原因の見立てを 1 件ずつ>

決定:
<D-番号ごとに、どう振る舞うべきかを 1 件ずつ。数値は具体的に>

やること:
- 上の欠陥を直し、決定を実装します。CONTRACTS.md の「ソース組 2」の小節に決定を書き足し、食い違う既存の記述も直します。
- あなた自身のテスト（source/test/source_*）で、決定と食い違うものは直してかまいません。
- 独立テストの担当のテスト（source/test/indep_*）は書き換えません。Defect_ のテストは、直ったら通るはずです。決定の変更で落ちる indep_* のテストは、どれがなぜ落ちるかを一覧にして報告します。
- gcc Debug（全体）、address,undefined、thread、Release で source 関係のテストを回します。警告ゼロ。変えたファイルを MinGW の構文チェックに通します。

報告: 変えた点、通るようになった Defect_ のテスト、落ちる indep_* のテストの一覧と理由、測り直した時間（変えたなら）、構成ごとの結果、ブランチの先頭のハッシュ。
```

## 4. 再確認の指示（独立テストのセッションを再開して貼る）

```
ソース組 2 の再確認をお願いします。受け入れ側があなたの報告に決定を下し、実装の担当が直しました。ブランチ int/s7-ext-src2 の先頭は <ハッシュ> です。

決定（全文は CONTRACTS.md の「段階 7-D」の「ソース組 2」の小節。実装を読む前に、まず決定の文面を読んでください）:
<D-番号ごとの要約>

実装の担当の報告で、決定の変更で落ちるとされたあなたのテスト:
<テスト名と理由の一覧>

やること:
1. 決定と食い違うあなたのテストを直します。落ちる理由が本当に決定の変更なのかを、1 件ずつ確かめてください。実装の誤りだと分かったものは、直さずに Defect_ として残します。仕様の穴の記録として書いたテストは、決定どおりの挙動を確かめる普通のテストに書き直します。許容差は、ただ緩めるのではなく、解析的に導ける上限で検査します。
2. 修正が新しい欠陥を持ち込んでいないかを確かめます。差分は git diff <差し戻し前のハッシュ>..<今のハッシュ> -- source/ contract/ api/ です。直した箇所の境界の条件と、並行部分を、机上とテストの両方で見ます。
3. 新しい決定の各項目に、対応するテストが少なくとも 1 件あることを確かめ、無ければ足します。
4. 直った Defect_ のテストは、名前から Defect_ を外して普通のテストのファイルへ移します。
5. 新しい欠陥は Defect_ のテストとして残し、直さずに報告します。実装と実装者のテストは変更しません。
6. gcc Debug（最後に全体を 1 回）、address,undefined、thread、Release で回します。並行性のテストは TSan（-j1）と Release で 20 回の繰り返し。

報告: 直したテストの一覧、新しい欠陥（あれば）、机上の検証の結論、測り直した時間、構成ごとの結果、ブランチの先頭のハッシュ。
```

## 5. 受け入れ側がやること

### 5.1 決定を下す（独立テストの報告のあと）

- 欠陥: 直させる。仕様どおりに直すだけなら決定番号は要らない。直し方に選択があるときだけ決定にする。
- 仕様の穴と決定への異議: 1 件ずつ、どう振る舞うかを決める。迷ったら、次の順で判断する。
  1. 設計書と指示書に書いてあることに合わせる。
  2. RT での費用に上限があること（一斉に起きても予算を超えない）。
  3. ゲームの作り手が驚かないこと（クリックが出ない、フルスケールを超えない、無音が挟まらない）。
  4. 持ち場の中で済むこと。済まないものは依頼にする。
- 記述だけの食い違い（実測と合わない数値など）は、実装を変えずに決定の文面を直させる。
- 決定は `CONTRACTS.md` に書かせる。受け入れ側が自分で書いてもよい。

参考: ソース組 1 で受け入れ側が下した決定は D-197〜D-200。同じ種類の論点（仮想からの復帰の費用、NaN の入力、開始直後の統計の数え方、利得とヘッドルーム）が Granular と Patch でも出る見込みが高い。

### 5.2 全構成を回す（再確認が済んだあと）

実装と独立テストの報告の数字をそのまま信じず、**新しいビルドディレクトリから**自分で回す。

```
#!/bin/sh
# usage: sh accept.sh <repo> <work dir outside the repo>
REPO=$1; OUT=$2; J=${J:-2}
run() {  # name, cmake args...
  name=$1; shift
  B=$OUT/$name
  rm -rf "$B"
  echo "== $name HEAD $(git -C "$REPO" rev-parse --short HEAD) args: $*"
  cmake -S "$REPO" -B "$B" -G Ninja "$@" >"$OUT/$name.cfg.log" 2>&1 || { echo "configure failed"; return; }
  cmake --build "$B" -j"$J" >"$OUT/$name.build.log" 2>&1; echo "build rc=$? warnings=$(grep -c 'warning:' "$OUT/$name.build.log")"
  [ "$name" = notests ] || { ctest --test-dir "$B" -j"$J" --output-on-failure >"$OUT/$name.test.log" 2>&1; tail -n 8 "$OUT/$name.test.log"; }
  [ -n "$KEEP" ] || rm -rf "$B"
}
run gcc-debug
run clang-debug -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang
run asan -DSND_SANITIZE=address,undefined
run tsan -DSND_SANITIZE=thread
run release -DCMAKE_BUILD_TYPE=Release
run notests -DSND_BUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=Release
run noopus -DSND_CODEC_OPUS=OFF
```

- 並行性のテストの繰り返しは、TSan と Release のビルドディレクトリを残して（`KEEP=1`）回す。TSan は `-j1`。
  ```
  ctest --test-dir <dir> --repeat until-fail:20 -R 'source_input|indep_input|indep_src1_(core|load|api)|<ソース組 2 の並行性のテスト>'
  ```
- 失敗してよいのは、dsp の `Defect_` の実行ファイルだけ（クラウドが dsp の差し戻しを入れるまで）。`platform_device_play_test` は SKIP になる。
- MinGW の構文チェックを、`git diff --name-only origin/int/s7-ext..HEAD -- '*.cpp' '*.hpp' '*.h'` の全ファイルに掛ける。
- 持ち場の外が変わっていないことを確かめる。次の出力が空であること。
  ```
  git diff --name-only origin/int/s7-ext..HEAD | grep -v -E '^(source/|contract/|api/compose/c_api\.cpp$|spec/interface/CONTRACTS\.md$|spec/handoff/parallel/)'
  ```
- 契約の変更が追加だけであることを、`git diff origin/int/s7-ext..HEAD -- contract/ api/compose/c_api.cpp` を読んで確かめる（削除された行と、変えられた既存の行が無いこと。`version.h` が変わっていないこと）。
- 要所のコードを読む。少なくとも、`RenderBatch` の中に確保とロックが無いこと、プログラムの検証が範囲外を読まないこと、グレインのプールが尽きたときの分岐。

### 5.3 判定と報告

`D-hq-codex.md` の 6.6 の 6 項目がそろったら受け入れる。`spec/handoff/parallel/report-D-src2.md` を書かせて（または書いて）push し、統合担当に知らせる。
