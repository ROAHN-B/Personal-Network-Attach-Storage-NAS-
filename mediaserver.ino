#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <SD.h>
#include <SPI.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <rickmoo_qrcode.h>
#include <memory>
#include <vector>

struct PageGenState {
  int step = 0;            // 0=head/topbar/grid-open, 1=folder loop, 2=tail, 3=done
  int folderIdx = 0;       // which folder we're currently listing
  bool folderOpened = false;
  bool folderAny = false;  // did the current folder have any files?
  File manifestFile;       // currently-open SD manifest file handle
  String pending;          // leftover text not yet flushed into the network buffer
};

volatile bool syncRequested = false;
String lastSyncResult = "No sync performed yet.";

volatile bool pushRequested = false;
String pushPath = "";
String lastPushResult = "No push performed yet.";

// ---------- CONFIG: EDIT THESE ----------
//NOTE:Frequency of WIFI should be at 2.4GHz
const char* WIFI_SSID     = "Redmi 12 5G";
const char* WIFI_PASSWORD = "123456789";
const char* PREVIEW_KEY = "nasPreviewKey987" ;
const char* HOSTNAME      = "mediaserver";   

#define SD_CS_PIN   5
#define SD_MOSI_PIN 23
#define SD_MISO_PIN 19
#define SD_SCK_PIN  18
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// I2C Pins
#define OLED_SDA 21
#define OLED_SCL 22

String ngrokURL = "https://media-server.app";

// Authentication — change these before flashing!
const char* AUTH_USERNAME = "Admin";
const char* AUTH_PASSWORD = "rohan123";

// Google Drive — from get_refres_token.py output. Treat as secrets: don't share/commit.
const char* DRIVE_CLIENT_ID     ="791272166374-b9fusoesvns0kl2uj3fvt3g38op7vicf.apps.googleusercontent.com";
const char* DRIVE_CLIENT_SECRET ="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";
const char* DRIVE_REFRESH_TOKEN ="1//0g_Su_AynAq1_CgYIARAAGBASNwF-L9Irw2tvSJS98-VGQ1eANvo47AbpdfaVJ187sCf-x0M_wjwt2OwsMlCFZjHBzESUyEfLprY";
const char* DRIVE_FOLDER_ID     ="1mTjQBYJD1DFc3kTTAXq3iaFaaMbVkaAZ";
// -----------------------------------------

AsyncWebServer server(80);
String driveAccessToken = "";
const char* SYNCED_FOLDER = "/synced";

// Dynamic Folder Management
std::vector<String> activeFolders;

void saveFolderManifest() {
  File f = SD.open("/.folders.txt", FILE_WRITE);
  if (f) {
    for (String folder : activeFolders) {
      f.println(folder);
    }
    f.close();
  }
}

void loadFolderManifest() {
  activeFolders.clear();
  File f = SD.open("/.folders.txt", FILE_READ);
  if (f) {
    while (f.available()) {
      String line = f.readStringUntil('\n');
      line.trim();
      if (line.length() > 0) activeFolders.push_back(line);
    }
    f.close();
  }
  // Ensure base folders exist if manifest is missing/empty
  if (activeFolders.empty()) {
    activeFolders.push_back("/documents");
    activeFolders.push_back("/media/audio");
    activeFolders.push_back("/media/video");
    activeFolders.push_back("/synced");
    saveFolderManifest();
  }
}

void ensureFolders() {
  for (size_t i = 0; i < activeFolders.size(); i++) {
    if (!SD.exists(activeFolders[i])) {
      // Create nested folders one level at a time
      String path = activeFolders[i];
      int idx = 1;
      while (idx > 0) {
        idx = path.indexOf('/', idx);
        String sub = (idx == -1) ? path : path.substring(0, idx);
        if (sub.length() > 0 && !SD.exists(sub)) {
          SD.mkdir(sub);
        }
        if (idx != -1) idx++;
      }
    }
  }
}

// Re-initializes the SD card connection.
bool remountSD() {
  SD.end();
  delay(300); // give the card a moment to settle
  bool ok = SD.begin(SD_CS_PIN);
  if (ok) {
    loadFolderManifest();
    ensureFolders();
    Serial.println("SD card remounted successfully.");
  } else {
    Serial.println("SD remount failed — check the card is seated properly.");
  }
  return ok;
}

// ================= MANIFEST HELPER FUNCTIONS =================

void appendToManifest(const String& folder, const String& filename) {
  String manifestPath = folder + "/.manifest.txt";
  bool existsInManifest = false;
  
  File mRead = SD.open(manifestPath);
  if (mRead) {
    while (mRead.available()) {
      String line = mRead.readStringUntil('\n');
      line.trim();
      if (line == filename) {
        existsInManifest = true;
        break;
      }
    }
    mRead.close();
  }

  if (!existsInManifest) {
    File mWrite = SD.open(manifestPath, FILE_APPEND);
    if (mWrite) {
      mWrite.println(filename);
      mWrite.close();
    }
  }
}

void removeFromManifest(const String& folder, const String& filename) {
  String manifestPath = folder + "/.manifest.txt";
  String tempPath = folder + "/.manifest_tmp.txt";
  
  File m = SD.open(manifestPath, FILE_READ);
  File t = SD.open(tempPath, FILE_WRITE);
  
  if (m && t) {
    while (m.available()) {
      String line = m.readStringUntil('\n');
      line.trim();
      if (line.length() > 0 && line != filename && line != ".manifest.txt") {
        t.println(line);
      }
    }
    m.close();
    t.close();
    SD.remove(manifestPath);
    SD.rename(tempPath, manifestPath);
  }
}

void renameInManifest(const String& folder, const String& oldName, const String& newName) {
  String manifestPath = folder + "/.manifest.txt";
  String tempPath = folder + "/.manifest_tmp.txt";
  
  File m = SD.open(manifestPath, FILE_READ);
  File t = SD.open(tempPath, FILE_WRITE);
  
  if (m && t) {
    while (m.available()) {
      String line = m.readStringUntil('\n');
      line.trim();
      if (line.length() > 0) {
        if (line == oldName) {
          t.println(newName);
        } else {
          t.println(line);
        }
      }
    }
    m.close();
    t.close();
    SD.remove(manifestPath);
    SD.rename(tempPath, manifestPath);
  }
}

// ================= END MANIFEST HELPER FUNCTIONS =================

String getFileIcon(const String& name) {
  String n = name;
  n.toLowerCase();
  if (n.endsWith(".mp3") || n.endsWith(".wav") || n.endsWith(".flac") || n.endsWith(".m4a") || n.endsWith(".aac")) return "&#127925;";
  if (n.endsWith(".mp4") || n.endsWith(".mkv") || n.endsWith(".avi") || n.endsWith(".mov") || n.endsWith(".webm")) return "&#127916;";
  if (n.endsWith(".pdf")) return "&#128196;";
  if (n.endsWith(".doc") || n.endsWith(".docx") || n.endsWith(".txt")) return "&#128221;";
  if (n.endsWith(".jpg") || n.endsWith(".jpeg") || n.endsWith(".png") || n.endsWith(".gif") || n.endsWith(".webp")) return "&#128444;";
  if (n.endsWith(".zip") || n.endsWith(".rar") || n.endsWith(".7z")) return "&#128230;";
  return "&#128196;";
}

String getFileKind(const String& name) {
  String n = name;
  n.toLowerCase();
  if (n.endsWith(".mp4") || n.endsWith(".mkv") || n.endsWith(".webm") || n.endsWith(".mov")) return "video";
  if (n.endsWith(".mp3") || n.endsWith(".wav") || n.endsWith(".flac") || n.endsWith(".m4a") || n.endsWith(".aac") || n.endsWith(".ogg")) return "audio";
  if (n.endsWith(".pdf")) return "pdf";
  if (n.endsWith(".jpg") || n.endsWith(".jpeg") || n.endsWith(".png") || n.endsWith(".gif") || n.endsWith(".webp") || n.endsWith(".bmp")) return "image";
  if (n.endsWith(".txt")) return "text";
  if (n.endsWith(".docx")) return "docx";
  return "other";
}
String escapeForJsAttr(const String& s) {
  String out = s;
  out.replace("\\", "\\\\");
  out.replace("'", "\\'");
  return out;
}

String htmlEscape(const String& s) {
  String out;
  out.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s.charAt(i);
    switch (c) {
      case '&':  out += "&amp;";  break;
      case '<':  out += "&lt;";   break;
      case '>':  out += "&gt;";   break;
      case '"':  out += "&quot;"; break;
      case '\'': out += "&#39;";  break;
      default:   out += c;
    }
  }
  return out;
}

