#pragma once

#include <M5Cardputer.h>
#include <string>
#include <vector>
#include "Helpers.h"

extern "C" {
#include <libssh/libssh.h>
}

// scp over the same LibSSH-ESP32 stack as the ssh client. This version does
// downloads only (remote -> local); uploads are parsed and reported as
// not-yet-supported. The transfer is streamed in small chunks so a big file
// never sits in RAM (the device has no PSRAM). The classic SCP protocol is used
// (ssh_scp_*); very new OpenSSH servers that dropped it would need the SFTP
// backend instead - a future addition.

struct ScpArgs {
    bool ok = false;
    bool download = false;     // true: remote->local; false: local->remote (upload)
    std::string user, host, remotePath, localPath;
    uint16_t port = 22;
    std::string err;           // set when ok == false ("usage" -> print usage)
};

// A token is remote when it has a ':' that comes before any '/', i.e. the
// "[user@]host:path" form. A plain "/local/path" has no such colon.
inline bool scp_is_remote(const std::string& tok) {
    size_t colon = tok.find(':');
    if (colon == std::string::npos) return false;
    size_t slash = tok.find('/');
    return slash == std::string::npos || colon < slash;
}

inline void scp_split_remote(const std::string& tok, std::string& user,
                             std::string& host, std::string& path) {
    size_t colon = tok.find(':');
    std::string hostpart = tok.substr(0, colon);
    path = tok.substr(colon + 1);
    size_t at = hostpart.find('@');
    if (at != std::string::npos) { user = hostpart.substr(0, at); host = hostpart.substr(at + 1); }
    else host = hostpart;
}

// scp [-p PORT] SOURCE DEST  - one side is [user@]host:path, the other local.
inline ScpArgs scp_parse(const std::vector<std::string>& toks) {
    ScpArgs a;
    std::vector<std::string> pos;
    for (size_t i = 0; i < toks.size(); ++i) {
        if (toks[i] == "-p") {
            if (i + 1 >= toks.size()) { a.err = "scp: -p needs a port"; return a; }
            long v;
            if (!Helpers::parse_uint(toks[++i], v) || v < 1 || v > 65535) {
                a.err = "scp: invalid port: " + toks[i]; return a;
            }
            a.port = (uint16_t)v;
        } else {
            pos.push_back(toks[i]);
        }
    }
    if (pos.size() != 2) { a.err = "usage"; return a; }

    bool srcRemote = scp_is_remote(pos[0]);
    bool dstRemote = scp_is_remote(pos[1]);
    if (srcRemote && !dstRemote) {
        a.download = true;
        scp_split_remote(pos[0], a.user, a.host, a.remotePath);
        a.localPath = pos[1];
    } else if (!srcRemote && dstRemote) {
        a.download = false; // upload
        scp_split_remote(pos[1], a.user, a.host, a.remotePath);
        a.localPath = pos[0];
    } else {
        a.err = "scp: one side must be [user@]host:path and the other a local path";
        return a;
    }

    if (a.host.empty())       { a.err = "scp: no host"; return a; }
    if (a.remotePath.empty()) { a.err = "scp: no remote path"; return a; }
    if (a.user.empty())       { a.err = "scp: no user (use user@host)"; return a; }
    a.ok = true;
    return a;
}

// Split an upload destination into the remote directory (passed to scp -t) and
// the target filename (sent with the file record). A trailing '/' means "a
// directory - keep the local name"; otherwise the last path component is the
// target filename. So `dir/` uses localBase, `dir/name` writes `name`.
inline void scp_split_target(const std::string& remotePath, const std::string& localBase,
                             std::string& dir, std::string& file) {
    if (!remotePath.empty() && remotePath.back() == '/') {
        dir = remotePath.substr(0, remotePath.size() - 1);
        if (dir.empty()) dir = "/";
        file = localBase;
    } else {
        size_t slash = remotePath.find_last_of('/');
        if (slash == std::string::npos) { dir = "."; file = remotePath; }
        else { dir = (slash == 0) ? "/" : remotePath.substr(0, slash);
               file = remotePath.substr(slash + 1); }
    }
}

