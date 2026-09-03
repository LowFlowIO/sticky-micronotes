#include "wifi_sync.h"
#include "config.h"
#include "file_manager.h"
#include "sd_backup.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <SDCardManager.h>
#include <Preferences.h>

// --- Internal state ---
static WebServer* server = nullptr;
static bool syncActive = false;
static SyncState syncState = SyncState::SCANNING;
static char statusText[64] = "";

extern bool screenDirty;

// --- Network list ---
static constexpr int MAX_NETWORKS = 20;
struct NetworkInfo {
  char ssid[33];
  int  rssi;
  bool encrypted;
  bool saved;  // Has stored password in NVS
};
static NetworkInfo networks[MAX_NETWORKS];
static int networkCount = 0;
static int selectedNet = 0;

// --- Password entry ---
static constexpr int MAX_PASSWORD_LEN = 63;
static char passwordBuf[MAX_PASSWORD_LEN + 1] = "";
static int  passwordLen = 0;

// --- NVS credential storage ---
static Preferences wifiPrefs;
static constexpr int MAX_SAVED_NETWORKS = 4;

static void loadSavedCredentials();
static bool getSavedPassword(const char* ssid, char* passBuf, int passBufSize);
static void saveCredential(const char* ssid, const char* pass);
static void forgetCredential(const char* ssid);

// --- Connecting state ---
static unsigned long connectStartMs = 0;
static char connectingSSID[33] = "";
static bool usedSavedPassword = false;
static bool autoConnectAttempted = false;  // True if we tried auto-connect with saved creds

// --- Sync activity tracking ---
static int filesSent = 0;       // Files downloaded by PC (GET)
static int filesReceived = 0;  // Files uploaded by PC (POST)
static int totalFilesToSync = 0; // Total .txt files on device

static constexpr int MAX_LOG_LINES = 6;
static char syncLog[MAX_LOG_LINES][48];
static int syncLogCount = 0;

static bool pcConnected = false;

static unsigned long lastHttpActivityMs = 0;
static constexpr unsigned long SYNC_TIMEOUT_MS = 60000;  // 60s no HTTP → auto-disconnect
static bool syncCompletePending = false;  // Set by handler, acted on in wifiSyncLoop

// --- DONE state ---
static unsigned long doneStartMs = 0;
static constexpr unsigned long DONE_DISPLAY_MS = 3000;  // 3s before returning to menu

// --- Forward declarations ---
static void startHttpServer();
static void stopHttpServer();
static void beginScan();
static void beginConnect(const char* ssid, const char* pass);
static void enterSyncingState();
static void enterDoneState();

static void resetSyncTracking() {
  filesSent = 0;
  filesReceived = 0;
  totalFilesToSync = 0;
  lastHttpActivityMs = millis();
  syncCompletePending = false;
  pcConnected = false;
  syncLogCount = 0;
  for (int i = 0; i < MAX_LOG_LINES; i++) syncLog[i][0] = '\0';
}

int getSyncTotalFiles() { return totalFilesToSync; }

static void addSyncLogEntry(const char* fmt, const char* filename) {
  // Shift entries up if full
  if (syncLogCount >= MAX_LOG_LINES) {
    for (int i = 0; i < MAX_LOG_LINES - 1; i++) {
      strncpy(syncLog[i], syncLog[i + 1], sizeof(syncLog[i]) - 1);
      syncLog[i][sizeof(syncLog[i]) - 1] = '\0';
    }
    syncLogCount = MAX_LOG_LINES - 1;
  }
  snprintf(syncLog[syncLogCount], sizeof(syncLog[syncLogCount]), fmt, filename);
  syncLogCount++;
  screenDirty = true;
}

// =========================================================================
// SD card backup for WiFi credentials
// =========================================================================

static constexpr char WIFI_BACKUP_PATH[] = "/sticky/wifi.json";