String getMimeForPath(const String& path) {
  String p = path;
  p.toLowerCase();
  
  // Documents & Images
  if (p.endsWith(".pdf")) return "application/pdf";
  if (p.endsWith(".txt")) return "text/plain";
  if (p.endsWith(".jpg") || p.endsWith(".jpeg")) return "image/jpeg";
  if (p.endsWith(".png")) return "image/png";
  if (p.endsWith(".gif")) return "image/gif";
  if (p.endsWith(".webp")) return "image/webp";
  if (p.endsWith(".html") || p.endsWith(".htm")) return "text/html";
  if (p.endsWith(".css")) return "text/css";
  
  // Audio
  if (p.endsWith(".mp3")) return "audio/mpeg";
  if (p.endsWith(".wav")) return "audio/wav";
  if (p.endsWith(".flac")) return "audio/flac";
  if (p.endsWith(".m4a")) return "audio/mp4";
  if (p.endsWith(".aac")) return "audio/aac";
  if (p.endsWith(".ogg")) return "audio/ogg";
  
  // Video
  if (p.endsWith(".mp4")) return "video/mp4";
  if (p.endsWith(".mkv")) return "video/x-matroska";
  if (p.endsWith(".webm")) return "video/webm";
  if (p.endsWith(".mov")) return "video/quicktime";
  
  return "application/octet-stream";
}

String getFileSizeStr(size_t bytes) {
  float kb = bytes / 1024.0;
  if (kb >= 1024.0) {
    return String(kb / 1024.0, 1) + " MB";
  }
  return String((int)kb) + " KB";
}

String getFolderDisplayName(const String& folderPath) {
  if (folderPath == "/documents") return "Documents";
  if (folderPath == "/media/audio") return "Music";
  if (folderPath == "/media/video") return "Videos";
  if (folderPath == "/synced") return "Synced (Drive)";
  
  // For custom folders, show the capitalized base name
  String name = folderPath;
  int lastSlash = name.lastIndexOf('/');
  if (lastSlash != -1 && lastSlash < name.length() - 1) {
    name = name.substring(lastSlash + 1);
  }
  return name;
}

// ================= GOOGLE DRIVE SYNC =================

bool refreshAccessToken() {
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.begin(client, "https://oauth2.googleapis.com/token");
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");

  String body = "client_id=" + String(DRIVE_CLIENT_ID) +
                "&client_secret=" + String(DRIVE_CLIENT_SECRET) +
                "&refresh_token=" + String(DRIVE_REFRESH_TOKEN) +
                "&grant_type=refresh_token";

  yield();
  delay(1);
  int code = http.POST(body);
  yield();
  delay(1);
  String payload = http.getString();
  yield();
  delay(1);
  http.end();

  if (code != 200) {
    Serial.println("Token refresh failed, HTTP " + String(code));
    Serial.println("Response: " + payload);
    return false;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.println("Token JSON parse failed: " + String(err.c_str()));
    Serial.println("Raw response was: " + payload);
    return false;
  }

  driveAccessToken = doc["access_token"].as<String>();
  return driveAccessToken.length() > 0;
}

