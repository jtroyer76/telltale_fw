#include "auth.h"

#include <string.h>
#include "settings.h"

// Same time whatever the input, so the password can't be guessed a character at a time.
bool auth_password_ok(const char *given)
{
    settings_t s;
    settings_get(&s);
    const char *want = s.password;
    size_t n = strlen(want);
    if (!n || strlen(given) != n) return false;
    unsigned diff = 0;
    for (size_t i = 0; i < n; i++) diff |= (unsigned char)given[i] ^ (unsigned char)want[i];
    return diff == 0;
}

bool auth_check(httpd_req_t *req)
{
    char pass[64] = "";
    if (httpd_req_get_hdr_value_str(req, "X-OTA-Password", pass, sizeof(pass)) == ESP_OK && auth_password_ok(pass)) {
        return true;
    }
    httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "bad password");
    return false;
}
