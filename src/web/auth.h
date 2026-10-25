#pragma once
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace simpleplc::web {

enum class Role { Viewer, Admin };

/// What a client tried; each kind has its own failure count.
enum class Attempt { Password, Token };

std::string_view roleName(Role role);
std::optional<Role> parseRole(std::string_view name);

/// The header a fleet sends with each request it relays: who asked the fleet. A device records it in
/// its events next to the token the request signed in with; it grants nothing.
inline constexpr const char* kOnBehalfHeader = "X-SimplePLC-For";

/// Who is making a request.
struct Principal {
    std::string name;    ///< user name, or the token name for API tokens (free text, not a user)
    Role role = Role::Viewer;
    bool token = false;  ///< authenticated with a bearer token (no CSRF check needed)
    std::string session; ///< the session id when signed in with a cookie
    std::string onBehalfOf;  ///< for an admin token relaying for someone (a fleet): who asked it; for events only
};

struct UserInfo {
    std::string name;
    Role role;
    int64_t createdMs;
};

struct TokenInfo {
    std::string id;     ///< public identifier, e.g. "tok_3f2a9c1d"
    std::string name;
    Role role;
    std::string createdBy;  ///< who created it: a user name, or a token's name (free text)
    int64_t createdMs;
    int64_t lastUsedMs;
    /// The signed-in user who created it (empty: another token did), so removing that user revokes it.
    /// Not known for tokens created before it was recorded: those go with the user named createdBy.
    std::optional<std::string> creatorUser = std::nullopt;
};

/**
 * Users, sessions and API tokens, persisted in users.json.
 *
 * - Passwords: PBKDF2-HMAC-SHA256 with a random salt.
 * - API tokens: shown once at creation; only their SHA-256 hash is stored.
 * - Sessions: random ids held in memory (a restart signs everybody out).
 * - First run: until a user exists, a one-time setup token printed on the
 *   console is required to create the first administrator.
 * - Failures count per client address: after 5 wrong passwords or setup codes
 *   (or 10 wrong bearer tokens) in 10 minutes, that address waits 30 seconds.
 *   A correct token that cannot be guessed (a generated one, or a SIMPLEPLC_TOKEN of
 *   at least 16 characters) still works from a waiting address: one misconfigured
 *   client must not lock out the others behind the same address.
 * - Passwords: at least 8 characters, not the user name, not a very common password.
 *   Changing your own needs the current one.
 * - Removing a user revokes the tokens that user created.
 * - SIMPLEPLC_TOKEN: an administrator token from the environment, for scripts
 *   and agents; it is never written to disk.
 *
 * Thread-safe.
 */
class Auth {
public:
    Auth(std::filesystem::path file, std::string environmentToken);

    std::expected<void, std::string> load();

    bool needsSetup() const;
    /// The one-time setup token; empty when setup is done.
    std::string setupToken() const;

    /// What checking a setup code found.
    struct SetupCodeCheck {
        enum Result { Ok, Done, Blocked, Wrong } result = Ok;
        std::string message;  ///< why not, for the user
    };
    static constexpr const char* kWrongSetupCode =
        "the setup code is not correct (it is printed in the console where SimplePLC runs)";
    /// Checks the setup code without using it (the panel asks for it before the account). Wrong codes
    /// count against the same per-address limit as setup().
    SetupCodeCheck checkSetupCode(std::string_view setupToken, const std::string& clientAddress);
    std::expected<std::string, std::string> setup(std::string_view setupToken, const std::string& user,
                                                  const std::string& password, const std::string& clientAddress);

    /// Returns a session id.
    std::expected<std::string, std::string> login(const std::string& user, const std::string& password,
                                                  const std::string& clientAddress);
    void logout(const std::string& session);
    std::optional<Principal> session(const std::string& session);
    /// Records the use (last_used, in memory) and never writes a file: a request must not wait for the disk.
    std::optional<Principal> token(std::string_view bearer);
    /// Writes users.json when a token's last_used is newer than the file's: at most once a minute, or at
    /// once with `force` (shutdown). Called from the main thread's wait loop, not from a request.
    void flush(bool force = false);
    /// True for a bearer value nobody can guess by trying (a generated token, or a SIMPLEPLC_TOKEN of
    /// 16 or more characters): it may be accepted from an address that is waiting after failures.
    bool unguessable(std::string_view bearer) const;

    /// Seconds an address must wait after too many failures; 0 when it may try.
    int64_t blockedFor(Attempt kind, const std::string& clientAddress);
    void recordFailure(Attempt kind, const std::string& clientAddress);

    std::vector<UserInfo> users() const;
    std::expected<void, std::string> addUser(const std::string& name, const std::string& password, Role role);
    /// Removes the user, signs them out and revokes the tokens they created; returns those tokens.
    std::expected<std::vector<TokenInfo>, std::string> removeUser(const std::string& name);
    /// Also signs the user out everywhere except `keepSession` (the session making the change). With
    /// `currentPassword`, refuses unless it is the user's current password (limited like logins).
    std::expected<void, std::string> setPassword(const std::string& name, const std::string& password,
                                                 const std::string& keepSession = {},
                                                 const std::optional<std::string>& currentPassword = std::nullopt);

    std::vector<TokenInfo> tokens() const;
    /// Returns the token's info and its secret (shown to the user once). `creatorUser`: the signed-in
    /// user creating it (empty when a token does), whose removal revokes it.
    std::expected<std::pair<TokenInfo, std::string>, std::string> createToken(const std::string& name, Role role,
                                                                             const std::string& createdBy,
                                                                             const std::string& creatorUser = {});
    std::expected<void, std::string> revokeToken(const std::string& id);

    /// Adoption by a fleet: an administrator asks for a code and gives it to the fleet, which trades it for an
    /// admin token named "fleet:<fleet>" (listed and revocable like any token). A code has 8 characters
    /// without look-alikes (no 0/O, 1/l/I), is valid for 10 minutes and once; a new one replaces the old one.
    struct PairingCode {
        std::string code;
        int64_t expiresMs = 0;  ///< Unix epoch milliseconds
    };
    PairingCode newPairingCode();
    /// Trades the pairing code for the fleet's token (its info and secret). Wrong codes count against the same
    /// per-address limit as setup codes and passwords. Errors: (HTTP status, message).
    std::expected<std::pair<TokenInfo, std::string>, std::pair<int, std::string>> pair(std::string_view code, const std::string& fleet,
                                                                                        const std::string& clientAddress);
    /// Revokes the stored token whose secret is `bearer` (a fleet leaving with its own token).
    std::expected<TokenInfo, std::string> revokeBearer(std::string_view bearer);

    static constexpr uint32_t kIterations = 210000;
    static constexpr auto kPairingValidity = std::chrono::minutes(10);
    /// How often flush() writes last_used while tokens are in use.
    static constexpr auto kLastUsedSaveInterval = std::chrono::minutes(1);

private:
    struct User {
        std::string name;
        Role role;
        std::string salt;   // hex
        std::string hash;   // hex
        uint32_t iterations;
        int64_t createdMs;
    };
    struct Token {
        TokenInfo info;
        std::string hash;   // hex SHA-256 of the secret
    };
    struct Session {
        std::string user;
        Role role;
        std::chrono::steady_clock::time_point expires;
    };
    struct Failures {
        int count = 0;
        std::chrono::steady_clock::time_point first;
        std::chrono::steady_clock::time_point blockedUntil;
    };

    std::expected<void, std::string> saveLocked() const;
    std::string newSessionLocked(const User& user);
    SetupCodeCheck checkSetupCodeLocked(std::string_view setupToken, const std::string& clientAddress);
    int64_t blockedForLocked(Attempt kind, const std::string& clientAddress);
    void recordFailureLocked(Attempt kind, const std::string& clientAddress);
    static std::optional<std::string> checkPassword(const std::string& password, const std::string& user);
    static User makeUser(const std::string& name, const std::string& password, Role role);
    static bool verify(const User& user, const std::string& password);

    std::filesystem::path file_;
    std::string environmentToken_;
    mutable std::mutex mutex_;
    std::vector<User> users_;
    std::vector<Token> tokens_;
    std::map<std::string, Session> sessions_;
    std::map<std::pair<Attempt, std::string>, Failures> failures_;
    std::string setupToken_;
    PairingCode pairing_;  // empty code: none
    mutable bool lastUsedDirty_ = false;  // a token's last_used is newer than the file (cleared by saveLocked)
    std::chrono::steady_clock::time_point lastUsedSavedAt_{};
};

}  // namespace simpleplc::web
