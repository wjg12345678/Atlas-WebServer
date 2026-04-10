#include "http_conn.h"
#include "http_auth_state.h"

#include <openssl/sha.h>

using namespace std;

namespace
{
locker g_auth_cache_lock;
map<string, string> g_auth_users;
map<string, string> g_auth_sessions;

bool starts_with_ignore_case_local(const char *text, const char *prefix)
{
    return strncasecmp(text, prefix, strlen(prefix)) == 0;
}
}

locker &auth_cache_lock()
{
    return g_auth_cache_lock;
}

map<string, string> &auth_user_cache()
{
    return g_auth_users;
}

map<string, string> &auth_session_cache()
{
    return g_auth_sessions;
}

string http_conn::make_password_salt() const
{
    char salt[64];
    unsigned int seed = (unsigned int)(time(NULL) ^ m_sockfd ^ (unsigned int)getpid());
    snprintf(salt, sizeof(salt), "%08x%08x",
             (unsigned int)(rand_r(&seed) ^ (unsigned int)time(NULL)),
             (unsigned int)(rand_r(&seed) ^ (unsigned int)getpid()));
    return salt;
}

string http_conn::hash_password(const string &password, const string &salt) const
{
    string input = salt + password;
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256((const unsigned char *)input.data(), input.size(), digest);

    static const char hex_chars[] = "0123456789abcdef";
    string hex;
    hex.reserve(SHA256_DIGEST_LENGTH * 2);
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i)
    {
        hex.push_back(hex_chars[(digest[i] >> 4) & 0x0F]);
        hex.push_back(hex_chars[digest[i] & 0x0F]);
    }
    return hex;
}

string http_conn::extract_bearer_token() const
{
    if (!starts_with_ignore_case_local(m_authorization.c_str(), "Bearer "))
    {
        return "";
    }
    return trim_copy(m_authorization.substr(7));
}

bool http_conn::lookup_session(const string &token, string &username)
{
    if (mysql == NULL)
    {
        return false;
    }

    locker &cache_lock = auth_cache_lock();
    map<string, string> &session_cache = auth_session_cache();

    cache_lock.lock();
    map<string, string>::iterator session_it = session_cache.find(token);
    if (session_it != session_cache.end())
    {
        username = session_it->second;
        cache_lock.unlock();
        return true;
    }
    cache_lock.unlock();

    const string escaped_token = escape_sql_value(token);
    const string sql = "SELECT username FROM user_sessions "
                       "WHERE token='" + escaped_token + "' AND expires_at > NOW() LIMIT 1";
    if (mysql_query(mysql, sql.c_str()) != 0)
    {
        return false;
    }

    MYSQL_RES *result = mysql_store_result(mysql);
    if (result == NULL)
    {
        return false;
    }

    MYSQL_ROW row = mysql_fetch_row(result);
    if (row == NULL)
    {
        mysql_free_result(result);
        const string cleanup = "DELETE FROM user_sessions WHERE token='" + escaped_token +
                               "' OR expires_at <= NOW()";
        mysql_query(mysql, cleanup.c_str());
        return false;
    }

    username = row[0] ? row[0] : "";
    mysql_free_result(result);

    if (!username.empty())
    {
        cache_lock.lock();
        session_cache[token] = username;
        cache_lock.unlock();
        return true;
    }
    return false;
}

bool http_conn::persist_session(const string &token, const string &username, int ttl_seconds)
{
    if (mysql == NULL)
    {
        return false;
    }

    char ttl_buffer[32];
    snprintf(ttl_buffer, sizeof(ttl_buffer), "%d", ttl_seconds);
    const string sql = "INSERT INTO user_sessions(token, username, expires_at) "
                       "VALUES('" + escape_sql_value(token) + "', '" + escape_sql_value(username) +
                       "', DATE_ADD(NOW(), INTERVAL " + string(ttl_buffer) + " SECOND)) "
                       "ON DUPLICATE KEY UPDATE username=VALUES(username), expires_at=VALUES(expires_at)";
    if (mysql_query(mysql, sql.c_str()) != 0)
    {
        return false;
    }

    auth_cache_lock().lock();
    auth_session_cache()[token] = username;
    auth_cache_lock().unlock();
    return true;
}

bool http_conn::remove_session(const string &token)
{
    if (token.empty())
    {
        return false;
    }

    auth_cache_lock().lock();
    auth_session_cache().erase(token);
    auth_cache_lock().unlock();

    if (mysql == NULL)
    {
        return true;
    }

    const string sql = "DELETE FROM user_sessions WHERE token='" + escape_sql_value(token) + "'";
    return mysql_query(mysql, sql.c_str()) == 0;
}

bool http_conn::update_user_password_hash(const string &username, const string &password)
{
    if (mysql == NULL)
    {
        return false;
    }

    string salt = make_password_salt();
    string password_hash = hash_password(password, salt);
    const string sql = "UPDATE user SET passwd='" + escape_sql_value(password_hash) +
                       "', passwd_salt='" + escape_sql_value(salt) +
                       "' WHERE username='" + escape_sql_value(username) + "'";
    if (mysql_query(mysql, sql.c_str()) != 0)
    {
        return false;
    }

    auth_cache_lock().lock();
    auth_user_cache()[username] = password_hash;
    auth_cache_lock().unlock();
    return true;
}

