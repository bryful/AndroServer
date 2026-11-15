package com.androserver

object NativeLib {
    init {
        System.loadLibrary("androserver")
    }

    /**
     * C++ (oatpp) サーバーを起動
     * @param port リッスンポート番号 (例: 8080)
     * @param rootDirs 公開フォルダの UTF-8 パス
     * @return 起動成否
     */
    external fun startServer(
        port: Int,
        rootDirs: Array<ByteArray>,
        host: String,
        authorization: String,
        publicOrigin: String = ""
    ): Boolean

    /**
     * サーバーを停止
     */
    external fun stopServer()

    /**
     * サーバーが現在稼働中か確認
     */
    external fun isServerRunning(): Boolean

    /**
     * 稼働中ポート番号を取得
     */
    external fun getServerPort(): Int
}