static void writeWifiBackup() {
    static char buf[512];
    int count = wifiPrefs.getInt("wifi_count", 0);
    snprintf(buf, sizeof(buf), "{\"count\":%d", count);
    for (int i = 0; i < count && i < MAX_SAVED_NETWORKS; i++) {
        char sKey[32], pKey[32];
        snprintf(sKey, sizeof(sKey), "wifi_ssid_%d", i);
        snprintf(pKey, sizeof(pKey), "wifi_pass_%d", i);
        String ssid = wifiPrefs.getString(sKey, "");
        String pass = wifiPrefs.getString(pKey, "");
        char tmp[32];
        snprintf(tmp, sizeof(tmp), ",\"s%d\":\"", i);  strncat(buf, tmp, sizeof(buf) - strlen(buf) - 1);
        jsonAppendStr(buf, sizeof(buf), ssid.c_str());  strncat(buf, "\"", sizeof(buf) - strlen(buf) - 1);
        snprintf(tmp, sizeof(tmp), ",\"p%d\":\"", i);  strncat(buf, tmp, sizeof(buf) - strlen(buf) - 1);
        jsonAppendStr(buf, sizeof(buf), pass.c_str());  strncat(buf, "\"", sizeof(buf) - strlen(buf) - 1);
    }
    strncat(buf, "}", sizeof(buf) - strlen(buf) - 1);
    if (!SdMan.exists("/sticky")) SdMan.mkdir("/sticky");
    sdWriteFile(WIFI_BACKUP_PATH, buf);
}

static void restoreWifiBackup() {
    static char buf[512];
    if (!sdReadFile(WIFI_BACKUP_PATH, buf, sizeof(buf))) return;
    int count = jsonGetInt(buf, "count");
    if (count <= 0) return;
    for (int i = 0; i < count && i < MAX_SAVED_NETWORKS; i++) {
        char sKey[32], pKey[32], sNvs[32], pNvs[32];
        snprintf(sKey, sizeof(sKey), "s%d", i);
        snprintf(pKey, sizeof(pKey), "p%d", i);
        snprintf(sNvs, sizeof(sNvs), "wifi_ssid_%d", i);
        snprintf(pNvs, sizeof(pNvs), "wifi_pass_%d", i);
        char ssid[64] = "", pass[128] = "";
        jsonGetStr(buf, sKey, ssid, sizeof(ssid));
        jsonGetStr(buf, pKey, pass, sizeof(pass));
        if (ssid[0]) {
            wifiPrefs.putString(sNvs, ssid);
            wifiPrefs.putString(pNvs, pass);
        }
    }
    wifiPrefs.putInt("wifi_count", count);
    DBG_PRINTF("[SYNC] Restored %d WiFi credential(s) from SD backup\n", count);
}

// =========================================================================
// NVS credential storage
// =========================================================================

static void loadSavedCredentials() {
  // Mark networks that have saved passwords
  int count = wifiPrefs.getInt("wifi_count", 0);
  for (int i = 0; i < networkCount; i++) {
    networks[i].saved = false;
    for (int j = 0; j < count && j < MAX_SAVED_NETWORKS; j++) {
      char key[32];
      snprintf(key, sizeof(key), "wifi_ssid_%d", j);
      String savedSSID = wifiPrefs.getString(key, "");
      if (savedSSID.length() > 0 && strcmp(savedSSID.c_str(), networks[i].ssid) == 0) {
        networks[i].saved = true;
        break;
      }
    }
  }
}

static bool getSavedPassword(const char* ssid, char* passBuf, int passBufSize) {
  int count = wifiPrefs.getInt("wifi_count", 0);
  for (int i = 0; i < count && i < MAX_SAVED_NETWORKS; i++) {
    char sKey[32], pKey[32];
    snprintf(sKey, sizeof(sKey), "wifi_ssid_%d", i);
    snprintf(pKey, sizeof(pKey), "wifi_pass_%d", i);
    String savedSSID = wifiPrefs.getString(sKey, "");
    if (savedSSID.length() > 0 && strcmp(savedSSID.c_str(), ssid) == 0) {
      String savedPass = wifiPrefs.getString(pKey, "");
      strncpy(passBuf, savedPass.c_str(), passBufSize - 1);
      passBuf[passBufSize - 1] = '\0';
      return true;
    }
  }
  return false;
}

