#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <SD.h>
#include <SPI.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

volatile bool syncRequested = false;
String lastSyncResult = "No sync performed yet.";

volatile bool pushRequested = false;
String pushPath = "";
String lastPushResult = "No push performed yet.";

// ---------- CONFIG: EDIT THESE ----------
//NOTE:Frequency of WIFI should be at 2.4GHz
const char* WIFI_SSID     = //WI-FI name
const char* WIFI_PASSWORD = // WI-FI password
const char* PREVIEW_KEY = "nasPreviewKey987" 
const char* HOSTNAME      = "mediaserver";   


#define SD_CS_PIN   5
#define SD_MOSI_PIN 23
#define SD_MISO_PIN 19
#define SD_SCK_PIN  18

// Authentication — change these before flashing!
const char* AUTH_USERNAME = "admin";
const char* AUTH_PASSWORD = "rohan123";

// Google Drive — from get_refresh_token.py output. Treat as secrets: don't share/commit.
const char* DRIVE_CLIENT_ID     = // Drive_Client_ID
const char* DRIVE_CLIENT_SECRET = // Drive_Client_Secret
const char* DRIVE_REFRESH_TOKEN = // Drive_Refresh_Token
const char* DRIVE_FOLDER_ID     = //Drive_folder_ID
// -----------------------------------------

AsyncWebServer server(80);
String driveAccessToken = "";
const char* SYNCED_FOLDER = "/synced";

// Folders we expect on the SD card
const char* FOLDERS[] = { "/documents", "/media/audio", "/media/video", "/synced" };
const int FOLDER_COUNT = 4;

