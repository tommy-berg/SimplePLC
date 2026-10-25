#include "web/auth.h"

#include <algorithm>
#include <format>

#include <nlohmann/json.hpp>

#include "app/log.h"
#include "runtime/program_store.h"
#include "web/crypto.h"

namespace simpleplc::web {
namespace {

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

int64_t epochMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string randomHex(size_t bytes) { return crypto::toHex(crypto::randomBytes(bytes)); }

bool validUserName(const std::string& name) {
    return !name.empty() && name.size() <= 64 && std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '_' || c == '-' || c == '.' || c == '@';
    });
}

constexpr auto kSessionLifetime = std::chrono::hours(24 * 7);
constexpr size_t kMaxSessions = 1000;
constexpr auto kFailureWindow = std::chrono::minutes(10);
constexpr auto kBlockTime = std::chrono::seconds(30);
/// A SIMPLEPLC_TOKEN this long cannot be found by trying (the start-up warning asks for it).
constexpr size_t kUnguessableLength = 16;

/// Characters (UTF-8 code points), not bytes, so a limit means what it says.
size_t characters(std::string_view s) {
    return static_cast<size_t>(std::count_if(s.begin(), s.end(), [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }));
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

/// Passwords of 8 or more characters that lists of leaked passwords put first; an administrator of
/// a controller must not choose one of them (compared without case).
constexpr std::string_view kCommonPasswords[] = {
    "password", "password1", "password12", "password123", "password!", "passw0rd", "p@ssw0rd", "p@ssword",
    "12345678", "123456789", "1234567890", "12341234", "123123123", "11111111", "00000000", "87654321",
    "88888888", "99999999", "12121212", "1q2w3e4r", "1q2w3e4r5t", "1qaz2wsx", "1qazxsw2", "zaq12wsx",
    "qwertyui", "qwertyuiop", "qwerty12", "qwerty123", "asdfghjk", "asdfghjkl", "zxcvbnm1", "abcdefgh",
    "abcd1234", "abc12345", "abc123456", "a1b2c3d4", "iloveyou", "iloveyou1", "sunshine", "princess",
    "football", "baseball", "superman", "starwars", "trustno1", "whatever", "welcome1", "welcome123",
    "letmein1", "letmein123", "changeme", "changeit", "administrator", "admin123", "admin1234", "adminadmin",
    "rootroot", "root1234", "toortoor", "default1", "guest123", "master12", "mustang1", "michael1",
    "jennifer", "computer", "internet", "monkey12", "dragon12", "shadow12", "secret12", "secret123",
    "test1234", "testtest", "operator", "operator1", "engineer", "simpleplc", "simpleplc1", "plcadmin",
    "plc12345", "scada123", "factory1", "siemens1", "schneider", "raspberry", "raspberrypi",
};

}  // namespace

std::string_view roleName(Role role) { return role == Role::Admin ? "admin" : "viewer"; }

std::optional<Role> parseRole(std::string_view name) {
    if (name == "admin") return Role::Admin;
    if (name == "viewer") return Role::Viewer;
    return std::nullopt;
}

Auth::Auth(std::filesystem::path file, std::string environmentToken)
    : file_(std::move(file)), environmentToken_(std::move(environmentToken)) {}