bool downloadDriveFile(const String& fileId, const String& filename) {
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  String url = "https://www.googleapis.com/drive/v3/files/" + fileId + "?alt=media";
  http.begin(client, url);
  http.addHeader("Authorization", "Bearer " + driveAccessToken);

  yield();
  delay(1);
  int code = http.GET();
  yield();
  delay(1);
  if (code != 200) {
    Serial.println("Download failed for " + filename + ", HTTP " + String(code));
    http.end();
    return false;
  }

  String path = String(SYNCED_FOLDER) + "/" + filename;
  if (SD.exists(path)) {
    SD.remove(path);
  }
  File f = SD.open(path, FILE_WRITE);
  if (!f) {
    Serial.println("Could not open file for writing: " + path);
    http.end();
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  uint8_t buf[512];
  int total = http.getSize();
  int written = 0;
  while (http.connected() && (total < 0 || written < total)) {
    yield();
    delay(1);
    size_t avail = stream->available();
    if (avail) {
      int toRead = min((int)avail, (int)sizeof(buf));
      int readBytes = stream->readBytes(buf, toRead);
      f.write(buf, readBytes);
      written += readBytes;
    } else {
      delay(1);
    }
    if (total > 0 && written >= total) break;
  }
  f.close();
  http.end();
  Serial.println("Downloaded: " + filename + " (" + String(written) + " bytes)");
  
  appendToManifest(String(SYNCED_FOLDER), filename);
  
  return true;
}

bool uploadFileToDrive(const String& localPath) {
  File f = SD.open(localPath, FILE_READ);
  if (!f) {
    Serial.println("Could not open local file: " + localPath);
    return false;
  }

  if (driveAccessToken.length() == 0) {
    Serial.println("No access token!");
    f.close();
    return false;
  }

  size_t fileSize = f.size();

  String filename = localPath;
  int slash = filename.lastIndexOf('/');
  if (slash != -1) filename = filename.substring(slash + 1);
  filename.trim();
  filename.replace(" ", "_");

  String sessionUrl;

  {
    WiFiClientSecure client1;
    client1.setInsecure();

    HTTPClient http1;
    http1.begin(client1, "https://www.googleapis.com/upload/drive/v3/files?uploadType=resumable");
    http1.addHeader("Authorization", "Bearer " + driveAccessToken);
    http1.addHeader("Content-Type", "application/json; charset=UTF-8");

    const char* headersToCollect[] = {"Location"};
    http1.collectHeaders(headersToCollect, 1);

    JsonDocument meta;
    meta["name"] = filename;
    JsonArray parents = meta["parents"].to<JsonArray>();
    parents.add(DRIVE_FOLDER_ID);

    String metaBody;
    serializeJson(meta, metaBody);

    int code1 = http1.POST(metaBody);
    sessionUrl = http1.header("Location");
    http1.end();

    if (code1 != 200 || sessionUrl.length() == 0) {
      Serial.println("Failed to start upload session, HTTP " + String(code1));
      f.close();
      return false;
    }
  }

  yield();
  delay(100);

  String urlNoProtocol = sessionUrl;
  urlNoProtocol.replace("https://", "");
  int slashIdx = urlNoProtocol.indexOf('/');
  String host = urlNoProtocol.substring(0, slashIdx);
  String path = urlNoProtocol.substring(slashIdx);

  WiFiClientSecure client2;
  client2.setInsecure();

  if (!client2.connect(host.c_str(), 443)) {
    Serial.println("Failed to connect to upload session host.");
    f.close();
    return false;
  }

  String reqHeader = "PUT " + path + " HTTP/1.1\r\n";
  reqHeader += "Host: " + host + "\r\n";
  reqHeader += "Authorization: Bearer " + driveAccessToken + "\r\n";
  reqHeader += "Content-Type: application/octet-stream\r\n";
  reqHeader += "Content-Length: " + String(fileSize) + "\r\n";
  reqHeader += "Connection: close\r\n\r\n";
  client2.print(reqHeader);

  f.seek(0);
  uint8_t buf[512];
  size_t sent = 0;
  while (sent < fileSize) {
    yield();
    delay(1);
    size_t toRead = min((size_t)sizeof(buf), fileSize - sent);
    size_t readBytes = f.read(buf, toRead);
    if (readBytes == 0) break;
    client2.write(buf, readBytes);
    sent += readBytes;
  }
  f.close();

  unsigned long waitStart = millis();
  while (client2.connected() && !client2.available()) {
    if (millis() - waitStart > 15000) break;
    yield();
    delay(1);
  }
  String statusLine = client2.readStringUntil('\n');
  client2.stop();

  Serial.println("Upload response status: " + statusLine);

  bool success = statusLine.indexOf("200") != -1 || statusLine.indexOf("201") != -1;
  if (success) {
    Serial.println("Uploaded: " + filename);
  } else {
    Serial.println("Upload failed: " + filename);
  }
  return success;
}

String syncWithDrive() {
  if (WiFi.status() != WL_CONNECTED) {
    return "No Wi-Fi connection.";
  }

  if (!refreshAccessToken()) {
    return "Failed to get Drive access token.";
  }

  Serial.println("\n========== ACCESS TOKEN OK ==========\n");

  {
    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    http.begin(client,
               "https://www.googleapis.com/drive/v3/about?fields=user");
    http.addHeader("Authorization",
                   "Bearer " + driveAccessToken);
    yield();
    delay(1);
    int code = http.GET();
    yield();
    delay(1);

    Serial.println("===== GOOGLE ACCOUNT =====");
    Serial.println("HTTP Code: " + String(code));
    Serial.println(http.getString());
    Serial.println("==========================");

    http.end();
  }

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  String url =
    "https://www.googleapis.com/drive/v3/files?"
    "q=%27" +
    String(DRIVE_FOLDER_ID) +
    "%27%20in%20parents%20and%20trashed=false"
    "&pageSize=1000"
    "&fields=files(id,name,size)";

  Serial.println("\n===== FOLDER QUERY =====");
  Serial.println(url);

  http.begin(client, url);
  http.addHeader("Authorization",
                 "Bearer " + driveAccessToken);
  yield();
  delay(1);
  int code = http.GET();
  yield();
  delay(1);
  String payload = http.getString();
  yield();
  delay(1);

  Serial.println("\n===== FOLDER RESULT =====");
  Serial.println("HTTP Code: " + String(code));
  Serial.println(payload);
  yield();
  delay(1);
  Serial.println("=========================");

  http.end();

  if (code != 200) {
    return "Failed to list Drive files: HTTP " +
           String(code);
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);

  if (err) {
    Serial.println(err.c_str());
    return "JSON parse failed.";
  }

  JsonArray files = doc["files"].as<JsonArray>();

  Serial.println(
      "Files found in folder: " +
      String(files.size()));

  int downloaded = 0;
  int uploaded = 0;

  for (JsonObject file : files) {
    yield();
    delay(1);
    String name = file["name"].as<String>();
    String id = file["id"].as<String>();

    Serial.println("Drive file: " + name);

    String localPath =
      String(SYNCED_FOLDER) + "/" + name;

    if (!SD.exists(localPath)) {
      if (downloadDriveFile(id, name))
        downloaded++;
    }
  }

  // Iterate local manifest instead of directory for sync upload check
  String manifestPath = String(SYNCED_FOLDER) + "/.manifest.txt";
  File manifestFile = SD.open(manifestPath);

  if (manifestFile) {
    while (manifestFile.available()) {
      yield();
      delay(1);
      String name = manifestFile.readStringUntil('\n');
      name.trim();
      if (name.length() == 0 || name == ".manifest.txt") continue;

      bool exists = false;
      for (JsonObject file : files) {
        if (file["name"].as<String>() == name) {
          exists = true;
          break;
        }
      }

      if (!exists) {
        String fullPath = String(SYNCED_FOLDER) + "/" + name;
        if (uploadFileToDrive(fullPath))
          uploaded++;
      }
    }
    manifestFile.close();
  }

  return "Sync complete. Downloaded "
         + String(downloaded)
         + ", Uploaded "
         + String(uploaded);
}

// Shared CSS for all pages. Purely cosmetic — no behavior here.
String getPageStyle() {
  return
    "<style>"
    ":root{"
      "--bg:#0f1420;--bg-soft:#161d2e;--card:#1b2338;--card-border:#2a3450;"
      "--text:#eef1f8;--text-dim:#9aa4bd;--accent:#5b8cff;--accent-soft:#26365f;"
      "--danger:#ff6b6b;--radius:14px;"
    "}"
    "*{box-sizing:border-box;}"
    "body{margin:0;background:var(--bg);color:var(--text);font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,Helvetica,Arial,sans-serif;-webkit-font-smoothing:antialiased;}"
    ".topbar{position:sticky;top:0;z-index:10;background:rgba(15,20,32,0.92);backdrop-filter:blur(8px);border-bottom:1px solid var(--card-border);padding:14px 20px;display:flex;align-items:center;justify-content:space-between;gap:12px;flex-wrap:wrap;}"
    ".topbar h1{font-size:1.15em;margin:0;display:flex;align-items:center;gap:8px;}"
    ".topbar .actions{display:flex;gap:8px;flex-wrap:wrap;}"
    ".container{max-width:960px;margin:0 auto;padding:20px;}"
    ".search-form{display:flex;gap:8px;margin-bottom:20px;}"
    ".search-form input[type=text]{flex:1;padding:11px 14px;border-radius:10px;border:1px solid var(--card-border);background:var(--bg-soft);color:var(--text);font-size:0.95em;}"
    ".search-form input[type=text]:focus{outline:2px solid var(--accent);}"
    "button,.btn{cursor:pointer;border:none;border-radius:10px;padding:10px 16px;font-size:0.9em;font-weight:600;background:var(--accent);color:#fff;text-decoration:none;display:inline-flex;align-items:center;gap:6px;transition:opacity .15s ease;}"
    "button:hover,.btn:hover{opacity:0.85;}"
    ".btn-secondary{background:var(--bg-soft);border:1px solid var(--card-border);color:var(--text);}"
    ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(300px,1fr));gap:16px;margin-bottom:20px;}"
    ".card{background:var(--card);border:1px solid var(--card-border);border-radius:var(--radius);overflow:hidden;}"
    ".card-header{padding:14px 16px;border-bottom:1px solid var(--card-border);display:flex;align-items:baseline;justify-content:space-between;gap:10px;}"
    ".card-header h2{font-size:1em;margin:0;}"
    ".card-path{font-size:0.75em;color:var(--text-dim);font-family:monospace;}"
    ".file-list{max-height:340px;overflow-y:auto;}"
    ".file-row{display:flex;align-items:center;gap:10px;padding:10px 16px;border-bottom:1px solid var(--card-border);}"
    ".file-row:last-child{border-bottom:none;}"
    ".file-row:hover{background:var(--bg-soft);}"
    ".file-name{flex:1;min-width:0;display:flex;align-items:center;gap:8px;color:var(--text);text-decoration:none;font-size:0.92em;}"
    ".file-name:hover{color:var(--accent);}"
    ".file-name-text{overflow:hidden;text-overflow:ellipsis;white-space:nowrap;}"
    ".file-icon{flex-shrink:0;}"
    ".file-size{flex-shrink:0;color:var(--text-dim);font-size:0.78em;min-width:56px;text-align:right;}"
    ".file-actions{flex-shrink:0;display:flex;gap:4px;}"
    ".btn-icon{width:30px;height:30px;display:flex;align-items:center;justify-content:center;border-radius:8px;background:var(--bg-soft);color:var(--text-dim);text-decoration:none;font-size:0.85em;border:1px solid var(--card-border);}"
    ".btn-icon:hover{color:var(--text);border-color:var(--accent);}"
    ".btn-danger:hover{color:var(--danger);border-color:var(--danger);}"
    ".empty-state{padding:24px 16px;text-align:center;color:var(--text-dim);font-size:0.9em;}"
    ".panel{background:var(--card);border:1px solid var(--card-border);border-radius:var(--radius);padding:18px 20px;margin-bottom:16px;}"
    ".panel h3{margin:0 0 12px 0;font-size:0.95em;}"
    ".upload-row{display:flex;gap:10px;flex-wrap:wrap;align-items:center;}"
    ".upload-row select,.upload-row input[type=file], .upload-row input[type=text] {padding:9px 12px;border-radius:10px;border:1px solid var(--card-border);background:var(--bg-soft);color:var(--text);font-size:0.88em;}"
    ".upload-row input[type=text] {flex:1; min-width:200px;}"
    "a.back-link{color:var(--accent);text-decoration:none;font-size:0.9em;}"
    "@media (max-width:600px){.topbar{padding:12px 14px;}.container{padding:14px;}.file-size{display:none;}}"
    "#toast{position:fixed;top:16px;left:50%;transform:translate(-50%,-40px);background:rgba(28,30,38,0.94);color:#fff;"
      "padding:10px 18px;border-radius:20px;font-size:0.85em;display:flex;align-items:center;gap:8px;opacity:0;"
      "pointer-events:none;transition:transform .28s cubic-bezier(.34,1.4,.64,1),opacity .28s ease;z-index:3000;"
      "box-shadow:0 8px 24px rgba(0,0,0,0.35);max-width:90vw;white-space:nowrap;}"
    "#toast.show{opacity:1;transform:translate(-50%,0);}"
    ".ios-spinner{width:14px;height:14px;border:2px solid rgba(255,255,255,0.25);border-top-color:#fff;border-radius:50%;"
      "animation:spin .7s linear infinite;flex-shrink:0;}"
    "@keyframes spin{to{transform:rotate(360deg);}}"
    ".toast-check{color:#4ade80;font-weight:700;}"
    ".toast-x{color:var(--danger);font-weight:700;}"
    "#previewOverlay{position:fixed;inset:0;background:rgba(0,0,0,0.8);display:none;align-items:center;justify-content:center;z-index:2500;padding:20px;}"
    "#previewOverlay.show{display:flex;}"
    "#previewBox{background:#0b0e16;border-radius:16px;max-width:92vw;max-height:88vh;overflow:hidden;position:relative;"
      "box-shadow:0 20px 60px rgba(0,0,0,0.5);}"
    "#previewBox video{max-width:92vw;max-height:82vh;display:block;background:#000;}"
    "#previewBox audio{width:min(90vw,380px);margin:44px 24px;display:block;}"
    "#previewBox iframe{width:88vw;height:82vh;border:none;background:#fff;}"
    "#previewBox img{max-width:92vw;max-height:82vh;display:block;}"
    "#previewBox #textPreview{max-width:82vw;max-height:80vh;overflow:auto;background:#161d2e;color:#e5e7eb;"
      "padding:24px;margin:24px;border-radius:8px;font-size:0.85em;line-height:1.5;white-space:pre-wrap;word-break:break-word;}"
    "#previewBox #docxPreview{max-width:82vw;max-height:80vh;overflow:auto;background:#fff;color:#111;"
      "padding:32px;margin:24px;border-radius:8px;font-size:0.95em;line-height:1.6;}"
    "#previewClose{position:absolute;top:8px;right:8px;width:32px;height:32px;border-radius:50%;background:rgba(255,255,255,0.15);"
      "color:#fff;border:none;font-size:1.1em;cursor:pointer;z-index:1;}"
    "#previewClose:hover{background:rgba(255,255,255,0.28);}"
    "#moveModalOverlay{position:fixed;inset:0;background:rgba(0,0,0,0.8);display:none;align-items:center;justify-content:center;z-index:2600;padding:20px;}"
    "#moveModalOverlay.show{display:flex;}"
    "#moveModal{background:var(--card);border:1px solid var(--card-border);border-radius:var(--radius);padding:20px;width:100%;max-width:400px;box-shadow:0 20px 60px rgba(0,0,0,0.5);}"
    "#moveModal h3{margin-top:0;margin-bottom:10px;font-size:1.1em;}"
    "#moveModal select{width:100%;padding:10px;margin:15px 0;border-radius:10px;background:var(--bg-soft);color:var(--text);border:1px solid var(--card-border);font-size:1em;}"
    ".modal-actions{display:flex;justify-content:flex-end;gap:10px;}"
    "</style>";
}

