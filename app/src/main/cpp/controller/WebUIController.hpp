#ifndef ANDROSERVER_WEB_UI_CONTROLLER_HPP
#define ANDROSERVER_WEB_UI_CONTROLLER_HPP

#include "oatpp/web/server/api/ApiController.hpp"
#include "oatpp/core/macro/codegen.hpp"
#include "oatpp/core/macro/component.hpp"

#include OATPP_CODEGEN_BEGIN(ApiController)

/**
 * PC等のブラウザからファイル操作を行うための内蔵Web UIコントローラー
 */
class WebUIController : public oatpp::web::server::api::ApiController {
public:
    WebUIController(const std::shared_ptr<ObjectMapper>& objectMapper)
        : oatpp::web::server::api::ApiController(objectMapper) {}

    static std::shared_ptr<WebUIController> createShared() {
        OATPP_COMPONENT(std::shared_ptr<ObjectMapper>, objectMapper);
        return std::make_shared<WebUIController>(objectMapper);
    }

    ENDPOINT("GET", "/", root) {
        const char* html = R"html(<!DOCTYPE html>
<html lang="ja">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>AndroServer - 高速ファイル転送</title>
<style>
  :root { --primary: #6200ee; --primary-hover: #3700b3; --bg: #f8f9fa; --card: #ffffff; --text: #212529; --border: #e0e0e0; }
  * { box-sizing: border-box; margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; }
  body { background: var(--bg); color: var(--text); padding: 24px; max-width: 1000px; margin: 0 auto; }
  header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 24px; padding-bottom: 16px; border-bottom: 1px solid var(--border); }
  h1 { font-size: 24px; color: var(--primary); }
  .card { background: var(--card); border-radius: 12px; padding: 20px; box-shadow: 0 2px 8px rgba(0,0,0,0.06); margin-bottom: 24px; }
  .drop-zone { border: 2px dashed var(--primary); border-radius: 12px; padding: 36px; text-align: center; cursor: pointer; transition: 0.2s; background: #faf5ff; }
  .drop-zone.dragover { background: #efe4ff; border-color: var(--primary-hover); transform: scale(1.01); }
  .drop-zone p { color: #555; margin-top: 8px; font-size: 14px; }
  .btn { background: var(--primary); color: white; border: none; padding: 10px 18px; border-radius: 8px; cursor: pointer; font-size: 14px; font-weight: bold; transition: 0.2s; }
  .btn:hover { background: var(--primary-hover); }
  .btn-danger { background: #dc3545; }
  .btn-danger:hover { background: #b02a37; }
  .btn-sm { padding: 6px 12px; font-size: 12px; }
  .breadcrumb { display: flex; gap: 8px; align-items: center; margin-bottom: 16px; font-size: 14px; }
  .breadcrumb span { color: var(--primary); cursor: pointer; text-decoration: underline; }
  table { width: 100%; border-collapse: collapse; margin-top: 8px; }
  th, td { padding: 12px 16px; text-align: left; border-bottom: 1px solid var(--border); font-size: 14px; }
  th { background: #f1f3f5; font-weight: 600; color: #495057; }
  tr:hover td { background: #f8f9fa; }
  .file-icon { margin-right: 8px; }
  .dir-link { color: var(--primary); font-weight: 600; cursor: pointer; text-decoration: none; }
  .dir-link:hover { text-decoration: underline; }
  .actions { display: flex; gap: 8px; }
  #progressBar { width: 100%; height: 8px; background: #e9ecef; border-radius: 4px; overflow: hidden; margin-top: 16px; display: none; }
  #progressFill { width: 0%; height: 100%; background: var(--primary); transition: width 0.1s; }
  #statusMsg { margin-top: 8px; font-size: 13px; color: #666; }
</style>
</head>
<body>

<header>
  <h1>⚡ AndroServer</h1>
  <button id="newFolder" class="btn btn-sm" onclick="showNewFolderPrompt()">+ 新規フォルダ</button>
</header>

<div class="card">
  <div class="drop-zone" id="dropZone" onclick="document.getElementById('fileInput').click()">
    <h2>ここにファイルをドロップして高速アップロード</h2>
    <p>またはクリックしてファイルを選択</p>
    <input type="file" id="fileInput" multiple style="display:none">
    <div id="progressBar"><div id="progressFill"></div></div>
    <div id="statusMsg"></div>
  </div>
</div>

<div class="card">
  <div class="breadcrumb" id="breadcrumb"></div>
  <table>
    <thead>
      <tr>
        <th>名前</th>
        <th>サイズ</th>
        <th>更新日時</th>
        <th style="width: 160px;">操作</th>
      </tr>
    </thead>
    <tbody id="fileListBody">
      <tr><td colspan="4" style="text-align:center;">読み込み中...</td></tr>
    </tbody>
  </table>
</div>

<script>
let currentPath = '';

function formatBytes(bytes) {
  if (bytes === 0) return '-';
  const k = 1024;
  const sizes = ['B', 'KB', 'MB', 'GB', 'TB'];
  const i = Math.floor(Math.log(bytes) / Math.log(k));
  return parseFloat((bytes / Math.pow(k, i)).toFixed(2)) + ' ' + sizes[i];
}

function formatDate(timestamp) {
  if (!timestamp) return '-';
  const d = new Date(timestamp * 1000);
  return d.toLocaleString('ja-JP');
}

async function loadFiles(path = '') {
  currentPath = path;
  document.getElementById('newFolder').disabled = !path;
  document.getElementById('dropZone').style.display = path ? '' : 'none';
  updateBreadcrumb();
  const tbody = document.getElementById('fileListBody');
  tbody.innerHTML = '<tr><td colspan="4" style="text-align:center;">読み込み中...</td></tr>';
  
  try {
    const res = await fetch(`/api/files/list?path=${encodeURIComponent(path)}`);
    if (!res.ok) throw new Error(await res.text() || res.statusText);
    const data = await res.json();
    if (currentPath !== path) return;
    tbody.innerHTML = '';
    
    if (path !== '') {
      const parentPath = path.includes('/') ? path.substring(0, path.lastIndexOf('/')) : '';
      const cell = tbody.insertRow().insertCell();
      cell.colSpan = 4;
      appendDirectoryLink(cell, '📁 .. (親ディレクトリへ)', parentPath, 'dir-link');
    }

    if (!data.items || data.items.length === 0) {
      const cell = tbody.insertRow().insertCell();
      cell.colSpan = 4;
      cell.textContent = 'ファイルがありません';
      return;
    }

    data.items.sort((a, b) => (b.is_dir - a.is_dir) || a.name.localeCompare(b.name));
    for (const item of data.items) {
      const row = tbody.insertRow();
      const itemPath = path ? path + '/' + item.name : item.name;
      const name = row.insertCell();
      const icon = document.createElement('span');
      icon.className = 'file-icon';
      icon.textContent = item.is_dir ? '📁' : '📄';
      name.appendChild(icon);
      if (item.is_dir) {
        appendDirectoryLink(name, item.name, itemPath, 'dir-link');
      } else {
        name.append(item.name);
      }
      row.insertCell().textContent = item.is_dir ? '-' : formatBytes(item.size);
      row.insertCell().textContent = formatDate(item.modified_at);

      const actionCell = row.insertCell();
      if (!path) continue;
      const actions = document.createElement('div');
      actions.className = 'actions';
      actionCell.appendChild(actions);
      if (!item.is_dir) {
        const download = document.createElement('a');
        download.className = 'btn btn-sm';
        download.href = '/api/files/download?path=' + encodeURIComponent(itemPath);
        download.download = item.name;
        download.textContent = 'DL';
        actions.appendChild(download);
      }
      const remove = document.createElement('button');
      remove.className = 'btn btn-sm btn-danger';
      remove.textContent = '削除';
      remove.addEventListener('click', () => deleteItem(itemPath));
      actions.appendChild(remove);
    }
  } catch (err) {
    if (currentPath !== path) return;
    tbody.replaceChildren();
    const cell = tbody.insertRow().insertCell();
    cell.colSpan = 4;
    cell.textContent = '取得エラー: ' + err.message;
  }
}

function appendDirectoryLink(container, label, path, className = '') {
  const link = document.createElement('span');
  link.className = className;
  link.textContent = label;
  link.addEventListener('click', () => loadFiles(path));
  container.appendChild(link);
}

function updateBreadcrumb() {
  const container = document.getElementById('breadcrumb');
  container.replaceChildren();
  appendDirectoryLink(container, '🏠 Root', '');

  let built = '';
  for (const part of currentPath.split('/')) {
    if (!part) continue;
    built = built ? built + '/' + part : part;
    container.append(' / ');
    appendDirectoryLink(container, part, built);
  }
}

async function uploadFile(file) {
  const progressBar = document.getElementById('progressBar');
  const progressFill = document.getElementById('progressFill');
  const statusMsg = document.getElementById('statusMsg');
  
  progressBar.style.display = 'block';
  statusMsg.innerText = `${file.name} をアップロード中...`;
  
  const targetPath = currentPath ? `${currentPath}/${file.name}` : file.name;
  const startTime = Date.now();
  
  return new Promise((resolve, reject) => {
    const xhr = new XMLHttpRequest();
    xhr.open('POST', `/api/files/upload?path=${encodeURIComponent(targetPath)}`);
    xhr.setRequestHeader('Content-Type', 'application/octet-stream');
    
    xhr.upload.onprogress = (e) => {
      if (e.lengthComputable) {
        const percent = (e.loaded / e.total) * 100;
        progressFill.style.width = percent + '%';
        const elapsedSec = (Date.now() - startTime) / 1000;
        const speed = elapsedSec > 0 ? (e.loaded / elapsedSec) : 0;
        statusMsg.innerText = `${file.name}: ${percent.toFixed(1)}% (${formatBytes(speed)}/s)`;
      }
    };
    
    xhr.onload = () => {
      if (xhr.status >= 200 && xhr.status < 300) {
        statusMsg.innerText = `${file.name} のアップロード完了！`;
        resolve();
      } else {
        statusMsg.innerText = `エラー: ${xhr.responseText}`;
        reject(new Error(xhr.responseText));
      }
    };
    xhr.onerror = () => reject(new Error('ネットワークエラー'));
    xhr.send(file);
  });
}

async function handleFiles(files) {
  for (let i = 0; i < files.length; i++) {
    await uploadFile(files[i]);
  }
  setTimeout(() => {
    document.getElementById('progressBar').style.display = 'none';
    document.getElementById('statusMsg').innerText = '';
  }, 2000);
  loadFiles(currentPath);
}

document.getElementById('fileInput').addEventListener('change', (e) => {
  if (e.target.files.length > 0) handleFiles(e.target.files);
});

const dropZone = document.getElementById('dropZone');
dropZone.addEventListener('dragover', (e) => { e.preventDefault(); dropZone.classList.add('dragover'); });
dropZone.addEventListener('dragleave', () => dropZone.classList.remove('dragover'));
dropZone.addEventListener('drop', (e) => {
  e.preventDefault();
  dropZone.classList.remove('dragover');
  if (e.dataTransfer.files.length > 0) handleFiles(e.dataTransfer.files);
});

async function showNewFolderPrompt() {
  const name = prompt('作成するフォルダ名を入力してください:');
  if (!name) return;
  const targetPath = currentPath ? `${currentPath}/${name}` : name;
  try {
    const res = await fetch(`/api/directories?path=${encodeURIComponent(targetPath)}`, { method: 'POST' });
    if (res.ok) {
      loadFiles(currentPath);
    } else {
      alert('フォルダ作成失敗: ' + (await res.text()));
    }
  } catch (err) {
    alert('エラー: ' + err.message);
  }
}

async function deleteItem(path) {
  if (!confirm(`本当に削除しますか？\n${path}`)) return;
  try {
    const res = await fetch(`/api/files?path=${encodeURIComponent(path)}`, { method: 'DELETE' });
    if (res.ok) {
      loadFiles(currentPath);
    } else {
      alert('削除失敗: ' + (await res.text()));
    }
  } catch (err) {
    alert('エラー: ' + err.message);
  }
}

loadFiles();
</script>
</body>
</html>)html";

        auto response = createResponse(Status::CODE_200, html);
        response->putHeader("Content-Type", "text/html; charset=utf-8");
        response->putHeader("Cache-Control", "no-store");
        response->putHeader("X-Frame-Options", "DENY");
        return response;
    }
};

#include OATPP_CODEGEN_END(ApiController)

#endif // ANDROSERVER_WEB_UI_CONTROLLER_HPP

