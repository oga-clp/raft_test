# Raft テストプログラム

一台のマシン上で、クラスタノードの数だけ raft_server を立ち上げ、raft_client からコマンドを送信する。

# 事前準備

プログラムと同階層に"dat"ディレクトリを作成しておく。

## サーバ実行方法

```
> ./raft_server node_name
```

|引数|説明|入力内容|備考|
|--|--|--|--|
|node_name|ノード名|任意の文字列|設定ファイルに定義されている必要がある。|

## クライアント実行方法

```
> ./raft_client command option
```

|引数|説明|入力内容|備考|
|--|--|--|--|
|command|コマンド|コマンド種別|GET: 最新のログエントリを取得 <br> SET: optionで指定した文字列をコミット|
|option|オプション|オプション|--|

## 設定ファイル

3ノードクラスタの例

```
5           # 選挙タイムアウト(秒)
node0 3000  # ノード名 ポート番号
node1 3001
node2 3002
```



(↓ 実装が大変なのでボツ)
```xml
<root>
	<cluster>
		<nodes>
			<node id="node0">
				<port> 3000 </port>
			</node>
			<node id="node1">
				<port> 3001 </port>
			</node>
			<node id="node2">
				<port> 3002 </port>
			</node>
		</nodes>
		<heartbeat>
			<timeout>
				<election> 5 </election>
			</timeout>
		</heartbeat>
	</cluster>
</root>
```

## コマンド一覧