//-------------------------------------OLED FUNCTIONS------------------------------------------------------------
void showCentered(String text, int size = 1)
{
  display.clearDisplay();
  display.setTextSize(size);
  display.setTextColor(SSD1306_WHITE);

  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(text,0,0,&x1,&y1,&w,&h);

  display.setCursor((128-w)/2,(64-h)/2);
  display.println(text);
  display.display();
}

// Boot Screen of Oled display
void showBootScreen()
{
  display.clearDisplay();
  display.setTextSize(2);
  display.setCursor(12,18);
  display.println("Media");
  display.setCursor(22,42);
  display.println("Server");
  display.display();
}

void showWiFiConnecting()
{
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(18,20);
  display.println("Connecting WiFi");
  display.setCursor(42,40);
  display.println("...");
  display.display();
}

void showIP()
{
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(2);
  display.setCursor(0,0);
  display.println("WiFi");

  display.setTextSize(1);
  display.setCursor(0,30);
  display.println(WiFi.localIP());

  display.display();
  delay(5000);   
}

void showQRCode(String url)
{
  display.clearDisplay();
  QRCode qrcode;
  uint8_t qrcodeData[qrcode_getBufferSize(3)];

  qrcode_initText(&qrcode,
                  qrcodeData,
                  3,
                  ECC_LOW,
                  url.c_str());

  for (uint8_t y = 0; y < qrcode.size; y++)
  {
    for (uint8_t x = 0; x < qrcode.size; x++)
    {
      if(qrcode_getModule(&qrcode,x,y))
      {
        display.fillRect(x*2, y*2, 2, 2, SSD1306_WHITE);
      }
    }
  }
  display.display();
  delay(5000);
}

void showDashboard()
{
    display.clearDisplay();
    display.setTextSize(1);

    display.setCursor(0,0);
    display.println("ROHAN'S NAS");

    display.drawLine(0,10,128,10,WHITE);

    display.setCursor(0,18);
    display.print("WiFi : ");

    if(WiFi.status()==WL_CONNECTED)
        display.println(WIFI_SSID);
    else
        display.println("DISCONNECTED");

    display.setCursor(0,30);
    display.print("IP:");
    display.println(WiFi.localIP());

    display.setCursor(0,44);
    display.print("SD:");
    
    if(SD.cardType() != CARD_NONE)
        display.println("Ready");
    else
        display.println("Missing");

    display.display();
}

void showStatus(String msg)
{
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println(msg);  
    display.display();
}

void listDocumentsFolder() {
  Serial.println("===== DOCUMENTS FOLDER =====");
  File dir = SD.open("/documents");
  if (!dir) {
    Serial.println("Cannot open /documents");
    return;
  }
  File file;
  while ((file = dir.openNextFile())) {
    Serial.print(file.name());
    Serial.print("   ");
    Serial.println(file.size());
    file.close();
  }
  dir.close();
  Serial.println("===========================");
}

void listAllFiles(const char *dirname) {
  File dir = SD.open(dirname);
  if (!dir) {
    Serial.print("Cannot open: ");
    Serial.println(dirname);
    return;
  }
  File file;
  while (true) {
    file = dir.openNextFile();
    if (!file) break;
    if (file.isDirectory()) {
      Serial.print("[DIR] ");
      Serial.println(file.name());
      String next = String(dirname);
      if (next != "/") next += "/";
      next += file.name();
      file.close();
      listAllFiles(next.c_str());
    } else {
      Serial.print("[FILE] ");
      Serial.print(dirname);
      Serial.print("/");
      Serial.print(file.name());
      Serial.print("   ");
      Serial.println(file.size());
      file.close();
    }
  }
  dir.close();
}