std::expected<void, std::string> Auth::load() {
    std::lock_guard lock(mutex_);
    users_.clear();
    tokens_.clear();
    std::error_code ec;
    if (std::filesystem::exists(file_, ec)) {
        auto text = runtime::readFile(file_);
        if (!text) return std::unexpected(text.error());
        json j = json::parse(*text, nullptr, false);
        if (!j.is_object()) return std::unexpected(std::format("{} is not valid JSON", file_.string()));
        try {
            for (const auto& u : j.value("users", json::array())) {
                users_.push_back({u.at("name").get<std::string>(),
                                  parseRole(u.value("role", "viewer")).value_or(Role::Viewer),
                                  u.at("salt").get<std::string>(), u.at("hash").get<std::string>(),
                                  u.value("iterations", kIterations), u.value("created", int64_t{0})});
            }
            for (const auto& t : j.value("tokens", json::array())) {
                TokenInfo info{t.at("id").get<std::string>(), t.value("name", ""),
                               parseRole(t.value("role", "viewer")).value_or(Role::Viewer),
                               t.value("created_by", ""), t.value("created", int64_t{0}),
                               t.value("last_used", int64_t{0})};
                if (t.contains("creator_user")) info.creatorUser = t.at("creator_user").get<std::string>();
                tokens_.push_back({info, t.at("hash").get<std::string>()});
            }
        } catch (const json::exception& e) {
            return std::unexpected(std::format("{} is damaged: {}", file_.string(), e.what()));
        }
    }
    if (users_.empty()) setupToken_ = randomHex(4) + "-" + randomHex(4);
    return {};
}

std::expected<void, std::string> Auth::saveLocked() const {
    json users = json::array();
    for (const User& u : users_) {
        users.push_back({{"name", u.name}, {"role", roleName(u.role)}, {"salt", u.salt},
                         {"hash", u.hash}, {"iterations", u.iterations}, {"created", u.createdMs}});
    }
    json tokens = json::array();
    for (const Token& t : tokens_) {
        json token = {{"id", t.info.id}, {"name", t.info.name}, {"role", roleName(t.info.role)},
                      {"created_by", t.info.createdBy}, {"created", t.info.createdMs},
                      {"last_used", t.info.lastUsedMs}, {"hash", t.hash}};
        if (t.info.creatorUser) token["creator_user"] = *t.info.creatorUser;
        tokens.push_back(token);
    }
    json j = {{"version", 1}, {"users", users}, {"tokens", tokens}};
    auto ok = runtime::writeFileAtomic(file_, j.dump(2) + "\n");
    if (ok) {
        std::error_code ec;
        std::filesystem::permissions(file_, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                     std::filesystem::perm_options::replace, ec);
        lastUsedDirty_ = false;  // the file holds every token's last_used now
    }
    return ok;
}

void Auth::flush(bool force) {
    std::lock_guard lock(mutex_);
    if (!lastUsedDirty_) return;
    auto now = Clock::now();
    if (!force && now - lastUsedSavedAt_ < kLastUsedSaveInterval) return;
    lastUsedSavedAt_ = now;
    (void)saveLocked();  // a failure is tried again at the next flush
}

std::optional<std::string> Auth::checkPassword(const std::string& password, const std::string& user) {
    size_t length = characters(password);
    if (length < 8) return "the password must have at least 8 characters";
    if (length > 256) return "the password is too long (at most 256 characters)";
    if (password.find_first_not_of(" \t") == std::string::npos) return "the password must not be only spaces";
    std::string low = lower(password);
    if (low == lower(user)) return "the password must not be the user name";
    if (std::all_of(password.begin(), password.end(), [&](char c) { return c == password[0]; })) {
        return "the password must not repeat one character";
    }
    bool sequence = true;  // 23456789, abcdefgh, 98765432
    for (int step : {1, -1}) {
        sequence = true;
        for (size_t i = 1; i < low.size() && sequence; i++) sequence = low[i] - low[i - 1] == step;
        if (sequence) break;
    }
    if (sequence || std::find(std::begin(kCommonPasswords), std::end(kCommonPasswords), low) != std::end(kCommonPasswords)) {
        return "this password is one of the most common ones, which are tried first; choose another";
    }
    return std::nullopt;
}

Auth::User Auth::makeUser(const std::string& name, const std::string& password, Role role) {
    std::vector<uint8_t> salt = crypto::randomBytes(16);
    crypto::Digest hash = crypto::pbkdf2Sha256(password, salt, kIterations);
    return {name, role, crypto::toHex(salt), crypto::toHex(hash), kIterations, epochMs()};
}

bool Auth::verify(const User& user, const std::string& password) {
    std::vector<uint8_t> salt = crypto::fromHex(user.salt);
    crypto::Digest hash = crypto::pbkdf2Sha256(password, salt, user.iterations);
    return crypto::constantTimeEqual(crypto::toHex(hash), user.hash);
}