static void saveCredential(const char* ssid, const char* pass) {
  int count = wifiPrefs.getInt("wifi_count", 0);

  // Check if already saved — update in place
  for (int i = 0; i < count && i < MAX_SAVED_NETWORKS; i++) {
    char sKey[32], pKey[32];
    snprintf(sKey, sizeof(sKey), "wifi_ssid_%d", i);
    snprintf(pKey, sizeof(pKey), "wifi_pass_%d", i);
    String savedSSID = wifiPrefs.getString(sKey, "");
    if (savedSSID.length() > 0 && strcmp(savedSSID.c_str(), ssid) == 0) {
      wifiPrefs.putString(pKey, pass);
      return;
    }
  }

  // Add new entry (wrap around if full)
  int slot = count < MAX_SAVED_NETWORKS ? count : (count % MAX_SAVED_NETWORKS);
  char sKey[32], pKey[32];
  snprintf(sKey, sizeof(sKey), "wifi_ssid_%d", slot);
  snprintf(pKey, sizeof(pKey), "wifi_pass_%d", slot);
  wifiPrefs.putString(sKey, ssid);
  wifiPrefs.putString(pKey, pass);
  if (count < MAX_SAVED_NETWORKS) {
    wifiPrefs.putInt("wifi_count", count + 1);
  }
  writeWifiBackup();
}

static void forgetCredential(const char* ssid) {
  int count = wifiPrefs.getInt("wifi_count", 0);
  for (int i = 0; i < count && i < MAX_SAVED_NETWORKS; i++) {
    char sKey[32];
    snprintf(sKey, sizeof(sKey), "wifi_ssid_%d", i);
    String savedSSID = wifiPrefs.getString(sKey, "");
    if (savedSSID.length() > 0 && strcmp(savedSSID.c_str(), ssid) == 0) {
      // Shift remaining entries down
      for (int j = i; j < count - 1 && j < MAX_SAVED_NETWORKS - 1; j++) {
        char srcS[32], srcP[32], dstS[32], dstP[32];
        snprintf(srcS, sizeof(srcS), "wifi_ssid_%d", j + 1);
        snprintf(srcP, sizeof(srcP), "wifi_pass_%d", j + 1);
        snprintf(dstS, sizeof(dstS), "wifi_ssid_%d", j);
        snprintf(dstP, sizeof(dstP), "wifi_pass_%d", j);
        wifiPrefs.putString(dstS, wifiPrefs.getString(srcS, ""));
        wifiPrefs.putString(dstP, wifiPrefs.getString(srcP, ""));
      }
      // Clear last slot
      int lastIdx = count - 1;
      char lastS[32], lastP[32];
      snprintf(lastS, sizeof(lastS), "wifi_ssid_%d", lastIdx);
      snprintf(lastP, sizeof(lastP), "wifi_pass_%d", lastIdx);
      wifiPrefs.remove(lastS);
      wifiPrefs.remove(lastP);
      wifiPrefs.putInt("wifi_count", count - 1);
      writeWifiBackup();
      return;
    }
  }
}

// =========================================================================
// WiFi scanning
// =========================================================================

static void beginScan() {
  syncState = SyncState::SCANNING;
  strcpy(statusText, "Scanning...");
  networkCount = 0;
  selectedNet = 0;
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  WiFi.scanNetworks(true);  // async scan
  screenDirty = true;
  DBG_PRINTLN("[SYNC] WiFi scan started");
}