String getPageScript() {
  return
    "<div id='toast'></div>"
    "<div id='previewOverlay' onclick=\"if(event.target===this)closePreview();\"><div id='previewBox'></div></div>"
    "<script>"
    "var PREVIEW_KEY='" + String(PREVIEW_KEY) + "';"
    "function setToast(iconHtml,msg){var t=document.getElementById('toast');t.innerHTML=iconHtml+'<span>'+msg+'</span>';t.classList.add('show');}"
    "function toastSpinner(msg){setToast('<span class=\"ios-spinner\"></span>',msg);}"
    "function toastDone(msg,ok){if(ok===undefined)ok=true;setToast(ok?'<span class=\"toast-check\">&#10003;</span>':'<span class=\"toast-x\">&#10005;</span>',msg);hideToastDelayed();}"
    "function hideToastDelayed(delay){clearTimeout(window.__toastTimer);window.__toastTimer=setTimeout(function(){document.getElementById('toast').classList.remove('show');},delay||1500);}"
    "function refreshListings(){fetch('/',{credentials:'same-origin'}).then(function(r){return r.text();}).then(function(html){"
      "var doc=new DOMParser().parseFromString(html,'text/html');"
      "var ng=doc.querySelector('.grid');var cg=document.querySelector('.grid');if(ng&&cg){cg.innerHTML=ng.innerHTML;}"
      "var ns=doc.querySelector('select[name=\"folder\"]');var cs=document.querySelector('select[name=\"folder\"]');if(ns&&cs){cs.innerHTML=ns.innerHTML;}"
      "var nmd=doc.getElementById('moveDest');var cmd=document.getElementById('moveDest');if(nmd&&cmd){cmd.innerHTML=nmd.innerHTML;}"
      "}).catch(function(){});}"
    "function guessMime(path){var ext=path.split('.').pop().toLowerCase();var map={mp4:'video/mp4',mkv:'video/x-matroska',"
      "webm:'video/webm',mov:'video/quicktime',mp3:'audio/mpeg',wav:'audio/wav',flac:'audio/flac',m4a:'audio/mp4',aac:'audio/aac',ogg:'audio/ogg'};"
      "return map[ext]||'';}"
    "function loadMammoth(cb){if(window.mammoth){cb();return;}var s=document.createElement('script');"
      "s.src='https://cdnjs.cloudflare.com/ajax/libs/mammoth/1.6.0/mammoth.browser.min.js';"
      "s.onload=cb;s.onerror=function(){toastDone('Could not load document viewer (no internet?)',false);};"
      "document.head.appendChild(s);}"
    "function openPreview(ev,path,kind){ev.preventDefault();var box=document.getElementById('previewBox');"
      "box.innerHTML='<button id=\"previewClose\" onclick=\"closePreview()\">&times;</button><div id=\"previewContent\">Loading...</div>';"
      "document.getElementById('previewOverlay').classList.add('show');"
      "var content=document.getElementById('previewContent');"
      "var freshPath=path+(path.indexOf('?')===-1?'?':'&')+'t='+Date.now();"
      "if(kind==='image'){content.innerHTML='<img src=\"'+freshPath+'\" alt=\"preview\">';}"
      "else if(kind==='video'){var vmime=guessMime(path);"
        "var vUrl='/mediaStream?path='+encodeURIComponent(path)+'&key='+PREVIEW_KEY+'&t='+Date.now();"
        "content.innerHTML='<video controls preload=\"metadata\" playsinline><source src=\"'+vUrl+'\"'+(vmime?' type=\"'+vmime+'\"':'')+'>Your browser can not play this video.</video>';"
        "var v=content.querySelector('video');"
        "v.addEventListener('loadedmetadata',function(){v.play().catch(function(){});});"
        "v.addEventListener('error',function(){toastDone('Playback failed - try downloading instead',false);});}"
      "else if(kind==='audio'){var mUrl='/mediaStream?path='+encodeURIComponent(path)+'&key='+PREVIEW_KEY+'&t='+Date.now();"
        "content.innerHTML='<audio controls preload=\"metadata\" src=\"'+mUrl+'\"></audio>';"
        "var aEl=content.querySelector('audio');"
        "aEl.addEventListener('loadedmetadata',function(){aEl.play().catch(function(){});});"
        "aEl.addEventListener('error',function(){toastDone('Playback failed - try downloading instead',false);});}"
      "else if(kind==='pdf'){content.innerHTML='<iframe src=\"'+freshPath+'\"></iframe>';}"
      "else if(kind==='text'){content.innerHTML='<pre id=\"textPreview\">Loading...</pre>';"
        "fetch(freshPath,{credentials:'same-origin'}).then(function(r){return r.text();})"
        ".then(function(txt){document.getElementById('textPreview').textContent=txt;})"
        ".catch(function(){toastDone('Could not load file',false);});}"
      "else if(kind==='docx'){content.innerHTML='<div id=\"docxPreview\">Loading document...</div>';"
        "loadMammoth(function(){fetch(freshPath,{credentials:'same-origin'}).then(function(r){return r.arrayBuffer();})"
        ".then(function(buf){return mammoth.convertToHtml({arrayBuffer:buf});})"
        ".then(function(result){document.getElementById('docxPreview').innerHTML=result.value;})"
        ".catch(function(){toastDone('Could not preview document',false);});});}"
      "return false;}"
    "function closePreview(){document.getElementById('previewOverlay').classList.remove('show');document.getElementById('previewBox').innerHTML='';}"
    "function runSync(){toastSpinner('Syncing with Drive...');fetch('/sync',{credentials:'same-origin'})"
      ".then(function(){toastDone('Sync started - check Serial Monitor');})"
      ".catch(function(){toastDone('Sync request failed',false);});}"
    "function runRemount(){toastSpinner('Remounting SD card...');fetch('/remountSD',{credentials:'same-origin'})"
      ".then(function(r){return r.text();}).then(function(txt){var ok=txt.indexOf('successfully')!==-1;"
      "toastDone(ok?'SD card remounted':'Remount failed',ok);refreshListings();})"
      ".catch(function(){toastDone('Remount request failed',false);});}"
    "function pushToDrive(path){toastSpinner('Uploading to Drive...');"
      "fetch('/pushToDrive?path='+encodeURIComponent(path),{credentials:'same-origin'})"
      ".then(function(){toastDone('Upload started - check Serial Monitor');})"
      ".catch(function(){toastDone('Upload request failed',false);});return false;}"
    "function deleteFile(path,name){if(!confirm('Delete '+name+'?'))return false;toastSpinner('Deleting '+name+'...');"
      "fetch('/deleteFile?path='+encodeURIComponent(path),{credentials:'same-origin'})"
      ".then(function(r){return r.text();}).then(function(txt){var ok=txt.indexOf('Deleted')!==-1;"
      "toastDone(ok?'Deleted':'Delete failed',ok);refreshListings();})"
      ".catch(function(){toastDone('Delete request failed',false);});return false;}"
    "function renameFile(path,name){var n=prompt('New name for '+name);if(!n)return false;toastSpinner('Renaming...');"
      "fetch('/renameFile?path='+encodeURIComponent(path)+'&newName='+encodeURIComponent(n),{credentials:'same-origin'})"
      ".then(function(r){return r.text();}).then(function(txt){var ok=txt.indexOf('Renamed to')!==-1;"
      "toastDone(ok?'Renamed':'Rename failed',ok);refreshListings();})"
      ".catch(function(){toastDone('Rename request failed',false);});return false;}"
    
    "var currentMovePath='';var currentMoveName='';"
    "function moveFile(path,name){currentMovePath=path;currentMoveName=name;document.getElementById('moveFileName').textContent=name;document.getElementById('moveModalOverlay').classList.add('show');return false;}"
    "function closeMoveModal(){document.getElementById('moveModalOverlay').classList.remove('show');}"
    "function submitMove(){var dest=document.getElementById('moveDest').value;if(!dest)return;closeMoveModal();toastSpinner('Moving...');fetch('/moveFile?path='+encodeURIComponent(currentMovePath)+'&newFolder='+encodeURIComponent(dest),{credentials:'same-origin'}).then(function(r){if(r.ok)return r.text();throw new Error('Failed');}).then(function(txt){toastDone('Moved successfully');refreshListings();}).catch(function(){toastDone('Move failed',false);});}"
    
    "function submitUpload(ev){ev.preventDefault();var form=document.getElementById('uploadForm');"
      "var fileInput=form.querySelector('input[type=file]');if(!fileInput.files.length){toastDone('Choose a file first',false);return false;}"
      "var fd=new FormData(form);var xhr=new XMLHttpRequest();xhr.open('POST','/upload',true);"
      "xhr.upload.onprogress=function(e){if(e.lengthComputable){var pct=Math.round((e.loaded/e.total)*100);"
      "toastSpinner('Uploading... '+pct+'%');}};"
      "xhr.onload=function(){if(xhr.status===200){toastDone('Upload complete');form.reset();refreshListings();}"
      "else{toastDone('Upload failed',false);}};"
      "xhr.onerror=function(){toastDone('Upload failed',false);};"
      "toastSpinner('Uploading...');xhr.send(fd);return false;}"
    "function createFolder(ev){ev.preventDefault();var name=document.getElementById('newFolderName').value;"
      "if(!name)return false;if(!name.startsWith('/'))name='/'+name;"
      "toastSpinner('Creating folder...');"
      "fetch('/createFolder?name='+encodeURIComponent(name),{credentials:'same-origin'})"
      ".then(function(r){if(r.ok)return r.text();throw new Error('Failed');}).then(function(txt){"
      "toastDone('Folder Created');document.getElementById('newFolderName').value='';refreshListings();"
      "}).catch(function(){toastDone('Failed (Already exists?)',false);});return false;}"
    "function deleteFolder(path){if(!confirm('Delete folder '+path+'? Must be empty!'))return false;"
      "toastSpinner('Deleting...'); fetch('/deleteFolder?path='+encodeURIComponent(path),{credentials:'same-origin'})"
      ".then(function(r){if(r.ok)return r.text();throw new Error('Failed');}).then(function(txt){"
      "toastDone('Folder Deleted');refreshListings();"
      "}).catch(function(){toastDone('Delete failed (Ensure empty)',false);});return false;}"
    "function renameFolder(path){var n=prompt('New folder name (e.g. /myfolder):', path);if(!n || n===path)return false;"
      "toastSpinner('Renaming...');fetch('/renameFolder?path='+encodeURIComponent(path)+'&newName='+encodeURIComponent(n),{credentials:'same-origin'})"
      ".then(function(r){if(r.ok)return r.text();throw new Error('Failed');}).then(function(txt){"
      "toastDone('Folder Renamed');refreshListings();"
      "}).catch(function(){toastDone('Rename failed',false);});return false;}"
    "</script>";
}

// Builds one file row's HTML. Uses name and size directly to support manifest reading.
String buildFileRowHtml(const String& folderPathStr, const String& name, size_t size) {
  String fullPath = folderPathStr + "/" + name;
  String kind = getFileKind(name);
  String safePath = escapeForJsAttr(fullPath);
  String safeName = escapeForJsAttr(name);
  String hrefPath = htmlEscape(fullPath);
  String displayName = htmlEscape(name);
  
  String nameClickAttr = "";
  if (kind != "other") {
    nameClickAttr = " onclick=\"return openPreview(event,'" + safePath + "','" + kind + "');\"";
  }
  
  return "<div class='file-row'>"
         "<a class='file-name' href=\"" + hrefPath + "\"" + nameClickAttr + ">"
         "<span class='file-icon'>" + getFileIcon(name) + "</span>"
         "<span class='file-name-text'>" + displayName + "</span></a>"
         "<span class='file-size'>" + getFileSizeStr(size) + "</span>"
         "<span class='file-actions'>"
         "<a class='btn-icon' href=\"" + hrefPath + "\" download title='Download'>&#11015;</a>"
         "<a class='btn-icon' href=\"#\" onclick=\"return moveFile('" + safePath + "','" + safeName + "');\" title='Move'>&#128194;</a>"
         "<a class='btn-icon' href=\"#\" onclick=\"return pushToDrive('" + safePath + "');\" title='Upload to Drive'>&#9729;</a>"
         "<a class='btn-icon' href=\"#\" onclick=\"return renameFile('" + safePath + "','" + safeName + "');\" title='Rename'>&#9998;</a>"
         "<a class='btn-icon btn-danger' href=\"#\" onclick=\"return deleteFile('" + safePath + "','" + safeName + "');\" title='Delete'>&#128465;</a>"
         "</span></div>";
}


