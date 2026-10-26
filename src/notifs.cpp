#include "notifs.h"

#include <shlobj.h>

#include <algorithm>
#include <cstring>
#include <vector>
#include <cstdint>

#include "sqlite3.h"

namespace {

uint64_t nowFiletime() {
  FILETIME ft{};
  GetSystemTimePreciseAsFileTime(&ft);
  ULARGE_INTEGER value{};
  value.LowPart = ft.dwLowDateTime;
  value.HighPart = ft.dwHighDateTime;
  return value.QuadPart;
}

std::wstring widen(const char* utf8) {
  if (!utf8 || !*utf8) return L"";
  const int len = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
  if (len <= 1) return L"";
  std::wstring out(static_cast<size_t>(len), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out.data(), len);
  out.pop_back();
  return out;
}

std::wstring databasePath() {
  wchar_t local[MAX_PATH]{};
  if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, local))) return L"";
  return std::wstring(local) + L"\\Microsoft\\Windows\\Notifications\\wpndatabase.db";
}

// AUMIDs look like "5319275A.WhatsAppDesktop_cv1g1gvanyjgm!App" or, for unpackaged
// apps, a plain name such as "com.stablyai.orca". The friendly label drops the
// publisher hash and the "!EntryPoint" tail.
// Toast payloads are XML binding documents. Every visible string sits in a
// <text> element, optionally wrapped in runs/spans, so tags are stripped after
// extraction rather than parsed structurally - the schema varies per app.
std::vector<std::wstring> textsFromPayload(const std::string& xml) {
  std::vector<std::wstring> out;

  size_t pos = 0;
  while (out.size() < 4) {
    const size_t open = xml.find("<text", pos);
    if (open == std::string::npos) break;
    const size_t gt = xml.find('>', open);
    if (gt == std::string::npos) break;

    const size_t close = xml.find("</text>", gt);
    if (close == std::string::npos) break;

    std::string raw = xml.substr(gt + 1, close - gt - 1);

    // Strip any nested markup (<run>, <span>) and decode the few entities that
    // actually show up in notification text.
    std::string plain;
    plain.reserve(raw.size());
    bool inTag = false;
    for (char c : raw) {
      if (c == '<') {
        inTag = true;
      } else if (c == '>') {
        inTag = false;
      } else if (!inTag) {
        plain.push_back(c);
      }
    }

    struct Entity {
      const char* from;
      const char* to;
    };
    static const Entity kEntities[] = {
        {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&#39;", "'"}};
    for (const auto& e : kEntities) {
      size_t at = 0;
      while ((at = plain.find(e.from, at)) != std::string::npos) {
        plain.replace(at, strlen(e.from), e.to);
        at += strlen(e.to);
      }
    }

    if (!plain.empty()) out.push_back(widen(plain.c_str()));
    pos = close + 7;
  }
  return out;
}

std::wstring friendlyLabel(const std::wstring& aumid) {
  std::wstring base = aumid;
  const size_t bang = base.find(L'!');
  if (bang != std::wstring::npos) base = base.substr(0, bang);

  const size_t underscore = base.find(L'_');
  if (underscore != std::wstring::npos) base = base.substr(0, underscore);

  // "com.stablyai.orca" -> "orca"
  if (!base.empty() && base.find(L'.') != std::wstring::npos) {
    const size_t dot = base.find_last_of(L'.');
    if (dot + 1 < base.size()) base = base.substr(dot + 1);
  }
  return base;
}

}  // namespace

struct NotifSource::Impl {
  sqlite3* db = nullptr;
  std::wstring path;

  bool open() {
    if (db) return true;
    path = databasePath();
    if (path.empty()) return false;

    // Read-only, and never create: if the store does not exist there is simply
    // nothing to show.
    // sqlite3_open_v2 takes UTF-8 on Windows, so the path is converted rather
    // than passed through the ANSI code page.
    const std::string utf8 = toUtf8(path);
    const int flags = SQLITE_OPEN_READONLY;
    if (sqlite3_open_v2(utf8.c_str(), &db, flags, nullptr) != SQLITE_OK) {
      if (db) {
        sqlite3_close(db);
        db = nullptr;
      }
      return false;
    }

    // The store is written by another process; a short busy timeout avoids
    // failing a poll just because the service happened to be mid-commit.
    sqlite3_busy_timeout(db, 120);
    return true;
  }

  static std::string toUtf8(const std::wstring& wide) {
    const int len =
        WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return {};
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, out.data(), len, nullptr, nullptr);
    out.pop_back();
    return out;
  }

  void close() {
    if (db) {
      sqlite3_close(db);
      db = nullptr;
    }
  }
};

bool NotifSource::init() {
  impl_ = new Impl();
  return impl_->open();
}

void NotifSource::release() {
  if (!impl_) return;
  impl_->close();
  delete impl_;
  impl_ = nullptr;
}

void NotifSource::poll() {
  apps_.clear();
  if (!impl_) return;

  // A failed open is retried: the store is created the first time a
  // notification arrives after a fresh sign-in.
  if (!impl_->db && !impl_->open()) return;

  // Diagnostics can stretch the window so the icon row can be inspected on a
  // machine whose last notification was hours ago.
  int minutes = windowMinutes_;
  if (const wchar_t* forced = _wgetenv(L"GLANCE_NOTIF_MINUTES")) {
    const int v = _wtoi(forced);
    if (v > 0) minutes = v;
  }

  const uint64_t cutoff =
      nowFiletime() - static_cast<uint64_t>(minutes) * 60ull * 10000000ull;

  // Payload is only needed for the newest row per app, so it is fetched with a
  // correlated subquery rather than aggregated.
  static const char* kSql =
      "SELECT h.PrimaryId AS aumid, COUNT(*) AS c, MAX(n.ArrivalTime) AS last, "
      "  (SELECT n2.Payload FROM Notification n2 "
      "     WHERE n2.HandlerId = h.RecordId AND n2.ArrivalTime >= ?1 "
      "     ORDER BY n2.ArrivalTime DESC LIMIT 1) AS payload "
      "FROM Notification n JOIN NotificationHandler h ON h.RecordId = n.HandlerId "
      "WHERE n.ArrivalTime >= ?1 "
      "GROUP BY h.PrimaryId ORDER BY last DESC LIMIT 12";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(impl_->db, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
    // Schema changed (a future Windows build). Drop the handle so the next poll
    // reopens cleanly instead of hammering a broken statement.
    impl_->close();
    return;
  }

  sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(cutoff));

  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const unsigned char* raw = sqlite3_column_text(stmt, 0);
    if (!raw) continue;

    NotifApp app;
    app.aumid = widen(reinterpret_cast<const char*>(raw));
    if (app.aumid.empty()) continue;

    app.count = sqlite3_column_int(stmt, 1);
    app.lastArrival = static_cast<uint64_t>(sqlite3_column_int64(stmt, 2));
    app.label = friendlyLabel(app.aumid);

    const unsigned char* blob =
        static_cast<const unsigned char*>(sqlite3_column_blob(stmt, 3));
    const int blobLen = sqlite3_column_bytes(stmt, 3);
    if (blob && blobLen > 0) {
      const std::string xml(reinterpret_cast<const char*>(blob),
                            static_cast<size_t>(blobLen));
      const auto texts = textsFromPayload(xml);
      if (texts.size() >= 2) {
        app.title = texts[0];
        app.body = texts[1];
      } else if (texts.size() == 1) {
        app.title = texts[0];
      }
    }
    apps_.push_back(std::move(app));
  }

  sqlite3_finalize(stmt);
}