bool Auth::needsSetup() const {
    std::lock_guard lock(mutex_);
    return users_.empty();
}

std::string Auth::setupToken() const {
    std::lock_guard lock(mutex_);
    return users_.empty() ? setupToken_ : std::string();
}

std::expected<std::string, std::string> Auth::setup(std::string_view setupToken, const std::string& user,
                                                    const std::string& password, const std::string& clientAddress) {
    if (!validUserName(user)) return std::unexpected("the user name may use letters, digits and _ - . @");
    if (auto problem = checkPassword(password, user)) return std::unexpected(*problem);
    // Check the code before the slow hash, so anonymous requests cannot burn CPU
    {
        std::lock_guard lock(mutex_);
        if (SetupCodeCheck check = checkSetupCodeLocked(setupToken, clientAddress); check.result != SetupCodeCheck::Ok) {
            return std::unexpected(check.message);
        }
    }
    User created = makeUser(user, password, Role::Admin);  // slow; outside the lock

    std::lock_guard lock(mutex_);
    if (!users_.empty()) return std::unexpected("setup is already done");  // a parallel request won
    users_.push_back(created);
    if (auto ok = saveLocked(); !ok) {
        users_.clear();
        return std::unexpected(ok.error());
    }
    setupToken_.clear();
    log::write(log::Level::Info, "web", "", std::format("administrator '{}' created", user));
    return newSessionLocked(created);
}

Auth::SetupCodeCheck Auth::checkSetupCode(std::string_view setupToken, const std::string& clientAddress) {
    std::lock_guard lock(mutex_);
    return checkSetupCodeLocked(setupToken, clientAddress);
}

Auth::SetupCodeCheck Auth::checkSetupCodeLocked(std::string_view setupToken, const std::string& clientAddress) {
    if (!users_.empty()) return {SetupCodeCheck::Done, "setup is already done"};
    // One limit for checking and for setting up: checking first must not allow guessing faster
    if (auto wait = blockedForLocked(Attempt::Password, clientAddress)) {
        return {SetupCodeCheck::Blocked, std::format("too many failed attempts; try again in {} seconds", wait)};
    }
    if (!crypto::constantTimeEqual(setupToken, setupToken_)) {
        recordFailureLocked(Attempt::Password, clientAddress);
        return {SetupCodeCheck::Wrong, kWrongSetupCode};
    }
    return {SetupCodeCheck::Ok, {}};
}

std::string Auth::newSessionLocked(const User& user) {
    auto now = Clock::now();
    std::erase_if(sessions_, [&](const auto& s) { return s.second.expires < now; });
    if (sessions_.size() >= kMaxSessions) sessions_.erase(sessions_.begin());
    std::string id = randomHex(32);
    sessions_[id] = {user.name, user.role, now + kSessionLifetime};
    return id;
}

int64_t Auth::blockedForLocked(Attempt kind, const std::string& clientAddress) {
    auto it = failures_.find({kind, clientAddress});
    auto now = Clock::now();
    if (it == failures_.end() || it->second.blockedUntil <= now) return 0;
    return std::chrono::duration_cast<std::chrono::seconds>(it->second.blockedUntil - now).count() + 1;
}

void Auth::recordFailureLocked(Attempt kind, const std::string& clientAddress) {
    auto now = Clock::now();
    if (failures_.size() > 10000) {
        std::erase_if(failures_, [&](const auto& f) { return now - f.second.first > kFailureWindow && f.second.blockedUntil <= now; });
    }
    // Generated tokens cannot be guessed; the limit protects a short SIMPLEPLC_TOKEN
    int limit = kind == Attempt::Token ? 10 : 5;
    Failures& f = failures_[{kind, clientAddress}];
    if (now - f.first > kFailureWindow) f = {0, now, {}};
    if (++f.count >= limit) {
        f.blockedUntil = now + kBlockTime;
        f.count = 0;
    }
}