// State machine using manifestFile instead of SD directory enumeration
String genHomeNext(PageGenState* st) {
  if (st->step == 0) {
    st->step = 1;
    String s = "<html><head><title>Home Media Server</title>"
               "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    s += getPageStyle();
    s += "</head><body>";
    s += "<div class='topbar'>"
         "<h1>&#128187; Home Media Server</h1>"
         "<div class='actions'>"
         "<button class='btn btn-secondary' onclick='runSync()'>&#9729; Sync with Drive</button>"
         "<button class='btn btn-secondary' onclick='runRemount()'>&#128260; Remount SD</button>"
         "<button class='btn btn-secondary' onclick='refreshListings()'>&#8635; Refresh</button>"
         "</div></div>";
    s += "<div class='container'>";
    s += "<form class='search-form' method='GET' action='/search'>"
         "<input type='text' name='q' placeholder='Search files across all folders...'>"
         "<button type='submit'>Search</button></form>";
    s += "<div class='grid'>";
    return s;
  }

  if (st->step == 1) {
    if (st->folderIdx >= activeFolders.size()) {
      st->step = 2;
      return "";
    }
    const String& path = activeFolders[st->folderIdx];
    
    // Open the manifest file instead of the directory
    if (!st->folderOpened) {
      String manifestPath = path + "/.manifest.txt";
      st->manifestFile = SD.open(manifestPath);
      st->folderOpened = true;
      st->folderAny = false;
      
      bool prot = (path == "/documents" || path == "/media/audio" || path == "/media/video" || path == "/synced");
      String actions = "";
      if (!prot) {
         actions = "<a class='btn-icon' href='#' onclick=\"return renameFolder('" + escapeForJsAttr(path) + "');\" title='Rename Folder' style='height:24px;width:24px;font-size:0.7em;'>&#9998;</a>"
                   "<a class='btn-icon btn-danger' href='#' onclick=\"return deleteFolder('" + escapeForJsAttr(path) + "');\" title='Delete Folder' style='height:24px;width:24px;font-size:0.7em;'>&#128465;</a>";
      }

      return "<div class='card'><div class='card-header'>"
             "<h2>" + getFolderDisplayName(path) + "</h2>"
             "<div style='display:flex;gap:8px;align-items:center;'><span class='card-path'>" + path + "</span>" + actions + "</div>"
             "</div><div class='file-list'>";
    }

    // Pull entries from the manifest text
    while (st->manifestFile && st->manifestFile.available()) {
      String name = st->manifestFile.readStringUntil('\n');
      name.trim();
      
      // Skip empty lines or the manifest itself
      if (name.length() == 0 || name == ".manifest.txt") continue;

      String fullPath = path + "/" + name;
      
      // Direct-path lookup for the actual file
      File entry = SD.open(fullPath);
      if (entry && !entry.isDirectory()) {
        st->folderAny = true;
        size_t fileSize = entry.size();
        entry.close(); // Close immediately to save handles
        return buildFileRowHtml(path, name, fileSize);
      }
      if (entry) entry.close(); // Close if it was a directory or phantom entry
    }

    // Manifest exhausted — close it out and move to the next folder
    if (st->manifestFile) st->manifestFile.close();
    String s = st->folderAny ? "" : "<div class='empty-state'>No files yet.</div>";
    s += "</div></div>"; // close file-list, card
    st->folderOpened = false;
    st->folderIdx++;
    return s;
  }

  if (st->step == 2) {
    st->step = 3;
    String s = "</div>"; // close grid
    
    // Build folder options string for reuse in multiple select dropdowns
    String folderOptions = "";
    for (size_t i = 0; i < activeFolders.size(); i++) {
        folderOptions += "<option value='" + activeFolders[i] + "'>" + getFolderDisplayName(activeFolders[i]) + "</option>";
    }

    // Folder Creation Panel
    s += "<div class='panel'>"
         "<h3>&#128193; Create Folder</h3>"
         "<form class='upload-row' onsubmit='return createFolder(event);'>"
         "<input type='text' id='newFolderName' placeholder='/newfolder' required pattern='^/.*'>"
         "<button type='submit'>Create</button>"
         "</form></div>";
    
    // File Upload Panel
    s += "<div class='panel'>"
         "<h3>&#11014; Upload a file</h3>"
         "<form id='uploadForm' class='upload-row' onsubmit='return submitUpload(event);'>"
         "<select name='folder'>" + folderOptions + "</select>"
         "<input type='file' name='file'>"
         "<button type='submit'>Upload</button>"
         "</form></div>";

    // Move File Modal (Hidden by default, triggered via JavaScript)
    s += "<div id='moveModalOverlay' onclick=\"if(event.target===this)closeMoveModal();\">"
         "<div id='moveModal'>"
         "<h3>&#128194; Move File</h3>"
         "<p style='margin:0;color:var(--text-dim);font-size:0.9em;'>Moving: <strong id='moveFileName' style='color:var(--text);'></strong></p>"
         "<select id='moveDest'>" + folderOptions + "</select>"
         "<div class='modal-actions'>"
         "<button type='button' class='btn btn-secondary' onclick='closeMoveModal()'>Cancel</button>"
         "<button type='button' class='btn' onclick='submitMove()'>Move</button>"
         "</div></div></div>";
         
    s += "</div>"; // close container
    s += getPageScript();
    s += "</body></html>";
    return s;
  }

  return ""; // step == 3: truly finished
}

// Searches via manifest instead of directory enumeration
void streamSearchResults(AsyncResponseStream* out, const String& query) {
  String q = query;
  q.toLowerCase();
  out->print("<div class='card'><div class='card-header'><h2>Search results for \"" + htmlEscape(query) + "\"</h2></div><div class='file-list'>");
  int matches = 0;

  for (size_t i = 0; i < activeFolders.size(); i++) {
    String manifestPath = activeFolders[i] + "/.manifest.txt";
    File manifestFile = SD.open(manifestPath);
    if (!manifestFile) continue;

    while (manifestFile.available()) {
      String name = manifestFile.readStringUntil('\n');
      name.trim();
      if (name.length() == 0 || name == ".manifest.txt") continue;

      String nameLower = name;
      nameLower.toLowerCase();
      
      if (nameLower.indexOf(q) != -1) {
        String fullPath = activeFolders[i] + "/" + name;
        File entry = SD.open(fullPath);
        if (entry && !entry.isDirectory()) {
          
          String kind = getFileKind(name);
          String safePath = escapeForJsAttr(fullPath);
          String safeName = escapeForJsAttr(name);
          String hrefPath = htmlEscape(fullPath);
          String displayPath = htmlEscape(fullPath);
          
          String nameClickAttr = "";
          if (kind != "other") {
            nameClickAttr = " onclick=\"return openPreview(event,'" + safePath + "','" + kind + "');\"";
          }
          
          out->print("<div class='file-row'>"
                  "<a class='file-name' href=\"" + hrefPath + "\"" + nameClickAttr + "><span class='file-icon'>" + getFileIcon(name) + "</span><span class='file-name-text'>" + displayPath + "</span></a>"
                  "<span class='file-size'>" + getFileSizeStr(entry.size()) + "</span>"
                  "<span class='file-actions'>"
                  "<a class='btn-icon' href=\"" + hrefPath + "\" download title='Download'>&#11015;</a>"
                  "<a class='btn-icon' href=\"#\" onclick=\"return moveFile('" + safePath + "','" + safeName + "');\" title='Move'>&#128194;</a>"
                  "<a class='btn-icon btn-danger' href=\"#\" onclick=\"return deleteFile('" + safePath + "','" + safeName + "');\" title='Delete'>&#128465;</a>"
                  "</span>"
                  "</div>");
          matches++;
        }
        if (entry) entry.close();
      }
      yield();
    }
    manifestFile.close();
  }

  if (matches == 0) {
    out->print("<div class='empty-state'>No files matched.</div>");
  }
  out->print("</div></div>");
}