void ensureFolders() {
  for (int i = 0; i < FOLDER_COUNT; i++) {
    if (!SD.exists(FOLDERS[i])) {
      // Create nested folders one level at a time
      String path = FOLDERS[i];
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

// Re-initializes the SD card connection. Use this after swapping the card
// without rebooting the ESP32. Best practice is still to power off before
// swapping, but this recovers the SD library's state either way.
bool remountSD() {
  SD.end();
  delay(300); // give the card a moment to settle
  bool ok = SD.begin(SD_CS_PIN);
  if (ok) {
    ensureFolders();
    Serial.println("SD card remounted successfully.");
  } else {
    Serial.println("SD remount failed — check the card is seated properly.");
  }
  return ok;
}

// Purely cosmetic helper: picks a small emoji icon based on file extension.
// Does not affect any file handling, only how the file is displayed.
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

// Purely cosmetic/UI helper: classifies a file so the front-end knows whether it can
// show an in-browser preview (image/video/audio/pdf/text/docx) or should fall back to
// normal link behavior. Does not change how any file is stored or served.
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
// Server-side MIME lookup for streaming audio/video with proper Content-Type + Range support
String getMimeForPath(const String& path) {
  String p = path;
  p.toLowerCase();
  if (p.endsWith(".mp3")) return "audio/mpeg";
  if (p.endsWith(".wav")) return "audio/wav";
  if (p.endsWith(".flac")) return "audio/flac";
  if (p.endsWith(".m4a")) return "audio/mp4";
  if (p.endsWith(".aac")) return "audio/aac";
  if (p.endsWith(".ogg")) return "audio/ogg";
  if (p.endsWith(".mp4")) return "video/mp4";
  if (p.endsWith(".mkv")) return "video/x-matroska";
  if (p.endsWith(".webm")) return "video/webm";
  if (p.endsWith(".mov")) return "video/quicktime";
  return "application/octet-stream";
}

// Purely cosmetic helper: friendlier size string. Same underlying size, just nicer formatting.
String getFileSizeStr(size_t bytes) {
  float kb = bytes / 1024.0;
  if (kb >= 1024.0) {
    return String(kb / 1024.0, 1) + " MB";
  }
  return String((int)kb) + " KB";
}

// Purely cosmetic helper: nicer folder titles for the UI.
String getFolderDisplayName(const char* folderPath) {
  String p = String(folderPath);
  if (p == "/documents") return "Documents";
  if (p == "/media/audio") return "Music";
  if (p == "/media/video") return "Videos";
  if (p == "/synced") return "Synced (Drive)";
  return p;
}

// Build a simple HTML listing for a given folder, now with delete/rename controls
String listFolder(const char* folderPath) {
  String folderPathStr = String(folderPath);
  String html = "<div class='card'><div class='card-header'>"
                "<h2>" + getFolderDisplayName(folderPath) + "</h2>"
                "<span class='card-path'>" + folderPathStr + "</span>"
                "</div><div class='file-list'>";
  File dir = SD.open(folderPath);
  if (!dir || !dir.isDirectory()) {
    return html + "<div class='empty-state'>Folder not found.</div></div></div>";
  }
  File entry = dir.openNextFile();
  bool any = false;
  while (entry) {
    String name = entry.name();
    if (!entry.isDirectory()) {
      any = true;
      String fullPath = folderPathStr + "/" + name;
      String kind = getFileKind(name);
      String safePath = escapeForJsAttr(fullPath);
      String safeName = escapeForJsAttr(name);
      String nameClickAttr = "";
      if (kind != "other") {
        nameClickAttr = " onclick=\"return openPreview(event,'" + safePath + "','" + kind + "');\"";
      }
      html += "<div class='file-row'>"
              "<a class='file-name' href=\"" + fullPath + "\"" + nameClickAttr + "><span class='file-icon'>" + getFileIcon(name) + "</span><span class='file-name-text'>" + name + "</span></a>"
              "<span class='file-size'>" + getFileSizeStr(entry.size()) + "</span>"
              "<span class='file-actions'>"
              "<a class='btn-icon' href=\"" + fullPath + "\" download title='Download'>&#11015;</a>"
              "<a class='btn-icon' href=\"#\" onclick=\"return pushToDrive('" + safePath + "');\" title='Upload to Drive'>&#9729;</a>"
              "<a class='btn-icon' href=\"#\" onclick=\"return renameFile('" + safePath + "','" + safeName + "');\" title='Rename'>&#9998;</a>"
              "<a class='btn-icon btn-danger' href=\"#\" onclick=\"return deleteFile('" + safePath + "','" + safeName + "');\" title='Delete'>&#128465;</a>"
              "</span>"
              "</div>";
    }
    entry = dir.openNextFile();
  }
  if (!any) {
    html += "<div class='empty-state'>No files yet.</div>";
  }
  html += "</div></div>";
  return html;
}

// Recursively search all known folders for files whose name contains the query (case-insensitive)
String searchFiles(const String& query) {
  String q = query;
  q.toLowerCase();
  String html = "<div class='card'><div class='card-header'><h2>Search results for \"" + query + "\"</h2></div><div class='file-list'>";
  int matches = 0;

  for (int i = 0; i < FOLDER_COUNT; i++) {
    File dir = SD.open(FOLDERS[i]);
    if (!dir || !dir.isDirectory()) continue;
    File entry = dir.openNextFile();
    while (entry) {
      if (!entry.isDirectory()) {
        String name = entry.name();
        String nameLower = name;
        nameLower.toLowerCase();
        if (nameLower.indexOf(q) != -1) {
          String fullPath = String(FOLDERS[i]) + "/" + name;
          String kind = getFileKind(name);
          String safePath = escapeForJsAttr(fullPath);
          String safeName = escapeForJsAttr(name);
          String nameClickAttr = "";
          if (kind != "other") {
            nameClickAttr = " onclick=\"return openPreview(event,'" + safePath + "','" + kind + "');\"";
          }
          html += "<div class='file-row'>"
                  "<a class='file-name' href=\"" + fullPath + "\"" + nameClickAttr + "><span class='file-icon'>" + getFileIcon(name) + "</span><span class='file-name-text'>" + fullPath + "</span></a>"
                  "<span class='file-size'>" + getFileSizeStr(entry.size()) + "</span>"
                  "<span class='file-actions'>"
                  "<a class='btn-icon' href=\"" + fullPath + "\" download title='Download'>&#11015;</a>"
                  "<a class='btn-icon btn-danger' href=\"#\" onclick=\"return deleteFile('" + safePath + "','" + safeName + "');\" title='Delete'>&#128465;</a>"
                  "</span>"
                  "</div>";
          matches++;
        }
      }
      entry = dir.openNextFile();
    }
  }

  if (matches == 0) {
    html += "<div class='empty-state'>No files matched.</div>";
  }
  html += "</div></div>";
  return html;
}

// ================= GOOGLE DRIVE SYNC (UNCHANGED — this part is working, left as-is) =================

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

  // STEP 1: start the resumable upload session.
  // Wrapped in { } so client1/http1 are destroyed (freeing their TLS memory)
  // before we open the second connection for the actual file transfer.
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
  } // client1 + http1 fully freed here

  yield();
  delay(100); // let memory/network settle before the big transfer

  // Parse the session URL into host + path so we can stream manually
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
    delay(1); // explicit breathing room every chunk, prevents watchdog trip
    size_t toRead = min((size_t)sizeof(buf), fileSize - sent);
    size_t readBytes = f.read(buf, toRead);
    if (readBytes == 0) break;
    client2.write(buf, readBytes);
    sent += readBytes;
  }
  f.close();

  // Read just the status line of the response
  unsigned long waitStart = millis();
  while (client2.connected() && !client2.available()) {
    if (millis() - waitStart > 15000) break; // 15s timeout waiting for response
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

// Main sync routine — UNCHANGED from your working version, including the
// "check account" and "list all files" debug steps you want kept as-is.
String syncWithDrive() {
  if (WiFi.status() != WL_CONNECTED) {
    return "No Wi-Fi connection.";
  }

  if (!refreshAccessToken()) {
    return "Failed to get Drive access token.";
  }

  Serial.println("\n========== ACCESS TOKEN OK ==========\n");

  // STEP 1: Check authenticated account
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

  // STEP 2: List all visible files
  {
    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;

    String testUrl =
      "https://www.googleapis.com/drive/v3/files"
      "?pageSize=20&fields=files(id,name)";

    http.begin(client, testUrl);
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

    Serial.println("\n===== ALL DRIVE FILES =====");
    Serial.println("HTTP Code: " + String(code));
    Serial.println(payload);
    yield();
    delay(1);
    Serial.println("===========================");

    http.end();
  }

  // STEP 3: Query sync folder
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
  DeserializationError err =
      deserializeJson(doc, payload);

  if (err) {
    Serial.println(err.c_str());
    return "JSON parse failed.";
  }

  JsonArray files =
      doc["files"].as<JsonArray>();

  Serial.println(
      "Files found in folder: " +
      String(files.size()));

  int downloaded = 0;
  int uploaded = 0;

  // Download files from Drive
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

  // Upload local files to Drive
  File dir = SD.open(SYNCED_FOLDER);

  if (dir) {
    File entry = dir.openNextFile();

    while (entry) {
      yield();
      delay(1);
      if (!entry.isDirectory()) {

        String name = entry.name();
        bool exists = false;

        for (JsonObject file : files) {
          if (file["name"].as<String>() == name) {
            exists = true;
            break;
          }
        }

        if (!exists) {
          entry.close();

          String fullPath =
            String(SYNCED_FOLDER) + "/" + name;

          if (uploadFileToDrive(fullPath))
            uploaded++;
        } else {
          entry.close();
        }
      } else {
        entry.close();
      }

      entry = dir.openNextFile();
    }
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
    ".upload-row select,.upload-row input[type=file]{padding:9px 12px;border-radius:10px;border:1px solid var(--card-border);background:var(--bg-soft);color:var(--text);font-size:0.88em;}"
    "a.back-link{color:var(--accent);text-decoration:none;font-size:0.9em;}"
    "@media (max-width:600px){.topbar{padding:12px 14px;}.container{padding:14px;}.file-size{display:none;}}"
    /* --- iOS-style toast with spinner --- */
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
    /* --- In-browser media preview modal --- */
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
    "</style>";
}

// Shared front-end JS for all pages: iOS-style toast/spinner feedback, an in-browser
// preview modal for audio/video/pdf, and AJAX wrappers for actions so the page never
// navigates away to a plain confirmation screen.
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
      "var doc=new DOMParser().parseFromString(html,'text/html');var ng=doc.querySelector('.grid');var cg=document.querySelector('.grid');"
      "if(ng&&cg){cg.innerHTML=ng.innerHTML;}}).catch(function(){});}"
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
        "content.innerHTML='<video controls preload=\"metadata\" playsinline><source src=\"'+freshPath+'\"'+(vmime?' type=\"'+vmime+'\"':'')+'>Your browser can not play this video.</video>';"
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
    "function submitUpload(ev){ev.preventDefault();var form=document.getElementById('uploadForm');"
      "var fileInput=form.querySelector('input[type=file]');if(!fileInput.files.length){toastDone('Choose a file first',false);return false;}"
      "var fd=new FormData(form);var xhr=new XMLHttpRequest();xhr.open('POST','/upload',true);"
      "xhr.upload.onprogress=function(e){if(e.lengthComputable){var pct=Math.round((e.loaded/e.total)*100);"
      "toastSpinner('Uploading... '+pct+'%');}};"
      "xhr.onload=function(){if(xhr.status===200){toastDone('Upload complete');form.reset();refreshListings();}"
      "else{toastDone('Upload failed',false);}};"
      "xhr.onerror=function(){toastDone('Upload failed',false);};"
      "toastSpinner('Uploading...');xhr.send(fd);return false;}"
    "</script>";
}

String buildHomePage() {
  String html = "<html><head><title>Home Media Server</title>"
                 "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += getPageStyle();
  html += "</head><body>";

  html += "<div class='topbar'>"
          "<h1>&#128187; Home Media Server</h1>"
          "<div class='actions'>"
          "<button class='btn btn-secondary' onclick='runSync()'>&#9729; Sync with Drive</button>"
          "<button class='btn btn-secondary' onclick='runRemount()'>&#128260; Remount SD</button>"
          "<button class='btn btn-secondary' onclick='refreshListings()'>&#8635; Refresh</button>"
          "</div>"
          "</div>";

  html += "<div class='container'>";

  html += "<form class='search-form' method='GET' action='/search'>"
          "<input type='text' name='q' placeholder='Search files across all folders...'>"
          "<button type='submit'>Search</button>"
          "</form>";

  html += "<div class='grid'>";
  html += listFolder("/documents");
  html += listFolder("/media/audio");
  html += listFolder("/media/video");
  html += listFolder("/synced");
  html += "</div>";

  html += "<div class='panel'>"
          "<h3>&#11014; Upload a file</h3>"
          "<form id='uploadForm' class='upload-row' onsubmit='return submitUpload(event);'>"
          "<select name='folder'>"
          "<option value='/documents'>Documents</option>"
          "<option value='/media/audio'>Music</option>"
          "<option value='/media/video'>Video</option>"
          "</select>"
          "<input type='file' name='file'>"
          "<button type='submit'>Upload</button>"
          "</form>"
          "</div>";

  html += "</div>";
  html += getPageScript();
  html += "</body></html>";
  return html;
}

void setup() {
  Serial.begin(115200);
  delay(500);

  // --- SD card init ---
  SPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
  if (!SD.begin(SD_CS_PIN)) {
    Serial.println("SD card mount failed! Check wiring.");
  } else {
    Serial.println("SD card mounted.");
    ensureFolders();
  }

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

  // --- mDNS so you can use http://mediaserver.local instead of typing the IP ---
  if (MDNS.begin(HOSTNAME)) {
    Serial.println("mDNS started: http://" + String(HOSTNAME) + ".local");
  }

  // --- Home page: lists all files (auth required) ---
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!request->authenticate(AUTH_USERNAME, AUTH_PASSWORD)) {
      return request->requestAuthentication();
    }
    request->send(200, "text/html", buildHomePage());
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
    bool allowed = path.startsWith("/media/audio/") || path.startsWith("/media/video/");
    if (!allowed || !SD.exists(path)) {
      request->send(404, "text/plain", "Not found.");
      return;
    }
    request->send(SD, path, getMimeForPath(path)); // supports Range requests natively — seeking works
  });

  // --- Serve files directly from SD with Range support (needed for video seeking) ---
  server.serveStatic("/documents", SD, "/documents").setAuthentication(AUTH_USERNAME, AUTH_PASSWORD);
  server.serveStatic("/media/audio", SD, "/media/audio").setAuthentication(AUTH_USERNAME, AUTH_PASSWORD);
  server.serveStatic("/media/video", SD, "/media/video").setAuthentication(AUTH_USERNAME, AUTH_PASSWORD);
  server.serveStatic("/synced", SD, "/synced").setAuthentication(AUTH_USERNAME, AUTH_PASSWORD);

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
    String html;
    if (SD.remove(path)) {
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
    String newPath = folder + "/" + newName;

    String html;
    if (SD.rename(path, newPath)) {
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
    String html = "<html><head><title>Search</title>"
                   "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += getPageStyle();
    html += "</head><body>"
            "<div class='topbar'><h1>&#128269; Search</h1>"
            "<div class='actions'><a class='btn btn-secondary' href='/'>&larr; Back to files</a></div></div>"
            "<div class='container'>";
    html += searchFiles(q);
    html += "</div>";
    html += getPageScript();
    html += "</body></html>";
    request->send(200, "text/html", html);
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
        if (request->hasParam("folder", true)) {
          targetFolder = request->getParam("folder", true)->value();
        }
        String path = targetFolder + "/" + filename;
        Serial.println("Upload start: " + path);
        uploadFile = SD.open(path, FILE_WRITE);
      }
      if (uploadFile) {
        uploadFile.write(data, len);
      }
      if (final) {
        if (uploadFile) uploadFile.close();
        Serial.println("Upload finished: " + filename);
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
        lastSyncResult = syncWithDrive();
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
        } else if (uploadFileToDrive(pushPath)) {
          lastPushResult = "Uploaded " + pushPath + " successfully.";
        } else {
          lastPushResult = "Upload failed for " + pushPath + ".";
        }

        Serial.println();
        Serial.println("================================");
        Serial.println(lastPushResult);
        Serial.println("================================");
    }

    delay(1);
    yield();
}
