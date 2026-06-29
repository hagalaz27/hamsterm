#include "HttpdCmds.h"
#include "Helpers.h"

#include <WebServer.h>
#include <WiFi.h>
#include <FS.h>
#include <vector>

static WebServer* g_server = nullptr;
static std::string g_root = "/";        // served subtree; "/" = internal + /sd
static File        g_upFile;            // current upload target
static const size_t EDIT_MAX = 32768;   // max bytes for in-browser edit/save

// ---- embedded UI (file manager) -------------------------------------------
static const char INDEX_HTML[] PROGMEM = R"HTML(<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>hamsTerm</title>
<style>
body{font-family:ui-monospace,monospace;background:#0a0d0b;color:#d8e6d2;margin:0;padding:14px}
a{color:#ff9f1c;text-decoration:none;cursor:pointer}a:hover{text-decoration:underline}.d{color:#7fd6a8}
h1{font-size:17px;margin:0 0 8px;color:#fff}
#nav{margin-bottom:8px;font-size:13px}#nav .navrow{margin-bottom:4px}#nav a,#nav label{margin-right:14px;color:#ff9f1c;cursor:pointer}#nav label:hover{text-decoration:underline}
#cur{color:#6f8;margin-bottom:6px;font-size:13px}
.row{padding:5px 0;border-bottom:1px solid #18201a;display:flex;justify-content:space-between;gap:10px;align-items:center}
.row>span:first-child{flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.row>span:last-child{flex-shrink:0;white-space:nowrap}
.sz{color:#5c6b5a;font-size:12px}.x{color:#e06b6b}
#ed{display:none;position:fixed;inset:0;background:#0a0d0b;padding:14px;box-sizing:border-box}
#ed textarea{width:100%;height:78%;background:#070907;color:#d8e6d2;border:1px solid #1b241b;font-family:ui-monospace,monospace;font-size:13px;box-sizing:border-box}
#edbar{margin-top:8px}#edbar button{font-family:inherit;background:#16201a;color:#ff9f1c;border:1px solid #2a3a2a;padding:6px 14px;margin-right:8px;cursor:pointer}
.mn{color:#9ab59a;font-weight:700;letter-spacing:1px}
#act{display:none;position:fixed;inset:0;background:rgba(0,0,0,.6)}
#actbox{position:absolute;left:50%;top:42%;transform:translate(-50%,-50%);background:#10160f;border:1px solid #2a3a2a;padding:14px;min-width:210px;max-width:90%}
#actname{color:#6f8;margin-bottom:10px;font-size:13px;word-break:break-all}
#actbox button{display:block;width:100%;font-family:inherit;background:#16201a;color:#ff9f1c;border:1px solid #2a3a2a;padding:9px;margin-top:8px;cursor:pointer}
#actbox button.x{color:#e06b6b}
</style></head><body>
<h1>hamsTerm</h1>
<div id="nav"></div><div id="cur"></div><div id="list"></div>
<div id="ed"><div id="edname" style="color:#6f8;margin-bottom:6px"></div>
<textarea id="edarea" spellcheck="false"></textarea>
<div id="edbar"><button onclick="save()">Save</button><button onclick="closeEd()">Cancel</button></div></div>
<div id="act"><div id="actbox"><div id="actname"></div>
<button id="actDownload" onclick="actDownload()">Download</button>
<button onclick="actRename()">Rename</button>
<button class="x" onclick="actDel()">Delete</button>
<button onclick="closeAct()">Cancel</button></div></div>
<script>
var ROOT='/';
function esc(s){return s.replace(/[&<>"]/g,function(c){return{'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]})}
function row(a,b){return '<div class="row"><span>'+a+'</span><span>'+b+'</span></div>'}
function post(u){return fetch(u,{method:'POST'})}
function go(p){
 fetch('/ls?path='+encodeURIComponent(p)).then(function(r){return r.json()}).then(function(j){
  ROOT=j.root;
  document.getElementById('cur').textContent=j.path;
  var nav='';
  if(ROOT==='/'){nav+='<div class="navrow"><a onclick="go(\'/\')">[ internal ]</a><a onclick="go(\'/sd\')">[ sd ]</a></div>';}
  nav+='<div class="navrow"><a onclick="newfile(\''+j.path+'\')">[ + file ]</a><a onclick="mkdir(\''+j.path+'\')">[ + dir ]</a><label>[ + upload ]<input type="file" style="display:none" onchange="upload(this,\''+j.path+'\')"></label></div>';
  document.getElementById('nav').innerHTML=nav;
  var h='';
  if(j.path!==ROOT){var up=j.path.replace(/\/[^/]*$/,'')||'/';h+=row('<a onclick="go(\''+up+'\')">../</a>','');}
  j.entries.forEach(function(e){
   var base=j.path==='/'?'':j.path;var full=base+'/'+e.name;
   var mn='<a class="mn" onclick="menu(\''+full+'\','+(e.dir?'1':'0')+')">...</a>';
   if(e.dir){h+=row('<a class="d" onclick="go(\''+full+'\')">'+esc(e.name)+'/</a>',mn);}
   else{h+=row('<a onclick="edit(\''+full+'\')">'+esc(e.name)+'</a>','<span class="sz">'+e.size+' B</span> '+mn);}
  });
  document.getElementById('list').innerHTML=h||row('(empty)','');
 }).catch(function(){document.getElementById('list').textContent='error'})
}
function newfile(dir){var n=prompt('New file name');if(!n)return;post('/newfile?path='+encodeURIComponent(dir.replace(/\/$/,'')+'/'+n)).then(function(){go(dir)})}
function mkdir(dir){var n=prompt('New folder name');if(!n)return;post('/mkdir?path='+encodeURIComponent(dir.replace(/\/$/,'')+'/'+n)).then(function(){go(dir)})}
function rm(p){var par=p.replace(/\/[^/]*$/,'')||'/';post('/rm?path='+encodeURIComponent(p)).then(function(){go(par)})}
var actPath='';
function menu(p,isDir){actPath=p;document.getElementById('actname').textContent=p;document.getElementById('actDownload').style.display=isDir?'none':'';document.getElementById('act').style.display='block';}
function closeAct(){document.getElementById('act').style.display='none';}
function actDownload(){closeAct();window.location='/dl?path='+encodeURIComponent(actPath)}
function actRename(){var base=actPath.replace(/.*\//,'');var nn=prompt('Rename to',base);if(!nn||nn===base)return;var par=actPath.replace(/\/[^/]*$/,'')||'/';var to=(par==='/'?'':par)+'/'+nn;closeAct();post('/rename?from='+encodeURIComponent(actPath)+'&to='+encodeURIComponent(to)).then(function(){go(par)})}
function actDel(){var p=actPath;if(!confirm('Delete '+p+' ?'))return;closeAct();rm(p)}
function upload(inp,dir){var f=inp.files[0];if(!f)return;var fd=new FormData();fd.append('f',f);fetch('/upload?path='+encodeURIComponent(dir),{method:'POST',body:fd}).then(function(){go(dir)})}
function edit(p){fetch('/raw?path='+encodeURIComponent(p)).then(function(r){if(!r.ok)throw 0;return r.text()}).then(function(t){
 document.getElementById('edname').textContent=p;var a=document.getElementById('edarea');a.value=t;a.dataset.path=p;
 document.getElementById('ed').style.display='block';}).catch(function(){alert('cannot edit (too large or not text)')})}
function save(){var a=document.getElementById('edarea');fetch('/save?path='+encodeURIComponent(a.dataset.path),{method:'POST',body:a.value}).then(function(){var par=a.dataset.path.replace(/\/[^/]*$/,'')||'/';closeEd();go(par)})}
function closeEd(){document.getElementById('ed').style.display='none'}
go('/');
</script></body></html>)HTML";

// ---- helpers ---------------------------------------------------------------
static std::string jsonEscape(const std::string& s) {
    std::string o; o.reserve(s.size() + 4);
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (c == '\n') o += "\\n";
        else if ((unsigned char)c < 0x20) { /* drop */ }
        else o += c;
    }
    return o;
}
static std::string baseName(const std::string& p) {
    size_t s = p.find_last_of('/');
    return (s == std::string::npos) ? p : p.substr(s + 1);
}
// Collapse '.', '..', '//' into a clean absolute path (no trailing slash).
static std::string normalizePath(const std::string& in) {
    std::vector<std::string> parts; size_t i = 0;
    while (i < in.size()) {
        while (i < in.size() && in[i] == '/') i++;
        size_t j = i; while (j < in.size() && in[j] != '/') j++;
        std::string seg = in.substr(i, j - i); i = j;
        if (seg == "" || seg == ".") continue;
        if (seg == "..") { if (!parts.empty()) parts.pop_back(); }
        else parts.push_back(seg);
    }
    std::string out = "/";
    for (size_t k = 0; k < parts.size(); k++) { out += parts[k]; if (k + 1 < parts.size()) out += "/"; }
    return out;
}
// Resolve a requested path and confine it to g_root. Returns false if it escapes.
static bool confine(const std::string& req, std::string& real) {
    std::string norm = normalizePath(req.empty() ? "/" : req);
    if (g_root == "/") { real = norm; return true; }
    if (norm == g_root || norm.compare(0, g_root.size() + 1, g_root + "/") == 0) { real = norm; return true; }
    return false;
}

static bool apActive() {
    wifi_mode_t m = WiFi.getMode();
    return (m == WIFI_MODE_AP || m == WIFI_MODE_APSTA);
}
static bool staActive() { return WiFi.status() == WL_CONNECTED; }
static void emitEndpoints(LineCallback emit) {
    char b[80];
    if (apActive())  { snprintf(b, sizeof(b), "  AP:   http://%s/\n", WiFi.softAPIP().toString().c_str()); emit(b); }
    if (staActive()) { snprintf(b, sizeof(b), "  wifi: http://%s/\n", WiFi.localIP().toString().c_str());  emit(b); }
}

// ---- route handlers --------------------------------------------------------
static void handleRoot() { g_server->send_P(200, "text/html", INDEX_HTML); }

static void handleLs() {
    std::string req = g_server->hasArg("path") ? std::string(g_server->arg("path").c_str()) : g_root;
    std::string real;
    if (!confine(req, real)) real = g_root; // clamp to root
    std::string fsPath; fs::FS& F = Helpers::fsFor(real, fsPath);
    File dir = F.open(fsPath.c_str());
    std::string json = "{\"root\":\"" + jsonEscape(g_root) + "\",\"path\":\"" + jsonEscape(real) + "\",\"entries\":[";
    bool first = true;
    if (dir && dir.isDirectory()) {
        File e = dir.openNextFile();
        while (e) {
            std::string nm = baseName(std::string(e.name()));
            if (!nm.empty()) {
                if (!first) json += ",";
                first = false;
                json += "{\"name\":\"" + jsonEscape(nm) + "\",\"dir\":" + (e.isDirectory() ? "true" : "false") +
                        ",\"size\":" + std::to_string((uint32_t)e.size()) + "}";
            }
            e = dir.openNextFile();
        }
    }
    json += "]}";
    if (dir) dir.close();
    g_server->send(200, "application/json", json.c_str());
}

static void handleDl() {
    std::string real;
    if (!g_server->hasArg("path") || !confine(std::string(g_server->arg("path").c_str()), real)) {
        g_server->send(403, "text/plain", "denied"); return;
    }
    std::string fsPath; fs::FS& F = Helpers::fsFor(real, fsPath);
    File f = F.open(fsPath.c_str(), "r");
    if (!f || f.isDirectory()) { if (f) f.close(); g_server->send(404, "text/plain", "not found"); return; }
    String fname = baseName(real).c_str();
    g_server->sendHeader("Content-Disposition", "attachment; filename=\"" + fname + "\"");
    g_server->streamFile(f, "application/octet-stream");
    f.close();
}

static void handleRaw() {
    std::string real;
    if (!g_server->hasArg("path") || !confine(std::string(g_server->arg("path").c_str()), real)) {
        g_server->send(403, "text/plain", "denied"); return;
    }
    std::string fsPath; fs::FS& F = Helpers::fsFor(real, fsPath);
    File f = F.open(fsPath.c_str(), "r");
    if (!f || f.isDirectory()) { if (f) f.close(); g_server->send(404, "text/plain", "not found"); return; }
    if (f.size() > EDIT_MAX) { f.close(); g_server->send(413, "text/plain", "too large to edit"); return; }
    g_server->streamFile(f, "text/plain");
    f.close();
}

static void handleSave() {
    std::string real;
    if (!g_server->hasArg("path") || !confine(std::string(g_server->arg("path").c_str()), real)) {
        g_server->send(403, "text/plain", "denied"); return;
    }
    if (!g_server->hasArg("plain")) { g_server->send(400, "text/plain", "no body"); return; }
    String body = g_server->arg("plain");
    if ((size_t)body.length() > EDIT_MAX) { g_server->send(413, "text/plain", "too large"); return; }
    std::string fsPath; fs::FS& F = Helpers::fsFor(real, fsPath);
    File f = F.open(fsPath.c_str(), "w");
    if (!f) { g_server->send(500, "text/plain", "open failed"); return; }
    f.write((const uint8_t*)body.c_str(), body.length());
    f.close();
    g_server->send(200, "text/plain", "ok");
}

static void handleMkdir() {
    std::string real;
    if (!g_server->hasArg("path") || !confine(std::string(g_server->arg("path").c_str()), real)) {
        g_server->send(403, "text/plain", "denied"); return;
    }
    std::string fsPath; fs::FS& F = Helpers::fsFor(real, fsPath);
    bool ok = F.mkdir(fsPath.c_str());
    g_server->send(ok ? 200 : 500, "text/plain", ok ? "ok" : "mkdir failed");
}

static void handleNewFile() {
    std::string real;
    if (!g_server->hasArg("path") || !confine(std::string(g_server->arg("path").c_str()), real)) {
        g_server->send(403, "text/plain", "denied"); return;
    }
    std::string fsPath; fs::FS& F = Helpers::fsFor(real, fsPath);
    if (F.exists(fsPath.c_str())) { g_server->send(409, "text/plain", "already exists"); return; }
    File f = F.open(fsPath.c_str(), "w"); // create an empty file
    if (!f) { g_server->send(500, "text/plain", "create failed"); return; }
    f.close();
    g_server->send(200, "text/plain", "ok");
}

// Delete a file, or a directory and everything inside it. Children are
// collected first (don't mutate a directory while iterating it), then removed.
static bool removeRecursive(fs::FS& F, const std::string& path) {
    File f = F.open(path.c_str());
    if (!f) return false;
    if (!f.isDirectory()) { f.close(); return F.remove(path.c_str()); }

    std::vector<std::pair<std::string, bool>> kids; // (full path, isDir)
    File e = f.openNextFile();
    while (e) {
        std::string child = path;
        if (child.empty() || child.back() != '/') child += "/";
        child += baseName(std::string(e.name()));
        kids.push_back(std::make_pair(child, e.isDirectory()));
        e = f.openNextFile();
    }
    f.close();

    bool ok = true;
    for (size_t i = 0; i < kids.size(); i++) {
        if (kids[i].second) ok = removeRecursive(F, kids[i].first) && ok;
        else                ok = F.remove(kids[i].first.c_str()) && ok;
    }
    if (ok) ok = F.rmdir(path.c_str());
    return ok;
}

static void handleRm() {
    std::string real;
    if (!g_server->hasArg("path") || !confine(std::string(g_server->arg("path").c_str()), real)) {
        g_server->send(403, "text/plain", "denied"); return;
    }
    if (real == g_root) { g_server->send(403, "text/plain", "cannot delete root"); return; }
    std::string fsPath; fs::FS& F = Helpers::fsFor(real, fsPath);
    bool ok = removeRecursive(F, fsPath); // handles files and non-empty dirs
    g_server->send(ok ? 200 : 500, "text/plain", ok ? "ok" : "delete failed");
}

static void handleRename() {
    if (!g_server->hasArg("from") || !g_server->hasArg("to")) {
        g_server->send(400, "text/plain", "missing args"); return;
    }
    std::string fromReal, toReal;
    if (!confine(std::string(g_server->arg("from").c_str()), fromReal) ||
        !confine(std::string(g_server->arg("to").c_str()),   toReal)) {
        g_server->send(403, "text/plain", "denied"); return;
    }
    // from and to share a parent dir (built by the page), so the same FS.
    std::string p1, p2;
    fs::FS& F = Helpers::fsFor(fromReal, p1);
    Helpers::fsFor(toReal, p2);
    bool ok = F.rename(p1.c_str(), p2.c_str());
    g_server->send(ok ? 200 : 500, "text/plain", ok ? "ok" : "rename failed");
}

static void handleUploadFinish() { g_server->send(200, "text/plain", "ok"); }
static void handleUploadChunk() {
    HTTPUpload& up = g_server->upload();
    if (up.status == UPLOAD_FILE_START) {
        std::string dir = g_server->hasArg("path") ? std::string(g_server->arg("path").c_str()) : g_root;
        std::string real;
        if (!confine(dir, real)) return; // outside root -> file stays closed
        std::string target = real;
        if (target.empty() || target.back() != '/') target += "/";
        target += std::string(up.filename.c_str());
        std::string fsPath; fs::FS& F = Helpers::fsFor(target, fsPath);
        g_upFile = F.open(fsPath.c_str(), "w");
    } else if (up.status == UPLOAD_FILE_WRITE) {
        if (g_upFile) {
            size_t off = 0;
            while (off < up.currentSize) {           // handle short writes
                size_t w = g_upFile.write(up.buf + off, up.currentSize - off);
                if (w == 0) break;
                off += w;
            }
        }
    } else if (up.status == UPLOAD_FILE_END) {
        if (g_upFile) g_upFile.close();
    }
}

// ---- command ---------------------------------------------------------------
void HttpdCmds::start(LineCallback emit, const std::string& rootArg) {
    if (g_server) { emit("httpd: already running\n"); Helpers::cmd_status = 1; return; }
    if (!apActive() && !staActive()) {
        emit("httpd: no network - start an AP ('ap ...') or connect Wi-Fi ('wf c ...')\n");
        Helpers::cmd_status = 1;
        return;
    }
    std::string root = "/";
    if (!rootArg.empty()) {
        root = normalizePath(rootArg);
        std::string fsPath; fs::FS& F = Helpers::fsFor(root, fsPath);
        File d = F.open(fsPath.c_str());
        if (!d || !d.isDirectory()) { if (d) d.close(); emit("httpd: not a directory: " + root + "\n"); Helpers::cmd_status = 1; return; }
        d.close();
    }
    g_root = root;

    g_server = new WebServer(80);
    g_server->on("/",      handleRoot);
    g_server->on("/ls",    handleLs);
    g_server->on("/dl",    handleDl);
    g_server->on("/raw",   handleRaw);
    g_server->on("/save",  HTTP_POST, handleSave);
    g_server->on("/mkdir", HTTP_POST, handleMkdir);
    g_server->on("/newfile", HTTP_POST, handleNewFile);
    g_server->on("/rm",    HTTP_POST, handleRm);
    g_server->on("/rename", HTTP_POST, handleRename);
    g_server->on("/upload", HTTP_POST, handleUploadFinish, handleUploadChunk);
    g_server->onNotFound([]() { g_server->send(404, "text/plain", "Not found"); });
    g_server->begin();

    emit("httpd: serving " + g_root + " on port 80\n");
    emitEndpoints(emit);
}

void HttpdCmds::stop(LineCallback emit) {
    if (!g_server) { emit("httpd: not running\n"); return; }
    g_server->stop();
    delete g_server; g_server = nullptr;
    emit("httpd: stopped\n");
}

void HttpdCmds::status(LineCallback emit) {
    if (g_server) {
        emit("httpd: running, root " + g_root + "\n");
        emitEndpoints(emit);
    } else {
        emit("httpd: stopped\n");
    }
}

void HttpdCmds::loop() { if (g_server) g_server->handleClient(); }
bool HttpdCmds::running() { return g_server != nullptr; }

void HttpdCmds::httpd(const std::string& args, LineCallback emit) {
    size_t i = 0; while (i < args.size() && args[i] == ' ') i++;
    size_t j = i; while (j < args.size() && args[j] != ' ') j++;
    std::string sub = args.substr(i, j - i);
    size_t k = j; while (k < args.size() && args[k] == ' ') k++;
    std::string rest = (k < args.size()) ? args.substr(k) : "";
    // trim trailing spaces of rest
    while (!rest.empty() && rest.back() == ' ') rest.pop_back();

    if (sub == "start")                      start(emit, rest);
    else if (sub == "stop")                  stop(emit);
    else if (sub.empty() || sub == "status") status(emit);
    else { emit("Usage: httpd [start [path] | stop | status]\n"); Helpers::cmd_status = 1; }
}