void debugListFolder(File dir, String indent, String& out) {
  File entry = dir.openNextFile();
  while (entry) {
    String name = entry.name();
    String line = indent + "- \"" + name + "\" (" + String(name.length()) + " chars, " +
                  String(entry.size()) + " bytes)" + (entry.isDirectory() ? " [DIR]" : "");
    Serial.println(line);
    out += line + "\n";
    if (entry.isDirectory()) {
      File sub = SD.open(entry.path());
      debugListFolder(sub, indent + "  ", out);
      sub.close();
    }
    entry.close();
    entry = dir.openNextFile();
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  
  Wire.begin(OLED_SDA, OLED_SCL);
  display.begin(SSD1306_SWITCHCAPVCC,0x3C);
  display.clearDisplay();
  showBootScreen();
  delay(1500);
  
  // --- SD card init ---
  SPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
  if (!SD.begin(SD_CS_PIN)) {
    Serial.println("SD card mount failed! Check wiring.");
  } else {
    Serial.println("SD card mounted.");
    loadFolderManifest();
    ensureFolders();
  }
  
  showWiFiConnecting();
  
  // --- Wi-Fi Station mode ---
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("Connected! IP address: ");
  Serial.println(WiFi.localIP());
  
  Serial.println("Showing IP");
  showIP();
  delay(2000);

  Serial.println("Showing WR Code");
  showQRCode(ngrokURL);

  Serial.println("Showing Dashboard");
  showDashboard();
  delay(2000);

  // --- mDNS so you can use http://mediaserver.local instead of typing the IP ---
  if (MDNS.begin(HOSTNAME)) {
    Serial.println("mDNS started: http://" + String(HOSTNAME) + ".local");
  }

  // --- Avoid the browser silently firing repeated authenticated requests for a favicon ---
  server.on("/favicon.ico", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(204);
  });

  // --- Home page: lists all files (auth required) ---
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!request->authenticate(AUTH_USERNAME, AUTH_PASSWORD)) {
      return request->requestAuthentication();
    }
    Serial.print("Free heap before home page: ");
    Serial.println(ESP.getFreeHeap());

    auto state = std::make_shared<PageGenState>();
    AsyncWebServerResponse *response = request->beginChunkedResponse(
      "text/html",
      [state](uint8_t *buffer, size_t maxLen, size_t /*index*/) -> size_t {
        size_t written = 0;
        while (written < maxLen) {
          if (state->pending.length() == 0) {
            if (state->step >= 3) break; // truly done
            state->pending = genHomeNext(state.get());
            if (state->pending.length() == 0 && state->step < 3) {
              continue; // that step produced no text (e.g. skipped dir) — try the next one
            }
          }
          size_t toCopy = maxLen - written;
          if (toCopy > (size_t)state->pending.length()) toCopy = state->pending.length();
          memcpy(buffer + written, state->pending.c_str(), toCopy);
          written += toCopy;
          state->pending.remove(0, toCopy);
        }
        return written;
      });
    request->send(response);
    Serial.print("Free heap after starting home page: ");
    Serial.println(ESP.getFreeHeap());
  });
  
  // --- Streams audio/video for in-browser preview, authenticated via key or Basic Auth ---
  server.on("/mediaStream", HTTP_GET, [](AsyncWebServerRequest *request) {
    bool keyOk = request->hasParam("key") && request->getParam("key")->value() == String(PREVIEW_KEY);
    if (!keyOk && !request->authenticate(AUTH_USERNAME, AUTH_PASSWORD)) {
      return request->requestAuthentication();
    }
    if (!request->hasParam("path")) {
      request->send(400, "text/plain", "Missing path.");
      return;
    }
    String path = request->getParam("path")->value();
    // Allow any folder now
    if (!SD.exists(path)) {
      request->send(404, "text/plain", "Not found.");
      return;
    }
    request->send(SD, path, getMimeForPath(path)); // supports Range requests natively — seeking works
  });

  // Since Folders are dynamic, we create a catch-all handler for file streaming statically
  // This allows serving from any dynamic folder name
  server.onNotFound([](AsyncWebServerRequest *request) {
    if (!request->authenticate(AUTH_USERNAME, AUTH_PASSWORD)) {
      return request->requestAuthentication();
    }
    String path = request->url();
    if (SD.exists(path) && !SD.open(path).isDirectory()) {
       request->send(SD, path, getMimeForPath(path));
    } else {
       request->send(404, "text/plain", "File not found");
    }
  });

  // --- Remount SD card after a hot-swap, without rebooting (auth required) ---
  server.on("/remountSD", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!request->authenticate(AUTH_USERNAME, AUTH_PASSWORD)) {
      return request->requestAuthentication();
    }
    bool ok = remountSD();
    String html = ok ? "<p>SD card remounted successfully.</p>" : "<p>Remount failed — check the card is seated properly.</p>";
    html += "<a href='/'>Go back</a>";
    request->send(200, "text/html", html);
  });

  // --- Manual Drive sync trigger (auth required) — UNCHANGED ---
  server.on("/sync", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!request->authenticate(AUTH_USERNAME, AUTH_PASSWORD)) {
        return request->requestAuthentication();
    }

    syncRequested = true;

    request->send(
        200,
        "text/html",
        "<h3>Sync Started</h3>"
        "<p>Check Serial Monitor.</p>"
        "<a href='/'>Go back</a>"
    );
  });

  server.on("/pushToDrive", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!request->authenticate(AUTH_USERNAME, AUTH_PASSWORD)) {
      return request->requestAuthentication();
    }
    if (!request->hasParam("path")) {
      request->send(400, "text/html", "<p>Missing file path.</p><a href='/'>Go back</a>");
      return;
    }
    pushPath = request->getParam("path")->value();
    pushRequested = true;
    request->send(200, "text/html",
        "<h3>Upload Started</h3><p>Check Serial Monitor for progress.</p><a href='/'>Go back</a>");
  });

  // --- Move File ---
  server.on("/moveFile", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!request->authenticate(AUTH_USERNAME, AUTH_PASSWORD)) return request->requestAuthentication();
    if (!request->hasParam("path") || !request->hasParam("newFolder")) { 
        request->send(400, "text/plain", "Missing path or newFolder"); 
        return; 
    }
    
    String path = request->getParam("path")->value();
    String newFolder = request->getParam("newFolder")->value();
    
    if (!newFolder.startsWith("/")) newFolder = "/" + newFolder;

    int slash = path.lastIndexOf('/');
    String oldFolder = (slash != -1) ? path.substring(0, slash) : "";
    String filename = (slash != -1) ? path.substring(slash + 1) : path;
    String newPath = newFolder + "/" + filename;

    if (oldFolder == newFolder) {
        request->send(400, "text/plain", "File is already in this folder.");
        return;
    }

    if (!SD.exists(newFolder)) {
        request->send(400, "text/plain", "Destination folder does not exist.");
        return;
    }

    if (SD.rename(path, newPath)) {
        removeFromManifest(oldFolder, filename);
        appendToManifest(newFolder, filename);
        
        showStatus("Moved File\n" + filename);
        delay(1000);
        showDashboard();
        
        request->send(200, "text/plain", "Moved successfully.");
    } else {
        request->send(500, "text/plain", "Move failed.");
    }
  });

  // --- Create Folder ---
  server.on("/createFolder", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!request->authenticate(AUTH_USERNAME, AUTH_PASSWORD)) return request->requestAuthentication();
    if (!request->hasParam("name")) { request->send(400, "text/plain", "Missing name"); return; }
    String name = request->getParam("name")->value();
    if (!name.startsWith("/")) name = "/" + name;
    
    if (SD.exists(name)) {
      request->send(400, "text/plain", "Already exists");
      return;
    }
    if (SD.mkdir(name)) {
      activeFolders.push_back(name);
      saveFolderManifest();
      
      showStatus("Folder Created\n" + name);
      delay(1000);
      showDashboard();
      
      request->send(200, "text/plain", "Folder created");
    } else {
      request->send(500, "text/plain", "Create failed");
    }
  });

  // --- Delete Folder ---
  server.on("/deleteFolder", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!request->authenticate(AUTH_USERNAME, AUTH_PASSWORD)) return request->requestAuthentication();
    if (!request->hasParam("path")) { request->send(400, "text/plain", "Missing path"); return; }
    String path = request->getParam("path")->value();
    
    if (path == "/" || path == "/documents" || path == "/media/audio" || path == "/media/video" || path == "/synced") {
       request->send(400, "text/plain", "Cannot delete system folder"); return;
    }

    bool canDelete = true;
    String manifestPath = path + "/.manifest.txt";
    File m = SD.open(manifestPath);
    if (m) {
        while (m.available()) {
            String line = m.readStringUntil('\n'); line.trim();
            if (line.length() > 0 && line != ".manifest.txt") { canDelete = false; break; }
        }
        m.close();
    }
    
    if (!canDelete) {
        request->send(400, "text/plain", "Folder not empty");
        return;
    }
    
    if (SD.exists(manifestPath)) SD.remove(manifestPath);
    
    if (SD.rmdir(path)) {
      for (auto it = activeFolders.begin(); it != activeFolders.end(); ++it) {
        if (*it == path) {
          activeFolders.erase(it);
          break;
        }
      }
      saveFolderManifest();
      
      showStatus("Folder Deleted\n" + path);
      delay(1000);
      showDashboard();
      
      request->send(200, "text/plain", "Deleted");
    } else {
      request->send(500, "text/plain", "Delete failed. Ensure it is empty.");
    }
  });

  // --- Rename Folder ---
  server.on("/renameFolder", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!request->authenticate(AUTH_USERNAME, AUTH_PASSWORD)) return request->requestAuthentication();
    if (!request->hasParam("path") || !request->hasParam("newName")) { request->send(400, "text/plain", "Missing args"); return; }
    String path = request->getParam("path")->value();
    String newName = request->getParam("newName")->value();
    if (!newName.startsWith("/")) newName = "/" + newName;
    
    if (path == "/" || path == "/documents" || path == "/media/audio" || path == "/media/video" || path == "/synced") {
       request->send(400, "text/plain", "Cannot rename system folder"); return;
    }
    
    if (SD.rename(path, newName)) {
      for (size_t i = 0; i < activeFolders.size(); i++) {
        if (activeFolders[i] == path) {
          activeFolders[i] = newName;
          break;
        }
      }
      saveFolderManifest();
      
      showStatus("Folder Renamed\n" + newName);
      delay(1000);
      showDashboard();
      
      request->send(200, "text/plain", "Renamed");
    } else {
      request->send(500, "text/plain", "Rename failed");
    }
  });

  // --- Delete a file (auth required) ---
  server.on("/deleteFile", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!request->authenticate(AUTH_USERNAME, AUTH_PASSWORD)) {
      return request->requestAuthentication();
    }
    if (!request->hasParam("path")) {
      request->send(400, "text/html", "<p>Missing file path.</p><a href='/'>Go back</a>");
      return;
    }
    String path = request->getParam("path")->value();
    int slash = path.lastIndexOf('/');
    String folder = (slash != -1) ? path.substring(0, slash) : "";
    String filename = (slash != -1) ? path.substring(slash + 1) : path;
    
    String html;
    if (SD.remove(path)) {
      removeFromManifest(folder, filename);
      
      showStatus("File Deleted\n" + filename);
      delay(1000);
      showDashboard();
      
      html = "<p>Deleted " + path + "</p>";
    } else {
      html = "<p>Failed to delete " + path + " (file may not exist).</p>";
    }
    html += "<a href='/'>Go back</a>";
    request->send(200, "text/html", html);
  });

  // --- Rename a file (auth required) ---
  server.on("/renameFile", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!request->authenticate(AUTH_USERNAME, AUTH_PASSWORD)) {
      return request->requestAuthentication();
    }
    if (!request->hasParam("path") || !request->hasParam("newName")) {
      request->send(400, "text/html", "<p>Missing path or newName.</p><a href='/'>Go back</a>");
      return;
    }
    String path = request->getParam("path")->value();
    String newName = request->getParam("newName")->value();

    int slash = path.lastIndexOf('/');
    String folder = (slash != -1) ? path.substring(0, slash) : "";
    String oldName = (slash != -1) ? path.substring(slash + 1) : path;
    String newPath = folder + "/" + newName;

    String html;
    if (SD.rename(path, newPath)) {
      renameInManifest(folder, oldName, newName);
      
      showStatus("File Renamed\n" + newName);
      delay(1000);
      showDashboard();
      
      html = "<p>Renamed to " + newPath + "</p>";
    } else {
      html = "<p>Rename failed. Check the new name doesn't already exist.</p>";
    }
    html += "<a href='/'>Go back</a>";
    request->send(200, "text/html", html);
  });

  // --- Search files by name across all folders (auth required) ---
  server.on("/search", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!request->authenticate(AUTH_USERNAME, AUTH_PASSWORD)) {
      return request->requestAuthentication();
    }
    String q = request->hasParam("q") ? request->getParam("q")->value() : "";
    AsyncResponseStream *response = request->beginResponseStream("text/html");
    response->print("<html><head><title>Search</title>"
                    "<meta name='viewport' content='width=device-width, initial-scale=1'>");
    response->print(getPageStyle());
    response->print("</head><body>"
            "<div class='topbar'><h1>&#128269; Search</h1>"
            "<div class='actions'><a class='btn btn-secondary' href='/'>&larr; Back to files</a></div></div>"
            "<div class='container'>");
    streamSearchResults(response, q);
    response->print("</div>");
    response->print(getPageScript());
    response->print("</body></html>");
    request->send(response);
  });

  // --- Diagnostic: print raw SD contents to Serial + browser. Read-only, no side effects. ---
  server.on("/debugListSD", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!request->authenticate(AUTH_USERNAME, AUTH_PASSWORD)) {
      return request->requestAuthentication();
    }
    Serial.println("\n===== DEBUG: RAW SD CARD CONTENTS =====");
    String out = "Raw SD card contents:\n\n";
    for (size_t i = 0; i < activeFolders.size(); i++) {
      Serial.println(activeFolders[i] + String(":"));
      out += activeFolders[i] + ":\n";
      File dir = SD.open(activeFolders[i]);
      if (dir && dir.isDirectory()) {
        debugListFolder(dir, "  ", out);
        dir.close();
      } else {
        Serial.println("  (could not open folder)");
        out += "  (could not open folder)\n";
      }
    }
    Serial.println("========================================");
    request->send(200, "text/plain", out);
  });

  // --- Upload handler (auth required) ---
  server.on("/upload", HTTP_POST,
    [](AsyncWebServerRequest *request) {
      if (!request->authenticate(AUTH_USERNAME, AUTH_PASSWORD)) {
        return request->requestAuthentication();
      }
      request->send(200, "text/html", "<p>Upload complete. <a href='/'>Go back</a></p>");
    },
    [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
      if (!request->authenticate(AUTH_USERNAME, AUTH_PASSWORD)) {
        return;
      }
      static File uploadFile;
      static String targetFolder = "/documents";

      if (index == 0) {
        Serial.print("Filename received: [");
        Serial.print(filename);
        Serial.println("]");
    
        showStatus("Uploading...\n" + filename);
    
        if (request->hasParam("folder", true)) {
          targetFolder = request->getParam("folder", true)->value();
        }
    
        String path = targetFolder + "/" + filename;
        SD.remove(path);
        
        uploadFile = SD.open(path, FILE_WRITE);
    
        Serial.println("Upload start: " + path);
      }
      if (uploadFile) {
        uploadFile.write(data, len);
      }
      if (final) {
        if (uploadFile) uploadFile.close();
        Serial.println("Upload finished: " + filename);
        String fullPath = targetFolder + "/" + filename;

        File verify = SD.open(fullPath);
    
        if (verify) {
            Serial.println("===== VERIFY SUCCESS =====");
            Serial.print("Name: ");
            Serial.println(verify.name());
            Serial.print("Size: ");
            Serial.println(verify.size());
            verify.close();

            // ADD TO MANIFEST AFTER VERIFYING SUCCESSFUL UPLOAD
            appendToManifest(targetFolder, filename);

            Serial.println("\n===== COMPLETE SD CARD =====");
            listAllFiles("/");
            Serial.println("============================");
        } else {
            Serial.println("===== VERIFY FAILED =====");
        }
        showStatus("Upload Complete\n" + filename);
        delay(1500);
        showDashboard();
      }
    });

  server.begin();
  Serial.println("Server started.");
}