int64_t Auth::blockedFor(Attempt kind, const std::string& clientAddress) {
    std::lock_guard lock(mutex_);
    return blockedForLocked(kind, clientAddress);
}

void Auth::recordFailure(Attempt kind, const std::string& clientAddress) {
    std::lock_guard lock(mutex_);
    recordFailureLocked(kind, clientAddress);
}

std::expected<std::string, std::string> Auth::login(const std::string& name, const std::string& password,
                                                    const std::string& clientAddress) {
    std::optional<User> user;
    {
        std::lock_guard lock(mutex_);
        if (auto wait = blockedForLocked(Attempt::Password, clientAddress)) {
            return std::unexpected(std::format("too many failed attempts; try again in {} seconds", wait));
        }
        // Count the attempt before the slow check: parallel requests must not all get past the limit.
        // A successful login clears the count.
        recordFailureLocked(Attempt::Password, clientAddress);
        for (const User& u : users_) {
            if (u.name == name) user = u;
        }
    }

    // Hash even for unknown users so the response time does not reveal which names exist
    bool ok = user ? verify(*user, password)
                   : (verify(User{"", Role::Viewer, "00", std::string(64, '0'), kIterations, 0}, password), false);

    std::lock_guard lock(mutex_);
    if (!ok) {
        std::string who = validUserName(name) ? std::format("'{}'", name) : std::string("an invalid user name");
        log::write(log::Level::Warn, "web", "", std::format("failed login for {} from {}", who, clientAddress));
        return std::unexpected("wrong user name or password");
    }
    failures_.erase({Attempt::Password, clientAddress});
    return newSessionLocked(*user);
}

void Auth::logout(const std::string& session) {
    std::lock_guard lock(mutex_);
    sessions_.erase(session);
}

std::optional<Principal> Auth::session(const std::string& session) {
    if (session.empty()) return std::nullopt;
    std::lock_guard lock(mutex_);
    auto it = sessions_.find(session);
    if (it == sessions_.end()) return std::nullopt;
    if (it->second.expires < Clock::now()) {
        sessions_.erase(it);
        return std::nullopt;
    }
    // The user may have been removed or demoted since login
    for (const User& u : users_) {
        if (u.name == it->second.user) return Principal{u.name, u.role, false, session, {}};
    }
    sessions_.erase(it);
    return std::nullopt;
}

std::optional<Principal> Auth::token(std::string_view bearer) {
    if (bearer.empty()) return std::nullopt;
    if (!environmentToken_.empty() && crypto::constantTimeEqual(bearer, environmentToken_)) {
        return Principal{"SIMPLEPLC_TOKEN", Role::Admin, true, {}, {}};
    }
    std::string hash = crypto::toHex(crypto::sha256(bearer));
    std::lock_guard lock(mutex_);
    for (Token& t : tokens_) {
        if (crypto::constantTimeEqual(hash, t.hash)) {
            // In memory only: a fleet polls every second with its token, and the request path must not wait
            // for a file write under this lock; flush() writes it once a minute and at shutdown
            t.info.lastUsedMs = epochMs();
            lastUsedDirty_ = true;
            return Principal{t.info.name, t.info.role, true, {}, {}};
        }
    }
    return std::nullopt;
}

bool Auth::unguessable(std::string_view bearer) const {
    if (!environmentToken_.empty() && crypto::constantTimeEqual(bearer, environmentToken_)) {
        return environmentToken_.size() >= kUnguessableLength;
    }
    return bearer.starts_with("spl_") && bearer.size() >= 52;  // generated: 192 random bits
}

std::vector<UserInfo> Auth::users() const {
    std::lock_guard lock(mutex_);
    std::vector<UserInfo> out;
    for (const User& u : users_) out.push_back({u.name, u.role, u.createdMs});
    return out;
}

