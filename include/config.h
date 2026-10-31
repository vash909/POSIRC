#ifndef POSIRC_CONFIG_H
#define POSIRC_CONFIG_H

struct posirc_app;
/* Caller frees the path; NULL when HOME is unavailable or allocation fails. */
char *posirc_config_path(void);
/* Copy known keys into existing app fields; no semantic value validation.
 * Missing files are normal. Return -1 on an I/O error. */
int posirc_config_load(struct posirc_app *app, const char *path);
int posirc_config_save(const struct posirc_app *app, const char *path);

#endif
