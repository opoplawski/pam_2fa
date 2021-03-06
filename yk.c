#ifdef HAVE_CONFIG_H
    #include "config.h"
#endif

#include <ykclient.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "pam_2fa.h"

int yk_auth_func(pam_handle_t * pamh, user_config * user_cfg, module_config * cfg, const char *otp, void * data);

const auth_mod yk_auth = {
    .pre_auth = NULL,
    .do_auth = &yk_auth_func,
    .name = "Yubikey",
    .prompt = "Yubikey: ",
    .otp_len = YK_OTP_LEN,
};

int yk_auth_func(pam_handle_t * pamh, user_config * user_cfg, module_config * cfg, const char *otp, void * data) {
    ykclient_t *ykc = NULL;
    int retval = 0;

    if (otp == NULL) {
        DBG(("Module error: auth  called without an otp"));
        return PAM_AUTH_ERR;
    }

    retval = ykclient_init(&ykc);
    if (retval != YKCLIENT_OK) {
	DBG(("ykclient_init() failed (%d): %s", retval,
	     ykclient_strerror(retval)));
	return PAM_AUTH_ERR;
    }

    retval = ykclient_set_client_hex(ykc, cfg->yk_id, cfg->yk_key);
    if (retval != YKCLIENT_OK) {
	DBG(("ykclient_set_client_b64() failed (%d): %s", retval,
	     ykclient_strerror(retval)));
        // cleanup
        ykclient_done(&ykc);
	return PAM_AUTH_ERR;
    }

    if (cfg->yk_key)
	ykclient_set_verify_signature(ykc, 1);

    if (cfg->capath)
	ykclient_set_ca_path(ykc, cfg->capath);

    if (cfg->yk_uri)
	ykclient_set_url_template(ykc, cfg->yk_uri);

    retval = PAM_AUTH_ERR;

    DBG(("Yubikey = %s", otp));
    pam_syslog(pamh, LOG_DEBUG, "Yubikey OTP: %s (%zu)", otp, strlen(otp));

    // VERIFY IF VALID INPUT !
    if (strlen(otp) != YK_OTP_LEN) {
        DBG(("INCORRRECT code from user!"));
        pam_syslog(pamh, LOG_INFO, "Yubikey OTP is incorrect: %s", otp);
        ykclient_done(&ykc);
        return PAM_AUTH_ERR;
    }

    int keyNotFound = 1;
    if (user_cfg->yk_publicids) {
        char **publicid = NULL;
        for(publicid = user_cfg->yk_publicids;
                keyNotFound && *publicid;
                ++publicid) {
            keyNotFound = strncmp(otp, *publicid, YK_PUBLICID_LEN);
        }
    }

    if (keyNotFound) {
        DBG(("INCORRECT yubikey public ID"));
        pam_syslog(pamh, LOG_INFO, "Yubikey OTP doesn't match user public ids");
        ykclient_done(&ykc);
        return PAM_AUTH_ERR;
    }

    ykclient_rc yk_server_retval = ykclient_request(ykc, otp);
    DBG(("ykclient return value (%d): %s",
         yk_server_retval, ykclient_strerror(yk_server_retval)));

    if (yk_server_retval == YKCLIENT_OK) {
        retval = PAM_SUCCESS;
    } else {
        pam_syslog(pamh, LOG_INFO, "Yubikey server response: %s (%d)", ykclient_strerror(yk_server_retval), yk_server_retval);
        pam_prompt(pamh, PAM_ERROR_MSG, NULL, "%s", ykclient_strerror(yk_server_retval));
    }

    ykclient_done(&ykc);
    return retval;
}