std::expected<void, std::string> Auth::addUser(const std::string& name, const std::string& password, Role role) {
    if (!validUserName(name)) return std::unexpected("the user name may use letters, digits and _ - . @");
    if (auto problem = checkPassword(password, name)) return std::unexpected(*problem);
    User user = makeUser(name, password, role);
    std::lock_guard lock(mutex_);
    for (const User& u : users_) {
        if (u.name == name) return std::unexpected(std::format("user '{}' already exists", name));
    }
    users_.push_back(user);
    return saveLocked();
}

std::expected<std::vector<TokenInfo>, std::string> Auth::removeUser(const std::string& name) {
    std::lock_guard lock(mutex_);
    auto it = std::find_if(users_.begin(), users_.end(), [&](const User& u) { return u.name == name; });
    if (it == users_.end()) return std::unexpected(std::format("no user named '{}'", name));
    auto admins = std::count_if(users_.begin(), users_.end(), [](const User& u) { return u.role == Role::Admin; });
    if (it->role == Role::Admin && admins == 1) return std::unexpected("the last administrator cannot be removed");
    users_.erase(it);
    std::erase_if(sessions_, [&](const auto& s) { return s.second.user == name; });
    // Removing a person ends their access, including the tokens they created (a token created before the
    // creator was recorded goes with the user it names as its creator)
    std::vector<TokenInfo> revoked;
    std::erase_if(tokens_, [&](const Token& t) {
        bool theirs = t.info.creatorUser ? *t.info.creatorUser == name : t.info.createdBy == name;
        if (theirs) revoked.push_back(t.info);
        return theirs;
    });
    if (auto ok = saveLocked(); !ok) return std::unexpected(ok.error());
    log::write(log::Level::Info, "web", "", std::format("user '{}' removed{}", name,
                                                         revoked.empty() ? "" : std::format(" with {} token(s)", revoked.size())));
    return revoked;
}

std::expected<void, std::string> Auth::setPassword(const std::string& name, const std::string& password,
                                                   const std::string& keepSession,
                                                   const std::optional<std::string>& currentPassword) {
    if (auto problem = checkPassword(password, name)) return std::unexpected(*problem);
    if (currentPassword) {
        // Re-authentication: an unattended session must not be able to lock the owner out. Limited like
        // logins (per user), so a stolen session cannot try passwords here instead
        std::optional<User> user;
        std::string key = "user:" + name;
        {
            std::lock_guard lock(mutex_);
            if (auto wait = blockedForLocked(Attempt::Password, key)) {
                return std::unexpected(std::format("too many wrong current passwords; try again in {} seconds", wait));
            }
            recordFailureLocked(Attempt::Password, key);
            for (const User& u : users_) {
                if (u.name == name) user = u;
            }
        }
        if (!user) return std::unexpected(std::format("no user named '{}'", name));
        if (!verify(*user, *currentPassword)) return std::unexpected("the current password is not correct");
        std::lock_guard lock(mutex_);
        failures_.erase({Attempt::Password, key});
    }
    std::vector<uint8_t> salt = crypto::randomBytes(16);
    std::string hash = crypto::toHex(crypto::pbkdf2Sha256(password, salt, kIterations));
    std::lock_guard lock(mutex_);
    for (User& u : users_) {
        if (u.name == name) {
            u.salt = crypto::toHex(salt);
            u.hash = hash;
            u.iterations = kIterations;
            // Whoever knew the old password is signed out
            std::erase_if(sessions_, [&](const auto& s) { return s.second.user == name && s.first != keepSession; });
            return saveLocked();
        }
    }
    return std::unexpected(std::format("no user named '{}'", name));
}

std::vector<TokenInfo> Auth::tokens() const {
    std::lock_guard lock(mutex_);
    std::vector<TokenInfo> out;
    for (const Token& t : tokens_) out.push_back(t.info);
    return out;
}

