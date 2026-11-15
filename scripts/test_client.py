#!/usr/bin/env python3
"""転送速度、ファイルの完全性、Range 応答を確認する。"""

import argparse
import getpass
import hashlib
import os
import tempfile
import time
import uuid

import requests


MIB = 1024 * 1024


def report(label, size, elapsed):
    print(f"{label}: {size / MIB / elapsed:.2f} MiB/s ({size * 8 / 1e6 / elapsed:.2f} Mbps)")


def benchmark_transfer(session, url, folder):
    directory = f"{folder}/andro_benchmark_{uuid.uuid4().hex}"
    path = {"path": f"{directory}/test.bin"}
    size = 100 * MIB
    session.get(f"{url}/api/files/list", timeout=30).raise_for_status()
    session.post(f"{url}/api/directories", params={"path": directory}, timeout=30).raise_for_status()
    try:
        uploaded = hashlib.sha256()
        with tempfile.TemporaryFile() as source:
            for _ in range(100):
                chunk = os.urandom(MIB)
                source.write(chunk)
                uploaded.update(chunk)
            expected_suffix = chunk[-500:]
            source.seek(0)

            started = time.perf_counter()
            response = session.post(
                f"{url}/api/files/upload",
                params=path,
                data=source,
                headers={"Content-Type": "application/octet-stream", "Content-Length": str(size)},
                timeout=120,
            )
            response.raise_for_status()
            result = response.json()
            if not result.get("success") or result.get("bytes") != size:
                raise ValueError("アップロード応答のバイト数が一致しません")
            report("アップロード", size, time.perf_counter() - started)

        downloaded = hashlib.sha256()
        received = 0
        started = time.perf_counter()
        with session.get(f"{url}/api/files/download", params=path, stream=True, timeout=120) as response:
            response.raise_for_status()
            for chunk in response.iter_content(chunk_size=256 * 1024):
                received += len(chunk)
                downloaded.update(chunk)
        if received != size or downloaded.digest() != uploaded.digest():
            raise ValueError("ダウンロードしたファイルが元データと一致しません")
        report("ダウンロード", received, time.perf_counter() - started)

        with session.get(
            f"{url}/api/files/download", params=path, headers={"Range": "bytes=-500"},
            stream=True, timeout=30,
        ) as response:
            if response.status_code != 206 or response.headers.get("Content-Range") != f"bytes {size - 500}-{size - 1}/{size}":
                raise ValueError("Range 応答が要求範囲と一致しません")
            if response.raw.read(501) != expected_suffix:
                raise ValueError("Range 応答の内容が一致しません")
        print("SHA-256 と Range 応答が一致しました。")
    finally:
        session.delete(f"{url}/api/files", params={"path": directory}, timeout=30).raise_for_status()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("url", help="Android アプリに表示されたサーバー URL")
    parser.add_argument("folder", help="Web UI に表示される公開フォルダ名")
    parser.add_argument("--user", default="androserver", help="アプリで設定したユーザー名")
    parser.add_argument("--ca", help="ローカル CA の公開証明書（PEM）")
    args = parser.parse_args()
    with requests.Session() as session:
        if args.ca:
            session.verify = args.ca
        password = getpass.getpass("AndroServer パスワード: ")
        credentials = base64.b64encode(f"{args.user}:{password}".encode("utf-8")).decode("ascii")
        session.headers["Authorization"] = f"Basic {credentials}"
        benchmark_transfer(session, args.url.rstrip("/"), args.folder)