bool http_conn::verify_user_password(const string &username, const string &password)
{
    if (mysql == NULL)
    {
        return false;
    }

    const string sql = "SELECT passwd, COALESCE(passwd_salt, '') FROM user WHERE username='" +
                       escape_sql_value(username) + "' LIMIT 1";
    if (mysql_query(mysql, sql.c_str()) != 0)
    {
        return false;
    }

    MYSQL_RES *result = mysql_store_result(mysql);
    if (result == NULL)
    {
        return false;
    }

    MYSQL_ROW row = mysql_fetch_row(result);
    if (row == NULL)
    {
        mysql_free_result(result);
        return false;
    }

    string stored_password = row[0] ? row[0] : "";
    string stored_salt = row[1] ? row[1] : "";
    mysql_free_result(result);

    if (stored_salt.empty())
    {
        if (stored_password == password)
        {
            update_user_password_hash(username, password);
            return true;
        }
        return false;
    }

    return stored_password == hash_password(password, stored_salt);
}

http_conn::HTTP_CODE http_conn::middleware_auth()
{
    if (!requires_auth())
    {
        return NO_REQUEST;
    }

    string token = extract_bearer_token();
    if (!token.empty())
    {
        if (lookup_session(token, m_current_user))
        {
            return NO_REQUEST;
        }

        if (!m_auth_token.empty() && token == m_auth_token)
        {
            m_current_user = "admin";
            return NO_REQUEST;
        }
    }

    set_memory_response(401, "Unauthorized",
                        "{\"code\":401,\"message\":\"unauthorized\"}",
                        "application/json");
    if (mysql != NULL)
    {
        write_operation_log("anonymous", "auth_failed", "request", 0, m_url ? m_url : "");
    }
    return MEMORY_REQUEST;
}

http_conn::HTTP_CODE http_conn::route_api_login()
{
    return handle_auth_request(false, true);
}

http_conn::HTTP_CODE http_conn::route_api_register()
{
    return handle_auth_request(true, true);
}

http_conn::HTTP_CODE http_conn::route_api_private_logout()
{
    return handle_logout_request();
}

http_conn::HTTP_CODE http_conn::handle_auth_request(bool is_register, bool api_mode)
{
    string name = request_value("username", "user");
    string password = request_value("passwd", "password");
    if (name.empty() || password.empty())
    {
        if (api_mode)
        {
            set_memory_response(400, "Bad Request",
                                "{\"code\":400,\"message\":\"username or password is empty\"}",
                                "application/json");
            return MEMORY_REQUEST;
        }
        return BAD_REQUEST;
    }

    bool success = false;
    if (is_register)
    {
        string password_salt = make_password_salt();
        string password_hash = hash_password(password, password_salt);
        const string sql_insert = "INSERT INTO user(username, passwd, passwd_salt) VALUES('" +
                                  escape_sql_value(name) + "', '" +
                                  escape_sql_value(password_hash) + "', '" +
                                  escape_sql_value(password_salt) + "')";

        map<string, string> &user_cache = auth_user_cache();
        locker &cache_lock = auth_cache_lock();
        if (user_cache.find(name) == user_cache.end())
        {
            cache_lock.lock();
            int res = mysql_query(mysql, sql_insert.c_str());
            if (!res)
            {
                user_cache.insert(pair<string, string>(name, password_hash));
                success = true;
                write_operation_log(name, "register", "user", 0, "register success");
            }
            cache_lock.unlock();
        }

        if (api_mode)
        {
            if (success)
            {
                set_memory_response(200, "OK",
                                    "{\"code\":0,\"message\":\"register success\"}",
                                    "application/json");
            }
            else
            {
                set_memory_response(409, "Conflict",
                                    "{\"code\":409,\"message\":\"register failed\"}",
                                    "application/json");
            }
            return MEMORY_REQUEST;
        }

        strcpy(m_url, success ? "/log.html" : "/registerError.html");
        return do_request();
    }

    success = verify_user_password(name, password);
    if (api_mode)
    {
        if (success)
        {
            string token = make_session_token(name);
            if (!persist_session(token, name, 7 * 24 * 3600))
            {
                return INTERNAL_ERROR;
            }
            m_current_user = name;
            write_operation_log(name, "login", "user", 0, "login success");

            string response = string("{\"code\":0,\"message\":\"login success\",\"target\":\"/welcome.html\",\"token\":\"") +
                              json_escape(token) + "\",\"expires_in\":604800}";
            set_memory_response(200, "OK", response, "application/json");
        }
        else
        {
            set_memory_response(401, "Unauthorized",
                                "{\"code\":401,\"message\":\"login failed\"}",
                                "application/json");
            write_operation_log(name, "login_failed", "user", 0, "invalid username or password");
        }
        return MEMORY_REQUEST;
    }

    strcpy(m_url, success ? "/welcome.html" : "/logError.html");
    return do_request();
}

http_conn::HTTP_CODE http_conn::handle_logout_request()
{
    string token = extract_bearer_token();
    if (token.empty())
    {
        set_memory_response(400, "Bad Request",
                            "{\"code\":400,\"message\":\"missing bearer token\"}",
                            "application/json");
        return MEMORY_REQUEST;
    }

    remove_session(token);
    if (!m_current_user.empty() && m_current_user != "admin")
    {
        write_operation_log(m_current_user, "logout", "user", 0, "logout success");
    }
    set_memory_response(200, "OK",
                        "{\"code\":0,\"message\":\"logout success\"}",
                        "application/json");
    return MEMORY_REQUEST;
}
