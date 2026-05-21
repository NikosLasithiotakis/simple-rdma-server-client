#ifndef UTILS_H
#define UTILS_H

#include <stddef.h>

#define CONFIG_FILE "../config.yml"
#define MSG_SIZE 64

void get_config_from_yaml(const char *filename, char *ip_buf, size_t max_ip_len, char *port_buf, size_t max_port_len);

#endif // UTILS_H