std::expected<std::pair<TokenInfo, std::string>, std::string> Auth::createToken(const std::string& name, Role role,
                                                                               const std::string& createdBy,
                                                                               const std::string& creatorUser) {
    if (name.empty() || characters(name) > 64) return std::unexpected("give the token a name (at most 64 characters)");
    if (std::any_of(name.begin(), name.end(), [](char c) { return static_cast<unsigned char>(c) < 0x20 || c == 0x7F; })) {
        return std::unexpected("the token name must not hold control characters");
    }
    std::string secret = "spl_" + randomHex(24);
    TokenInfo info{"tok_" + randomHex(4), name, role, createdBy, epochMs(), 0, creatorUser};
    std::lock_guard lock(mutex_);
    if (tokens_.size() >= 100) return std::unexpected("at most 100 tokens; revoke unused ones first");
    tokens_.push_back({info, crypto::toHex(crypto::sha256(secret))});
    if (auto ok = saveLocked(); !ok) {
        tokens_.pop_back();
        return std::unexpected(ok.error());
    }
    return std::pair{info, secret};
}

Auth::PairingCode Auth::newPairingCode() {
    // 32 symbols without look-alikes: a random byte's low 5 bits pick one uniformly (40 bits in all)
    static constexpr std::string_view kAlphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    std::string code;
    for (uint8_t b : crypto::randomBytes(8)) code += kAlphabet[b & 31u];
    std::lock_guard lock(mutex_);
    pairing_ = {code, epochMs() + std::chrono::duration_cast<std::chrono::milliseconds>(kPairingValidity).count()};
    return pairing_;
}

std::expected<std::pair<TokenInfo, std::string>, std::pair<int, std::string>> Auth::pair(std::string_view code, const std::string& fleet,
                                                                                          const std::string& clientAddress) {
    bool plain = !fleet.empty() && fleet.size() <= 58 && std::all_of(fleet.begin(), fleet.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
    });
    if (!plain) return std::unexpected(std::pair{422, std::string("fleet must be the fleet's name: 1-58 letters, digits, _ - and .")});
    // As typed: any case, with spaces or dashes between the groups
    std::string given;
    for (char c : code) {
        if (c == ' ' || c == '-') continue;
        given += (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
    }
    {
        std::lock_guard lock(mutex_);
        if (auto wait = blockedForLocked(Attempt::Password, clientAddress)) {
            return std::unexpected(std::pair{429, std::format("too many wrong pairing codes; try again in {} seconds", wait)});
        }
        bool valid = !pairing_.code.empty() && epochMs() < pairing_.expiresMs && crypto::constantTimeEqual(given, pairing_.code);
        if (!valid) {
            recordFailureLocked(Attempt::Password, clientAddress);
            return std::unexpected(std::pair{403, std::string("the pairing code is not correct or has expired; create a new one on the "
                                                              "device (its panel, or POST /api/v1/pairing)")});
        }
        pairing_ = {};  // once
    }
    auto created = createToken("fleet:" + fleet, Role::Admin, "pairing");
    if (!created) return std::unexpected(std::pair{422, created.error()});
    return *created;
}

std::expected<TokenInfo, std::string> Auth::revokeBearer(std::string_view bearer) {
    if (!environmentToken_.empty() && crypto::constantTimeEqual(bearer, environmentToken_)) {
        return std::unexpected("this is SIMPLEPLC_TOKEN, set in the environment: it cannot be revoked here");
    }
    std::string hash = crypto::toHex(crypto::sha256(bearer));
    std::lock_guard lock(mutex_);
    for (auto it = tokens_.begin(); it != tokens_.end(); ++it) {
        if (!crypto::constantTimeEqual(hash, it->hash)) continue;
        TokenInfo info = it->info;
        tokens_.erase(it);
        if (auto ok = saveLocked(); !ok) return std::unexpected(ok.error());
        return info;
    }
    return std::unexpected("not a stored API token");
}

std::expected<void, std::string> Auth::revokeToken(const std::string& id) {
    std::lock_guard lock(mutex_);
    auto removed = std::erase_if(tokens_, [&](const Token& t) { return t.info.id == id; });
    if (removed == 0) return std::unexpected(std::format("no token with id '{}'", id));
    return saveLocked();
}

}  // namespace simpleplc::web