static void processScanResults() {
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return;  // Still scanning

  if (n <= 0) {
    // Scan failed or no networks found
    networkCount = 0;
    syncState = SyncState::NETWORK_LIST;
    strcpy(statusText, n == 0 ? "No networks found" : "Scan failed");
    WiFi.scanDelete();
    screenDirty = true;
    return;
  }

  // Deduplicate by SSID, keeping strongest signal
  networkCount = 0;
  for (int i = 0; i < n && networkCount < MAX_NETWORKS; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) continue;  // Skip hidden networks

    // Check for duplicate
    bool duplicate = false;
    for (int j = 0; j < networkCount; j++) {
      if (strcmp(networks[j].ssid, ssid.c_str()) == 0) {
        duplicate = true;
        if (WiFi.RSSI(i) > networks[j].rssi) {
          networks[j].rssi = WiFi.RSSI(i);
        }
        break;
      }
    }
    if (duplicate) continue;

    strncpy(networks[networkCount].ssid, ssid.c_str(), 32);
    networks[networkCount].ssid[32] = '\0';
    networks[networkCount].rssi = WiFi.RSSI(i);
    networks[networkCount].encrypted = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
    networks[networkCount].saved = false;
    networkCount++;
  }

  WiFi.scanDelete();

  // Mark saved networks
  loadSavedCredentials();

  // Sort: saved networks first, then by signal strength
  for (int i = 0; i < networkCount - 1; i++) {
    for (int j = i + 1; j < networkCount; j++) {
      bool swap = false;
      if (networks[j].saved && !networks[i].saved) {
        swap = true;
      } else if (networks[j].saved == networks[i].saved && networks[j].rssi > networks[i].rssi) {
        swap = true;
      }
      if (swap) {
        NetworkInfo tmp = networks[i];
        networks[i] = networks[j];
        networks[j] = tmp;
      }
    }
  }

  selectedNet = 0;
  syncState = SyncState::NETWORK_LIST;
  statusText[0] = '\0';
  screenDirty = true;
  DBG_PRINTF("[SYNC] Found %d networks\n", networkCount);

}

static void beginConnect(const char* ssid, const char* pass) {
  strncpy(connectingSSID, ssid, 32);
  connectingSSID[32] = '\0';
  syncState = SyncState::CONNECTING;
  snprintf(statusText, sizeof(statusText), "Connecting to %s...", ssid);
  connectStartMs = millis();

  WiFi.disconnect(true);
  delay(50);
  WiFi.begin(ssid, pass);
  screenDirty = true;
  DBG_PRINTF("[SYNC] Connecting to %s\n", ssid);
}

static void enterSyncingState() {
  resetSyncTracking();
  startHttpServer();
  snprintf(statusText, sizeof(statusText), "%s",
           WiFi.localIP().toString().c_str());
  syncState = SyncState::SYNCING;
  lastHttpActivityMs = millis();
  screenDirty = true;
  DBG_PRINTF("[SYNC] Syncing — server at %s\n", statusText);
}

static void enterDoneState() {
  stopHttpServer();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  syncState = SyncState::DONE;
  doneStartMs = millis();

  if (filesSent == 0 && filesReceived == 0) {
    strcpy(statusText, "No changes");
  } else {
    snprintf(statusText, sizeof(statusText), "Sent: %d  Received: %d",
             filesSent, filesReceived);
  }
  screenDirty = true;
  DBG_PRINTF("[SYNC] Done — %s\n", statusText);
}

static void pollConnection() {
  if (WiFi.status() == WL_CONNECTED) {
    // If we used a manually entered password, prompt to save first
    if (!usedSavedPassword) {
      syncState = SyncState::SAVE_PROMPT;
      snprintf(statusText, sizeof(statusText), "%s",
               WiFi.localIP().toString().c_str());
      screenDirty = true;
    } else {
      enterSyncingState();
    }
    return;
  }

  if (millis() - connectStartMs > 25000) {
    WiFi.disconnect(true);
    strcpy(statusText, "Connection failed");

    if (usedSavedPassword) {
      syncState = SyncState::FORGET_PROMPT;
    } else {
      syncState = SyncState::CONNECT_FAILED;
    }

    screenDirty = true;
    DBG_PRINTLN("[SYNC] Connection timed out");
  }
}