class ScpClient {
public:
    // Download remotePath -> localAbs. Blocking (DNS + TCP + key exchange + auth
    // + transfer); can take several seconds. Streams in chunks. Returns false on
    // error (see error()); on success transferred() is the byte count.
    bool download(const std::string& host, const std::string& user,
                  const std::string& password, uint16_t port,
                  const std::string& remotePath, const std::string& localAbs,
                  LineCallback emit) {
        ssh_session session = connect_auth(host, user, password, port);
        if (!session) return false; // err already set

        ssh_scp scp = ssh_scp_new(session, SSH_SCP_READ, remotePath.c_str());
        if (!scp || ssh_scp_init(scp) != SSH_OK) {
            err = std::string("scp: ") + ssh_get_error(session);
            if (scp) ssh_scp_free(scp);
            ssh_disconnect(session); ssh_free(session); return false;
        }

        int rc = ssh_scp_pull_request(scp);
        if (rc != SSH_SCP_REQUEST_NEWFILE) {
            err = (rc == SSH_SCP_REQUEST_NEWDIR)
                    ? "scp: remote is a directory (only single files are supported)"
                    : std::string("scp: ") + ssh_get_error(session);
            ssh_scp_close(scp); ssh_scp_free(scp);
            ssh_disconnect(session); ssh_free(session); return false;
        }

        size_t size = ssh_scp_request_get_size(scp);
        ssh_scp_accept_request(scp);

        File out = Helpers::fsOpen(localAbs, "w");
        if (!out) {
            err = "scp: cannot write " + Helpers::clearFilename(localAbs);
            ssh_scp_close(scp); ssh_scp_free(scp);
            ssh_disconnect(session); ssh_free(session); return false;
        }

        char buf[1024];
        size_t total = 0;
        bool ioError = false;
        while (total < size) {
            size_t want = size - total;
            if (want > sizeof(buf)) want = sizeof(buf);
            int r = ssh_scp_read(scp, buf, want);
            if (r == SSH_ERROR || r <= 0) break;
            if ((int)out.write((const uint8_t*)buf, r) != r) { ioError = true; break; }
            total += (size_t)r;
            if ((total & 0x0FFF) == 0) delay(1); // feed the watchdog on big files
        }
        out.close();
        ssh_scp_close(scp);
        ssh_scp_free(scp);
        ssh_disconnect(session);
        ssh_free(session);

        if (ioError) { err = "scp: write error (disk full?)"; return false; }
        if (total < size) {
            err = "scp: transfer incomplete (" + std::to_string(total) + "/" +
                  std::to_string(size) + " bytes)";
            return false;
        }
        bytes = total;
        return true;
    }

    // Upload localAbs -> remotePath. Blocking, streamed in chunks. remotePath
    // ending in '/' means a directory (keeps the local name); otherwise its last
    // component is the target filename.
    bool upload(const std::string& host, const std::string& user,
                const std::string& password, uint16_t port,
                const std::string& localAbs, const std::string& remotePath,
                LineCallback emit) {
        File in = Helpers::fsOpen(localAbs, "r");
        if (!in || in.isDirectory()) {
            if (in) in.close();
            err = "scp: cannot read " + Helpers::clearFilename(localAbs);
            return false;
        }
        size_t size = in.size();

        // local basename for the "upload into a directory" case
        std::string localBase = localAbs;
        size_t ls = localBase.find_last_of('/');
        if (ls != std::string::npos) localBase = localBase.substr(ls + 1);

        std::string dir, file;
        scp_split_target(remotePath, localBase, dir, file);

        ssh_session session = connect_auth(host, user, password, port);
        if (!session) { in.close(); return false; }

        ssh_scp scp = ssh_scp_new(session, SSH_SCP_WRITE, dir.c_str());
        if (!scp || ssh_scp_init(scp) != SSH_OK) {
            err = std::string("scp: ") + ssh_get_error(session);
            if (scp) ssh_scp_free(scp);
            ssh_disconnect(session); ssh_free(session); in.close(); return false;
        }

        if (ssh_scp_push_file(scp, file.c_str(), size, 0644) != SSH_OK) {
            err = std::string("scp: ") + ssh_get_error(session);
            ssh_scp_close(scp); ssh_scp_free(scp);
            ssh_disconnect(session); ssh_free(session); in.close(); return false;
        }

        char buf[1024];
        size_t total = 0;
        bool netError = false;
        while (total < size) {
            int r = in.read((uint8_t*)buf, sizeof(buf));
            if (r <= 0) break;
            if (ssh_scp_write(scp, buf, r) != SSH_OK) { netError = true; break; }
            total += (size_t)r;
            if ((total & 0x0FFF) == 0) delay(1); // feed the watchdog on big files
        }
        in.close();
        ssh_scp_close(scp);
        ssh_scp_free(scp);
        ssh_disconnect(session);
        ssh_free(session);

        if (netError) { err = "scp: send failed"; return false; }
        if (total < size) {
            err = "scp: read incomplete (" + std::to_string(total) + "/" +
                  std::to_string(size) + " bytes) - SD read bug on large files?";
            return false;
        }
        bytes = total;
        return true;
    }

    size_t transferred() const { return bytes; }
    const std::string& error() const { return err; }

private:
    // DNS + TCP + key exchange + password auth. Returns the session, or nullptr
    // with err set. Shared by download() and upload().
    ssh_session connect_auth(const std::string& host, const std::string& user,
                             const std::string& password, uint16_t port) {
        ssh_session session = ssh_new();
        if (!session) { err = "scp: out of memory"; return nullptr; }

        ssh_options_set(session, SSH_OPTIONS_HOST, host.c_str());
        unsigned int p = port;
        ssh_options_set(session, SSH_OPTIONS_PORT, &p);
        ssh_options_set(session, SSH_OPTIONS_USER, user.c_str());
        long timeout = 15;
        ssh_options_set(session, SSH_OPTIONS_TIMEOUT, &timeout);
        ssh_options_set(session, SSH_OPTIONS_COMPRESSION, "no");

        if (ssh_connect(session) != SSH_OK) {
            err = std::string("scp: ") + ssh_get_error(session);
            ssh_free(session); return nullptr;
        }
        // Host key intentionally not verified (as in the ssh client).
        if (ssh_userauth_password(session, NULL, password.c_str()) != SSH_AUTH_SUCCESS) {
            err = std::string("scp: auth failed: ") + ssh_get_error(session);
            ssh_disconnect(session); ssh_free(session); return nullptr;
        }
        return session;
    }

    std::string err;
    size_t bytes = 0;
};