void loop() {
    if (syncRequested) {
        syncRequested = false;
        Serial.println();
        Serial.println("================================");
        Serial.println("Starting Google Drive Sync...");
        Serial.println("================================");
        showStatus("Syncing Drive...");
        lastSyncResult = syncWithDrive();
        showStatus("Sync Complete");
        delay(1500);
        showDashboard();
        Serial.println();
        Serial.println("================================");
        Serial.println(lastSyncResult);
        Serial.println("================================");
    }

    if (pushRequested) {
        pushRequested = false;
        Serial.println();
        Serial.println("================================");
        Serial.println("Starting single-file upload: " + pushPath);
        Serial.println("================================");

        if (WiFi.status() != WL_CONNECTED) {
          lastPushResult = "No Wi-Fi connection.";
        } else if (!refreshAccessToken()) {
          lastPushResult = "Failed to get Drive access token.";
        } else {
                  showStatus("Uploading...");
                  if (uploadFileToDrive(pushPath)) {
                      lastPushResult = "Uploaded " + pushPath + " successfully.";
                      showStatus("Upload Done");
                      delay(1500);
                  } else {
                      lastPushResult = "Upload failed for " + pushPath + ".";
                      showStatus("Upload Failed");
                      delay(1500);
                  }
                  showDashboard();
              }

        Serial.println();
        Serial.println("================================");
        Serial.println(lastPushResult);
        Serial.println("================================");
    }
    delay(1);
    yield();
}