int yk_load_user_file(pam_handle_t *pamh, const module_config *cfg, struct passwd *user_entry, char ***user_publicids)
{
    int fd, retval;
    ssize_t bytes_read = 0;
    size_t yk_id_pos = 0, yk_id_len = 0;
    char *filename;
    char buf[2048];
    char *buf_pos = NULL, *buf_next_line = NULL;
    char **yk_publicids = NULL;
    size_t buf_len = 0, buf_remaining_len = 0;

    if (asprintf(&filename, "%s/%s", user_entry->pw_dir, cfg->yk_user_file) < 0) {
        pam_syslog(pamh, LOG_DEBUG, "Can't allocate filename buffer");
        return ERROR;
    }

    {
      // check the exitence of the file
      struct stat st;
      retval = stat(filename, &st);
      if(retval < 0) {
        pam_syslog(pamh, LOG_ERR, "Can't get stats of file '%s'", filename);
        free(filename);
        return ERROR;
      }
      if(!S_ISREG(st.st_mode)) {
        pam_syslog(pamh, LOG_ERR, "Not a regular file '%s'", filename);
        free(filename);
        return ERROR;
      }
    }

    fd = open(filename, O_RDONLY);
    if(fd < 0) {
        pam_syslog(pamh, LOG_ERR, "Can't open file '%s'", filename);
        free(filename);
        return ERROR;
    }
    free(filename);

    buf_pos = buf;
    buf_len = sizeof(buf) / sizeof(char);
    buf_remaining_len = 0;

    while((bytes_read = read(fd, buf_pos, buf_len - buf_remaining_len)) > 0) {
        buf[bytes_read] = 0;
        buf_pos = buf;

        while((buf_next_line = strchr(buf_pos, '\n'))) {
            *(buf_next_line) = 0;
            buf_next_line++;
            retval = yk_get_publicid(pamh, buf_pos, &yk_id_pos, &yk_id_len, &yk_publicids);
            if(retval != OK) {
                yk_free_publicids(yk_publicids);
                return ERROR;
            }

            buf_pos = buf_next_line;
        }

        buf_remaining_len = strlen(buf_pos);
        memmove(buf, buf_pos, buf_remaining_len + 1);
        buf_pos = buf + buf_remaining_len;
    }

    if(buf_remaining_len) {
        retval = yk_get_publicid(pamh, buf_pos, &yk_id_pos, &yk_id_len, &yk_publicids);
        if(retval != OK) {
            yk_free_publicids(yk_publicids);
            return ERROR;
        }
    }

    *user_publicids = yk_publicids;
    return OK;
}

void yk_free_publicids(char **publicids)
{
    if(publicids) {
        char **yk_id_p;
        for(yk_id_p = publicids; *yk_id_p; ++yk_id_p)
            free(*yk_id_p);

        free(publicids);
    }
}

int yk_get_publicid(pam_handle_t *pamh, char *buf, size_t *yk_id_pos, size_t *yk_id_len, char ***yk_publicids)
{
    if(buf[0] != '#') {
        if(strlen(buf) >= YK_PUBLICID_LEN &&
            (buf[YK_PUBLICID_LEN] == 0    ||
             buf[YK_PUBLICID_LEN] == '\r' ||
             buf[YK_PUBLICID_LEN] == ' '  ||
             buf[YK_PUBLICID_LEN] == '\t' ||
             buf[YK_PUBLICID_LEN] == '#')) {

            if (!*yk_id_len || *yk_id_pos == *yk_id_len - 1) {
                *yk_id_len += YK_IDS_DEFAULT_SIZE;
                *yk_publicids = (char **) realloc (*yk_publicids, *yk_id_len * sizeof(char *));
                if (!*yk_publicids) {
                    return ERROR;
                }
            }
                    
            (*yk_publicids)[*yk_id_pos] = (char *) calloc(YK_PUBLICID_LEN + 1, sizeof(char));
            if (!(*yk_publicids)[*yk_id_pos]) {
                return ERROR;
            }

            buf[YK_PUBLICID_LEN] = 0;
            strncpy((*yk_publicids)[*yk_id_pos], buf, YK_PUBLICID_LEN + 1);
            ++(*yk_id_pos);
            (*yk_publicids)[*yk_id_pos] = NULL;

        } else {
            pam_syslog(pamh, LOG_WARNING, "Invalid yubikey public id: %s", buf);
        }
    }

    return OK;   
}
