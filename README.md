# Raft テストプログラム

一台のマシン上で、クラスタノードの数だけ raft_server を立ち上げ、raft_client からコマンドを送信する。

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
|command|コマンド|コマンド種別|--|
|option|オプション|オプション|--|

## 設定ファイル

3ノードクラスタの例

```
timeout 5
node0 3000
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