static const char FILE_MANAGER_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Sticky MicroNotes</title>
<style>
  :root { --ink:#161616; --paper:#f3efe6; --card:#fffcf6; --line:#d9d1c3; --mute:#6b655c; }
  * { box-sizing:border-box; }
  body { margin:0; background:var(--paper); color:var(--ink);
         font:16px/1.45 -apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif; }
  header { text-align:center; padding:36px 20px 18px; }
  header img { width:min(220px,70vw); height:auto; display:block; margin:0 auto 18px; }
  header h1 { margin:0; font-size:28px; font-weight:650; letter-spacing:.01em; }
  main { max-width:560px; margin:0 auto; padding:8px 20px 64px; }
  .panel { background:var(--card); border:1px solid var(--line); border-radius:16px; padding:16px; }
  .upload { display:flex; gap:10px; align-items:center; }
  .upload input { flex:1; min-width:0; }
  button, .btn {
    appearance:none; border:0; border-radius:10px; padding:10px 14px;
    font:inherit; font-weight:600; cursor:pointer; text-decoration:none; display:inline-block;
  }
  .primary { background:#1c1c1c; color:#fff; }
  .ghost { background:#efe8db; color:#1c1c1c; }
  .danger { background:#efe8db; color:#7a2e2e; }
  .list { margin-top:16px; display:flex; flex-direction:column; gap:10px; }
  .file { background:var(--card); border:1px solid var(--line); border-radius:16px; padding:14px 16px; }
  .name { font-weight:650; word-break:break-word; }
  .meta { color:var(--mute); font-size:13px; margin-top:3px; }
  .actions { display:flex; gap:8px; margin-top:12px; flex-wrap:wrap; }
  .actions .btn, .actions button { flex:1; text-align:center; min-width:90px; }
  .empty { text-align:center; color:var(--mute); padding:28px 8px; }
</style>
</head>
<body>
<header>
  <img alt="Sticky" src="data:image/png;base64,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">
  <h1>MicroNotes</h1>
</header>
<main>
  <div class="panel">
    <div class="upload">
      <input id="pick" type="file" accept=".txt,text/plain">
      <button class="primary" id="send">Upload</button>
    </div>
  </div>
  <div id="list" class="list"><div class="empty">Loading…</div></div>
</main>
<script>
const $ = id => document.getElementById(id);
function fmt(n){ return n < 1024 ? n+" B" : (n/1024).toFixed(1)+" KB"; }
async function refresh(){
  const files = await (await fetch("/api/files")).json();
  const el = $("list");
  if (!files.length) { el.innerHTML = '<div class="empty">No notes on this Sticky yet.</div>'; return; }
  el.innerHTML = files.map(f => `
    <div class="file">
      <div class="name">${f.name}</div>
      <div class="meta">${fmt(f.size)}</div>
      <div class="actions">
        <a class="btn ghost" href="/notes/${encodeURIComponent(f.name)}" target="_blank" rel="noopener">Open</a>
        <a class="btn ghost" href="/notes/${encodeURIComponent(f.name)}?dl=1" download="${f.name}">Download</a>
        <button class="danger" data-del="${f.name}">Delete</button>
      </div>
    </div>`).join("");
  el.querySelectorAll("[data-del]").forEach(b => b.onclick = () => del(b.dataset.del));
}
async function del(name){
  if (!confirm("Delete "+name+"?")) return;
  await fetch("/api/delete?name="+encodeURIComponent(name), {method:"POST"});
  refresh();
}
$("send").onclick = async () => {
  const f = $("pick").files[0];
  if (!f) return;
  const body = new FormData();
  body.append("file", f, f.name);
  $("send").disabled = true;
  await fetch("/upload", {method:"POST", body});
  $("send").disabled = false;
  $("pick").value = "";
  refresh();
};
refresh();
</script>
</body>
</html>
)HTML";


static void handleRoot() {
  lastHttpActivityMs = millis();
  if (!pcConnected) { pcConnected = true; screenDirty = true; }
  server->send_P(200, "text/html", FILE_MANAGER_HTML);
}

static bool safeNoteName(const String& name) {
  if (!name.length() || name.length() >= 60) return false;
  if (name.indexOf('/') >= 0 || name.indexOf('\\') >= 0 || name.indexOf("..") >= 0) return false;
  return true;
}

static void handleUpload() {
  HTTPUpload& up = server->upload();
  static char path[96];
  if (up.status == UPLOAD_FILE_START) {
    String name = up.filename;
    int slash = name.lastIndexOf('/');
    if (slash >= 0) name = name.substring(slash + 1);
    if (!safeNoteName(name)) return;
    if (!name.endsWith(".txt") && !name.endsWith(".TXT")) name += ".txt";
    if (!SdMan.exists("/notes")) SdMan.mkdir("/notes");
    snprintf(path, sizeof(path), "/notes/%s", name.c_str());
    auto f = SdMan.open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (f) f.close();
  } else if (up.status == UPLOAD_FILE_WRITE) {
    auto f = SdMan.open(path, O_WRONLY | O_APPEND);
    if (f) {
      f.write(up.buf, up.currentSize);
      f.close();
    }
  } else if (up.status == UPLOAD_FILE_END) {
    filesReceived++;
    lastHttpActivityMs = millis();
    pcConnected = true;
    refreshFileList();
    screenDirty = true;
  }
}

static void handleUploadDone() {
  server->send(200, "text/plain", "OK");
}

static void handleDelete() {
  lastHttpActivityMs = millis();
  String name = server->arg("name");
  if (!safeNoteName(name)) { server->send(400, "text/plain", "bad name"); return; }
  char path[96];
  snprintf(path, sizeof(path), "/notes/%s", name.c_str());
  if (SdMan.exists(path)) SdMan.remove(path);
  refreshFileList();
  server->send(200, "text/plain", "OK");
}


static void handleFileList() {
  lastHttpActivityMs = millis();
  if (!pcConnected) {
    pcConnected = true;
    screenDirty = true;
  }

  auto dir = SdMan.open("/notes");
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    server->send(500, "application/json", "[]");
    return;
  }

  String json = "[";
  bool first = true;
  char name[256];

  dir.rewindDirectory();
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    file.getName(name, sizeof(name));
    if (name[0] == '.') { file.close(); continue; }

    int nameLen = strlen(name);
    if (nameLen > 4 && strcmp(name + nameLen - 4, ".txt") == 0) {
      if (!first) json += ",";
      first = false;
      json += "{\"name\":\"";
      json += name;
      json += "\",\"size\":";
      json += String((unsigned long)file.size());
      json += "}";
    }
    file.close();
  }
  dir.close();

  json += "]";
  server->send(200, "application/json", json);
}

static void handleFileDownload() {
  lastHttpActivityMs = millis();

  String uri = server->uri();
  if (!uri.startsWith("/notes/") || uri.length() <= 7) {
    server->send(400, "text/plain", "Bad request");
    return;
  }

  String filename = uri.substring(7);
  char path[320];
  snprintf(path, sizeof(path), "/notes/%s", filename.c_str());

  auto file = SdMan.open(path, O_RDONLY);
  if (!file) {
    server->send(404, "text/plain", "Not found");
    return;
  }

  size_t fileSize = file.size();
  if (server->hasArg("dl")) {
    String disp = "attachment; filename=\"" + filename + "\"";
    server->sendHeader("Content-Disposition", disp);
    server->sendHeader("Content-Type", "text/plain");
  }
  server->setContentLength(fileSize);
  server->send(200, "text/plain", "");

  uint8_t buf[512];
  while (file.available()) {
    int bytesRead = file.read(buf, sizeof(buf));
    if (bytesRead <= 0) break;
    server->client().write(buf, bytesRead);
  }
  file.close();

  // Track: PC downloaded a file from device = "sent"
  filesSent++;
  screenDirty = true;
  DBG_PRINTF("[SYNC] Sent file: %s\n", filename.c_str());
}

static void handleSyncComplete() {
  lastHttpActivityMs = millis();
  server->send(200, "text/plain", "OK");
  DBG_PRINTLN("[SYNC] PC signaled sync complete");
  syncCompletePending = true;  // enterDoneState() called from wifiSyncLoop, not here
}

static void handleNotFound() {
  String uri = server->uri();

  if (uri.startsWith("/notes/") && uri.length() > 7 && server->method() == HTTP_GET) {
    handleFileDownload();
    return;
  }

  server->send(404, "text/plain", "Not found");
}

static void startHttpServer() {
  if (server) return;
  server = new WebServer(80);
  server->on("/", HTTP_GET, handleRoot);
  server->on("/api/files", HTTP_GET, handleFileList);
  server->on("/api/delete", HTTP_POST, handleDelete);
  server->on("/upload", HTTP_POST, handleUploadDone, handleUpload);
  server->on("/api/sync-complete", HTTP_POST, handleSyncComplete);
  server->onNotFound(handleNotFound);
  server->begin();
  MDNS.begin("sticky");
  DBG_PRINTF("[SYNC] HTTP server started at %s\n", WiFi.localIP().toString().c_str());
}

static void stopHttpServer() {
  if (server) {
    server->stop();
    delete server;
    server = nullptr;
  }
  MDNS.end();
}

// =========================================================================
// Input handling — called from input_handler for all key events
// =========================================================================

void syncHandleKey(uint8_t keyCode, uint8_t modifiers) {
  switch (syncState) {
    case SyncState::SCANNING:
      // No input during scan
      if (keyCode == HID_KEY_ESCAPE) {
        wifiSyncStop();
      }
      break;

    case SyncState::NETWORK_LIST:
      if (keyCode == HID_KEY_DOWN && networkCount > 0) {
        selectedNet = (selectedNet + 1) % networkCount;
        screenDirty = true;
      } else if (keyCode == HID_KEY_UP && networkCount > 0) {
        selectedNet = (selectedNet - 1 + networkCount) % networkCount;
        screenDirty = true;
      } else if (keyCode == HID_KEY_ENTER && networkCount > 0) {
        // Try saved password first
        char savedPass[MAX_PASSWORD_LEN + 1];
        if (getSavedPassword(networks[selectedNet].ssid, savedPass, sizeof(savedPass))) {
          usedSavedPassword = true;
          autoConnectAttempted = false;
          beginConnect(networks[selectedNet].ssid, savedPass);
        } else if (!networks[selectedNet].encrypted) {
          // Open network — connect directly
          usedSavedPassword = false;
          autoConnectAttempted = false;
          beginConnect(networks[selectedNet].ssid, "");
        } else {
          // Need password
          usedSavedPassword = false;
          autoConnectAttempted = false;
          passwordBuf[0] = '\0';
          passwordLen = 0;
          syncState = SyncState::PASSWORD_ENTRY;
          screenDirty = true;
        }
      } else if (keyCode == HID_KEY_ESCAPE) {
        wifiSyncStop();
      }
      break;

    case SyncState::PASSWORD_ENTRY:
      if (keyCode == HID_KEY_ENTER) {
        if (passwordLen > 0) {
          beginConnect(networks[selectedNet].ssid, passwordBuf);
        }
      } else if (keyCode == HID_KEY_ESCAPE) {
        syncState = SyncState::NETWORK_LIST;
        screenDirty = true;
      } else if (keyCode == HID_KEY_BACKSPACE) {
        if (passwordLen > 0) {
          passwordLen--;
          passwordBuf[passwordLen] = '\0';
          screenDirty = true;
        }
      } else {
        // Printable character — reuse hidToAscii from input_handler
        extern char hidToAscii(uint8_t hid, uint8_t modifiers);
        char c = hidToAscii(keyCode, modifiers);
        if (c != 0 && c >= ' ' && c != '\n' && c != '\t' && passwordLen < MAX_PASSWORD_LEN) {
          passwordBuf[passwordLen++] = c;
          passwordBuf[passwordLen] = '\0';
          screenDirty = true;
        }
      }
      break;

    case SyncState::CONNECTING:
      // No input while connecting (timeout handles failure)
      if (keyCode == HID_KEY_ESCAPE) {
        WiFi.disconnect(true);
        if (autoConnectAttempted) {
          // Was auto-connecting — fall back to scan
          beginScan();
        } else {
          syncState = SyncState::NETWORK_LIST;
          screenDirty = true;
        }
      }
      break;

    case SyncState::SYNCING:
      if (keyCode == HID_KEY_ESCAPE) {
        wifiSyncStop();
      }
      break;

    case SyncState::DONE:
      // Any key press returns to menu immediately
      wifiSyncStop();
      break;

    case SyncState::CONNECT_FAILED:
      if (keyCode == HID_KEY_ENTER) {
        // Back to network list, re-scan
        beginScan();
      } else if (keyCode == HID_KEY_ESCAPE) {
        wifiSyncStop();
      }
      break;

    case SyncState::SAVE_PROMPT:
      // Up = Yes (save), Down = No (skip)
      if (keyCode == HID_KEY_UP || keyCode == HID_KEY_ENTER) {
        saveCredential(connectingSSID, passwordBuf);
        DBG_PRINTF("[SYNC] Saved credentials for %s\n", connectingSSID);
        enterSyncingState();
      } else if (keyCode == HID_KEY_DOWN || keyCode == HID_KEY_ESCAPE) {
        enterSyncingState();
      }
      break;

    case SyncState::FORGET_PROMPT:
      // Up = Yes (forget), Down = No (keep)
      if (keyCode == HID_KEY_UP || keyCode == HID_KEY_ENTER) {
        forgetCredential(connectingSSID);
        DBG_PRINTF("[SYNC] Forgot credentials for %s\n", connectingSSID);
        beginScan();
      } else if (keyCode == HID_KEY_DOWN || keyCode == HID_KEY_ESCAPE) {
        // Keep credentials — go to network list so user can retry manually
        // rather than triggering auto-connect again immediately
        syncState = SyncState::NETWORK_LIST;
        screenDirty = true;
      }
      break;
  }
}

// =========================================================================
// Public API
// =========================================================================

void wifiSyncStart() {
  if (syncActive) return;
  syncActive = true;
  wifiPrefs.begin("wifi_creds", false);
  if (!wifiPrefs.isKey("wifi_count")) restoreWifiBackup();
  resetSyncTracking();

  beginScan();

  DBG_PRINTLN("[SYNC] WiFi sync started");
}

void wifiSyncStop() {
  if (!syncActive) return;

  stopHttpServer();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  wifiPrefs.end();
  syncActive = false;
  networkCount = 0;
  passwordBuf[0] = '\0';
  passwordLen = 0;
  statusText[0] = '\0';

  // Return to main menu
  extern UIState currentState;
  currentState = UIState::MAIN_MENU;
  screenDirty = true;

  DBG_PRINTLN("[SYNC] WiFi sync stopped");
}

void wifiSyncLoop() {
  if (!syncActive) return;

  switch (syncState) {
    case SyncState::SCANNING:
      processScanResults();
      break;

    case SyncState::CONNECTING:
      pollConnection();
      break;

    case SyncState::SYNCING:
      if (server) server->handleClient();
      if (syncCompletePending) {
        syncCompletePending = false;
        enterDoneState();
      } else if (millis() - lastHttpActivityMs > SYNC_TIMEOUT_MS) {
        DBG_PRINTLN("[SYNC] Timeout — no HTTP activity for 60s");
        enterDoneState();
      }
      break;

    case SyncState::DONE:
      // Auto-return to menu after 3 seconds
      if (millis() - doneStartMs > DONE_DISPLAY_MS) {
        wifiSyncStop();
      }
      break;

    case SyncState::SAVE_PROMPT:
      // Server is NOT running during save prompt (will start after user responds)
      break;

    default:
      break;
  }
}

bool isWifiSyncActive() {
  return syncActive;
}

SyncState getSyncState() {
  return syncState;
}

int getNetworkCount() {
  return networkCount;
}

const char* getNetworkSSID(int i) {
  if (i < 0 || i >= networkCount) return "";
  return networks[i].ssid;
}

int getNetworkRSSI(int i) {
  if (i < 0 || i >= networkCount) return -100;
  return networks[i].rssi;
}

bool isNetworkEncrypted(int i) {
  if (i < 0 || i >= networkCount) return false;
  return networks[i].encrypted;
}

bool isNetworkSaved(int i) {
  if (i < 0 || i >= networkCount) return false;
  return networks[i].saved;
}

int getSelectedNetwork() {
  return selectedNet;
}

const char* getPasswordBuffer() {
  return passwordBuf;
}

int getPasswordLen() {
  return passwordLen;
}

const char* getSyncStatusText() {
  return statusText;
}

int getSyncFilesSent() {
  return filesSent;
}

int getSyncFilesReceived() {
  return filesReceived;
}

bool isPcConnected() {
  return pcConnected;
}
